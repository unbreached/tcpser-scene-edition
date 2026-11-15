#include <stdio.h>
#include <sys/select.h>   // fd_set, select (not pulled in by other headers on musl)

#include <sys/socket.h>   // for recv...
#include <unistd.h>       // for read...
#include <stdlib.h>       // for exit...
#include <sys/param.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <arpa/inet.h>    // inet_pton

#include "util.h"
#include "debug.h"
#include "nvt.h"
#include "modem_core.h"
#include "ip.h"
#include "getcmd.h"

#include "bridge.h"
#include "dashboard.h"
#include "connlog.h"
#include "webhook.h"
#include "secure_script.h"
#include "secure_paths.h"

const char MDM_NO_ANSWER[] = "NO ANSWER\n";

/* --- Simple IP-based gate fail blocklist (in-memory) -------------------- */
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

typedef struct {
  char ip[INET_ADDRSTRLEN];
  int fails;
  time_t blocked_until;       /* monotonic seconds; 0 = no timed block */
  int permanent;
  time_t last_attempt;        /* monotonic; for rate limiting */
  int fast_hits;              /* connects that tripped the rate limit, in a row */
} ip_block_t;

#define RATE_LIMIT_SECONDS 5  /* minimum seconds between attempts from same IP */
#define FLOOD_HITS 5          /* this many rate-limited connects in a row ... */
#define FLOOD_BLOCK_SECONDS 600 /* ... earn a timed block (not written to the file) */
#define GATE_MAX_GUESSES_PER_CONN 3  /* wrong password tries before we drop the connection */
/* ct_streq() (constant-time compare) now lives in util.c */

/* The table grows on demand. It holds permanent blocks (mirrored in the
 * blocklist file), timed blocks and rate-limit tracking. It used to be a
 * fixed 512 slots: blocklist-file lines beyond that were silently ignored. */
#define BLOCKS_INITIAL 512
#define BLOCKS_LIMIT   65536
static ip_block_t *g_blocks = NULL;
static int g_blocks_cap = 0;
static int g_blocks_inited = 0;
static pthread_mutex_t g_blocks_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t blocks_once = PTHREAD_ONCE_INIT;

static void init_blocks_once(void) {
  g_blocks = (ip_block_t *)calloc(BLOCKS_INITIAL, sizeof(ip_block_t));
  if (!g_blocks) {
    LOG(LOG_FATAL, "Could not allocate blocklist memory");
    exit(-1);
  }
  g_blocks_cap = BLOCKS_INITIAL;
  g_blocks_inited = 1;
  LOG(LOG_DEBUG, "IP blocks initialized successfully");
}

static void ensure_blocks_init(void) {
  pthread_once(&blocks_once, init_blocks_once);
}

/* IP allowlist — trusted IPs that skip the gate.
 * One IP per line, same format as blocklist.
 * Set TCPSER_ALLOWLIST env var or use data/tcpser.allowlist.
 */
static const char* allowlist_path(void) {
  const char *env_path = getenv("TCPSER_ALLOWLIST");
  if (env_path && env_path[0]) {
    /* Validate environment path */
    static __thread char safe_path[512];   /* thread-local: called from several threads */
    if (validate_file_path(env_path, safe_path, sizeof(safe_path)) == 0) {
      return safe_path;
    }
    LOG(LOG_WARN, "Invalid allowlist path in TCPSER_ALLOWLIST: %s", env_path);
  }

  /* Use secure fallback paths */
  if(access("data/tcpser.allowlist", F_OK) == 0) return "data/tcpser.allowlist";
  return "tcpser.allowlist";
}

/* In-memory allowlist cache. The allowlist was re-opened and fully scanned on
 * every connection; cache it and reload only when the file's mtime changes
 * (checked at most every 10s), mirroring the blocklist. Edits take effect
 * within ~10s instead of instantly — the same trade-off the blocklist makes. */
#define MAX_ALLOW 256
static char g_allow[MAX_ALLOW][INET_ADDRSTRLEN];
static int  g_allow_count = 0;
static time_t g_allow_last_check = 0;
static time_t g_allow_mtime = 0;
static pthread_mutex_t g_allow_mutex = PTHREAD_MUTEX_INITIALIZER;

static void allowlist_load_if_needed(void) {
  time_t now = mono_now();
  if(g_allow_last_check != 0 && (now - g_allow_last_check) < 10) return;
  g_allow_last_check = now;

  const char *path = allowlist_path();
  struct stat st;
  if(stat(path, &st) != 0) { g_allow_count = 0; g_allow_mtime = 0; return; }
  if(g_allow_mtime != 0 && st.st_mtime == g_allow_mtime) return;
  g_allow_mtime = st.st_mtime;

  FILE *f = fopen(path, "r");
  if(!f) { g_allow_count = 0; return; }
  g_allow_count = 0;
  char line[256];
  while(fgets(line, sizeof(line), f) && g_allow_count < MAX_ALLOW) {
    char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == '\n' || *p == '\0') continue;
    size_t n = strlen(p);
    while(n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';
    if(p[0] == 0 || strlen(p) >= INET_ADDRSTRLEN) continue;
    snprintf(g_allow[g_allow_count], INET_ADDRSTRLEN, "%s", p);
    g_allow_count++;
  }
  fclose(f);
}

static int ip_is_allowed(const char *ip) {
  if(!ip || !ip[0]) return FALSE;
  int result = FALSE;
  pthread_mutex_lock(&g_allow_mutex);
  allowlist_load_if_needed();
  for(int i = 0; i < g_allow_count; i++) {
    if(strcmp(g_allow[i], ip) == 0) { result = TRUE; break; }
  }
  pthread_mutex_unlock(&g_allow_mutex);
  return result;
}

int allowlist_has(const char *ip) { return ip_is_allowed(ip); }

int allowlist_list(char out[][16], int max) {
  int n = 0;
  pthread_mutex_lock(&g_allow_mutex);
  allowlist_load_if_needed();
  for(; n < g_allow_count && n < max; n++) snprintf(out[n], 16, "%.15s", g_allow[n]);
  pthread_mutex_unlock(&g_allow_mutex);
  return n;
}

/* the cache must be re-read after we changed the file ourselves */
static void allowlist_invalidate(void) { g_allow_last_check = 0; g_allow_mtime = 0; }

static int is_ipv4(const char *ip) {
  struct in_addr a;
  return ip && strlen(ip) < INET_ADDRSTRLEN && inet_pton(AF_INET, ip, &a) == 1;
}

/* Append one address. The file is created 0600 when it does not exist yet;
 * comments and the operator's own lines are left alone. */
int allowlist_add(const char *ip) {
  if(!is_ipv4(ip)) return -1;
  int rc = 0;
  pthread_mutex_lock(&g_allow_mutex);
  allowlist_invalidate();
  allowlist_load_if_needed();
  for(int i = 0; i < g_allow_count; i++) if(strcmp(g_allow[i], ip) == 0) rc = 1;
  if(rc == 0 && g_allow_count >= MAX_ALLOW) rc = -3;
  if(rc == 0) {
    int fd = open(allowlist_path(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if(fd < 0) rc = -2;
    else {
      char line[INET_ADDRSTRLEN + 2], last = '\n';
      off_t size = lseek(fd, 0, SEEK_END);
      if(size > 0 && pread(fd, &last, 1, size - 1) != 1) last = '\n';
      int n = snprintf(line, sizeof(line), "%s%s\n", last == '\n' ? "" : "\n", ip);
      if(write(fd, line, (size_t)n) != n) rc = -2;
      close(fd);
    }
  }
  allowlist_invalidate();
  pthread_mutex_unlock(&g_allow_mutex);
  return rc;
}

/* Rewrite the file without the address (temp file + rename, so a crash never
 * leaves half a list). Everything else in the file is kept as it was. */
int allowlist_remove(const char *ip) {
  if(!is_ipv4(ip)) return 1;
  int rc = 1;
  pthread_mutex_lock(&g_allow_mutex);
  const char *path = allowlist_path();
  char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *in = fopen(path, "r");
  if(in) {
    struct stat st; mode_t mode = 0600;
    if(fstat(fileno(in), &st) == 0) mode = st.st_mode & 0777;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if(!out) { if(fd >= 0) close(fd); rc = -2; }
    else {
      char line[256], key[256];
      while(fgets(line, sizeof(line), in)) {
        char *p = line; while(*p == ' ' || *p == '\t') p++;
        snprintf(key, sizeof(key), "%s", p);
        size_t n = strlen(key);
        while(n > 0 && (key[n-1] == '\n' || key[n-1] == '\r' || key[n-1] == ' ')) key[--n] = 0;
        if(strcmp(key, ip) == 0) { rc = 0; continue; }
        fputs(line, out);
      }
      int bad = ferror(out) | fchmod(fileno(out), mode);
      if(fclose(out) != 0) bad = 1;
      if(bad || rc != 0) { unlink(tmp); if(bad) rc = -2; }
      else if(rename(tmp, path) != 0) { unlink(tmp); rc = -2; }
    }
    fclose(in);
  }
  allowlist_invalidate();
  pthread_mutex_unlock(&g_allow_mutex);
  return rc;
}

/* Persistent blocklist file.
 * - Permanent blocks are stored in a newline-delimited file (IPv4 dotted).
 * - The file is loaded on demand and reloaded if it changes on disk.
 * - Set TCPSER_BLOCKLIST env var to override the path.
 */
static time_t g_blocklist_last_check = 0;
static time_t g_blocklist_mtime = 0;

static const char* blocklist_path(void) {
  const char *env_path = getenv("TCPSER_BLOCKLIST");
  if (env_path && env_path[0]) {
    /* Validate environment path */
    static __thread char safe_path[512];   /* thread-local: called from several threads */
    if (validate_file_path(env_path, safe_path, sizeof(safe_path)) == 0) {
      return safe_path;
    }
    LOG(LOG_WARN, "Invalid blocklist path in TCPSER_BLOCKLIST: %s", env_path);
  }

  /* Use secure fallback paths */
  if(access("data/tcpser.blocklist", F_OK) == 0) return "data/tcpser.blocklist";
  return "tcpser.blocklist";
}

static void trim_line(char *s) {
  if(!s) return;
  size_t n = strlen(s);
  while(n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t')) {
    s[n-1] = 0; n--;
  }
  /* trim leading whitespace */
  char *p = s;
  while(*p == ' ' || *p == '\t') p++;
  if(p != s) memmove(s, p, strlen(p) + 1);
}

/* forward decl: blocklist loader uses it */
static ip_block_t* find_or_alloc_ip(const char *ip);

/* Append one IP to the blocklist file. The caller only does this when the IP
 * was not already permanent in memory (which mirrors the file), so the file is
 * not re-read to look for duplicates: that made every new ban cost a full scan
 * of a file that a botnet can grow without limit. */
static void blocklist_append_ip(const char *ip) {
  if(!ip || !ip[0]) return;

  int wfd = open(blocklist_path(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if(wfd < 0) return;
  set_cloexec(wfd);
  if(flock(wfd, LOCK_EX) != 0) {
    close(wfd);
    return;
  }
  FILE *f = fdopen(wfd, "a");
  if(!f) {
    close(wfd);
    return;
  }
  fprintf(f, "%s\n", ip);
  fflush(f);
  fclose(f); /* also closes wfd */
}

static void blocklist_load_if_needed(void) {
  /* Reload at most every 10 seconds, and only if file mtime changes. */
  time_t now = mono_now();
  if(g_blocklist_last_check != 0 && (now - g_blocklist_last_check) < 10) return;
  g_blocklist_last_check = now;

  struct stat st;
  if(stat(blocklist_path(), &st) != 0) {
    /* File missing/removed: do NOT wipe in-memory permanent blocks. Deleting
     * the file must not silently unblock every banned IP (an attacker who can
     * remove the file, or an accidental wrong CWD, would otherwise clear all
     * bans). Operators clear a block by editing the file (which still exists);
     * the next append re-creates it. */
    g_blocklist_mtime = 0;
    return;
  }
  if(g_blocklist_mtime != 0 && st.st_mtime == g_blocklist_mtime) {
    return;
  }
  g_blocklist_mtime = st.st_mtime;

  /* Authoritative reload: clear permanent flags before re-reading file. */
  for(int i = 0; i < g_blocks_cap; i++) {
    if(g_blocks[i].ip[0] && g_blocks[i].permanent == TRUE) {
      g_blocks[i].permanent = FALSE;
    }
  }

  FILE *f = fopen(blocklist_path(), "r");
  if(!f) return;

  char line[256];
  int skipped = 0;
  while(fgets(line, sizeof(line), f)) {
    trim_line(line);
    if(line[0] == 0) continue;
    if(line[0] == '#') continue;
    /* Basic sanity: dotted IPv4 fits in INET_ADDRSTRLEN */
    if(strlen(line) >= INET_ADDRSTRLEN) continue;
    ip_block_t *e = find_or_alloc_ip(line);
    if(e) {
      e->permanent = TRUE;
      e->blocked_until = 0;
      e->fails = 0;
    } else skipped++;
  }
  fclose(f);
  if(skipped)
    LOG(LOG_ERROR, "Blocklist file has more than %d entries: %d were NOT loaded and are not blocked", BLOCKS_LIMIT, skipped);
}

static ip_block_t* find_or_alloc_ip(const char *ip) {
  ensure_blocks_init();
  int free_i = -1;
  int oldest_i = -1;
  time_t oldest_t = 0;
  for(int i = 0; i < g_blocks_cap; i++) {
    if(g_blocks[i].ip[0] == 0 && free_i == -1) free_i = i;
    if(ip && ip[0] && 0 == strcmp(g_blocks[i].ip, ip)) return &g_blocks[i];
    /* Track the oldest non-permanent slot as an eviction candidate. */
    if(g_blocks[i].ip[0] && !g_blocks[i].permanent) {
      if(oldest_i == -1 || g_blocks[i].last_attempt < oldest_t) {
        oldest_i = i;
        oldest_t = g_blocks[i].last_attempt;
      }
    }
  }
  if(!ip || !ip[0]) return NULL;

  if(free_i == -1 && g_blocks_cap < BLOCKS_LIMIT) {
    /* grow (the mutex is held by every caller, so no pointer is live) */
    int ncap = g_blocks_cap * 2;
    ip_block_t *nb = (ip_block_t *)realloc(g_blocks, (size_t)ncap * sizeof(ip_block_t));
    if(nb) {
      memset(nb + g_blocks_cap, 0, (size_t)(ncap - g_blocks_cap) * sizeof(ip_block_t));
      free_i = g_blocks_cap;
      g_blocks = nb;
      g_blocks_cap = ncap;
    }
  }
  if(free_i == -1) {
    /* At the hard limit. Evict the oldest timed/rate-limit entry so we keep
     * tracking new offenders. Permanent blocks are never evicted. */
    if(oldest_i != -1) {
      LOG(LOG_WARN, "IP table full (%d), evicting oldest entry %s to track %s",
          g_blocks_cap, g_blocks[oldest_i].ip, ip);
      free_i = oldest_i;
    } else {
      LOG(LOG_WARN, "IP table full (%d, all permanent), cannot track %s",
          g_blocks_cap, ip);
      return NULL;
    }
  }

  memset(&g_blocks[free_i], 0, sizeof(g_blocks[free_i]));
  snprintf(g_blocks[free_i].ip, sizeof(g_blocks[free_i].ip), "%s", ip);
  return &g_blocks[free_i];
}

static int ip_is_blocked(const char *ip) {
  int result = FALSE;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  blocklist_load_if_needed();
  ip_block_t *e = find_or_alloc_ip(ip);
  if(e) {
    if(e->permanent == TRUE) result = TRUE;
    else if(e->blocked_until != 0 && mono_now() < e->blocked_until) result = TRUE;
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return result;
}

static void ip_note_fail(const char *ip, int max_fails, int block_seconds) {
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  ip_block_t *e = find_or_alloc_ip(ip);
  if(!e) { pthread_mutex_unlock(&g_blocks_mutex); return; }
  if(e->permanent == TRUE) { pthread_mutex_unlock(&g_blocks_mutex); return; }
  e->fails++;
  if(e->fails >= max_fails) {
    e->permanent = TRUE;
    e->blocked_until = 0;
    e->fails = 0;
    pthread_mutex_unlock(&g_blocks_mutex);
    blocklist_append_ip(ip); /* file I/O outside lock */
    webhook_notify("IP_BLOCKED", ip);
    dashboard_note("blocked %s after repeated gate failures", ip);
    (void)block_seconds;
    return;
  }
  pthread_mutex_unlock(&g_blocks_mutex);
}

/* Count currently blocked IPs (permanent + timed) */
int blocklist_count(void) {
  int count = 0;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  blocklist_load_if_needed();
  time_t now = mono_now();
  for(int i = 0; i < g_blocks_cap; i++) {
    if(g_blocks[i].ip[0] == 0) continue;
    if(g_blocks[i].permanent) { count++; continue; }
    if(g_blocks[i].blocked_until != 0 && now < g_blocks[i].blocked_until) count++;
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return count;
}

/* List currently blocked IPs into caller-provided array. Returns count. */
int blocklist_list(char buf[][16], int max) {
  int count = 0;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  blocklist_load_if_needed();
  time_t now = mono_now();
  for(int i = 0; i < g_blocks_cap && count < max; i++) {
    if(g_blocks[i].ip[0] == 0) continue;
    if(g_blocks[i].permanent ||
       (g_blocks[i].blocked_until != 0 && now < g_blocks[i].blocked_until)) {
      snprintf(buf[count], 16, "%s", g_blocks[i].ip);
      count++;
    }
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return count;
}

/* Richer listing for the web dashboard: kind + expiry per entry. */
int blocklist_list_ex(block_info_t *out, int max) {
  int count = 0;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  blocklist_load_if_needed();
  time_t now = mono_now();
  for(int i = 0; i < g_blocks_cap && count < max; i++) {
    if(g_blocks[i].ip[0] == 0) continue;
    if(g_blocks[i].permanent || (g_blocks[i].blocked_until != 0 && now < g_blocks[i].blocked_until)) {
      snprintf(out[count].ip, sizeof(out[count].ip), "%s", g_blocks[i].ip);
      out[count].permanent = g_blocks[i].permanent ? 1 : 0;
      /* blocked_until is on the monotonic clock; report wall-clock time */
      out[count].until = g_blocks[i].permanent ? 0 : time(NULL) + (g_blocks[i].blocked_until - now);
      count++;
    }
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return count;
}

/* Clear all timed (non-permanent) blocks. Returns number cleared. */
int blocklist_unblock_all_timed(void) {
  int cleared = 0;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  for(int i = 0; i < g_blocks_cap; i++) {
    if(g_blocks[i].ip[0] && !g_blocks[i].permanent && g_blocks[i].blocked_until != 0) {
      g_blocks[i].ip[0] = 0;
      g_blocks[i].blocked_until = 0;
      g_blocks[i].fails = 0;
      g_blocks[i].fast_hits = 0;
      cleared++;
    }
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return cleared;
}

/* --- Batched PETSCII output --------------------------------------------
 * Banner drawing used to emit one write() syscall per byte (~90 per incoming
 * call, plus a matching trace/monitor hit each). These helpers accumulate into
 * a stack-local buffer (thread-safe — no shared state) and flush in a single
 * ip_write(). Callers flush at each pacing usleep so the "typing" effect and
 * exact byte order are preserved. */
typedef struct { int fd; int len; int failed; unsigned char buf[1024]; } obuf_t;

static void ob_init(obuf_t *o, int fd) { o->fd = fd; o->len = 0; o->failed = 0; }

static void ob_flush(obuf_t *o) {
  /* after one failed write (peer gone / not reading) stop trying: each further
   * attempt could wait for the send timeout again */
  if(o->len > 0 && !o->failed && ip_write(o->fd, o->buf, o->len) < 0) o->failed = 1;
  o->len = 0;
}

static void ob_byte(obuf_t *o, unsigned char b) {
  if(o->len >= (int)sizeof(o->buf)) ob_flush(o);
  o->buf[o->len++] = b;
}

static void ob_str(obuf_t *o, const char *s) {
  if(!s) return;
  while(*s) ob_byte(o, (unsigned char)*s++);
}

static void ob_goto(obuf_t *o, int row, int col) {
  /* row: 0-24, col: 0-39 — PETSCII HOME, then DOWN*row, RIGHT*col */
  ob_byte(o, 19);                              /* HOME  $13 */
  for(int i = 0; i < row; i++) ob_byte(o, 17); /* DOWN  $11 */
  for(int i = 0; i < col; i++) ob_byte(o, 29); /* RIGHT $1D */
}

/* Normalize PETSCII-ish input to ASCII-ish for password compare */
static unsigned char petscii_norm(unsigned char ch) {
  ch &= 0x7F; /* strip high bit */
  if(ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
  return ch;
}

static void norm_str_inplace(char *s) {
  for(; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c >= 'A' && c <= 'Z') *s = (char)(c - 'A' + 'a');
  }
}


// --- Pre-connect banner & bot gate (runs BEFORE any serial/modem traffic) ---

static void preconnect_write(int fd, const char *s) {
  if(s && *s) {
    (void)ip_write(fd, (unsigned char*)s, strlen(s));
  }
}


/* If path is a directory, pick a random file from it */
static int g_srand_done = 0;
static void pick_random_file(const char *dir, char *out, size_t outsz) {
  DIR *d;
  struct dirent *ent;
  char files[64][512];
  int count = 0;

  if(!g_srand_done) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); g_srand_done = 1; }
  d = opendir(dir);
  if(!d) { snprintf(out, outsz, "%s", dir); return; }
  while((ent = readdir(d)) && count < 64) {
    if(ent->d_name[0] == '.') continue; /* skip hidden/dot files */
    snprintf(files[count], sizeof(files[count]), "%s/%s", dir, ent->d_name);
    count++;
  }
  closedir(d);
  if(count == 0) { out[0] = '\0'; return; }
  snprintf(out, outsz, "%s", files[rand() % count]);
}

static void send_preconnect_banner(modem_config *cfg) {
  int fd = cfg->line_data.fd;
  struct stat st;

  /* Optional user-provided file or directory (raw bytes, can be PETSCII/ANSI) */
  if(cfg->pre_connect_file[0]) {
    if(stat(cfg->pre_connect_file, &st) == 0 && S_ISDIR(st.st_mode)) {
      char chosen[256];
      pick_random_file(cfg->pre_connect_file, chosen, sizeof(chosen));
      if(chosen[0]) { (void)writeFile(chosen, fd); }
    } else {
      (void)writeFile(cfg->pre_connect_file, fd);
    }
    return;
  }

  /* Elite PETSCII boot sequence */
  const unsigned char CLS = 147;     /* $93 CLR/HOME */
  const unsigned char GREEN = 30;    /* $1E */
  const unsigned char WHITE = 5;     /* $05 */
  const unsigned char YELLOW = 158;  /* $9E */
  const unsigned char RVS_ON = 18;   /* $12 */
  const unsigned char RVS_OFF = 146; /* $92 */

  obuf_t o; ob_init(&o, fd);
  ob_byte(&o, CLS);
  ob_byte(&o, WHITE);

  /* Header */
  ob_goto(&o, 2, 7);
  ob_byte(&o, RVS_ON);
  ob_byte(&o, GREEN);
  ob_str(&o, "   UNBREACHED BBS GATEWAY   ");
  ob_byte(&o, RVS_OFF);
  ob_byte(&o, WHITE);

  /* Boot lines — flush at each pace point so the "typing" effect is preserved */
  ob_goto(&o, 6, 4);
  ob_byte(&o, YELLOW);
  ob_str(&o, "Loading BBS... Please Wait");
  ob_flush(&o); usleep(250000);

  ob_goto(&o, 8, 4);
  ob_byte(&o, WHITE);
  ob_str(&o, "Enabling RS232 connection... ");
  ob_flush(&o); usleep(200000);
  ob_byte(&o, GREEN);
  ob_str(&o, "DONE");
  ob_byte(&o, WHITE);

  ob_goto(&o, 10, 4);
  ob_byte(&o, WHITE);
  ob_str(&o, "Connecting to Commodore 64... ");
  ob_flush(&o); usleep(200000);
  ob_byte(&o, GREEN);
  ob_str(&o, "DONE");
  ob_byte(&o, WHITE);

  ob_goto(&o, 13, 4);
  ob_byte(&o, GREEN);
  ob_str(&o, "READY.");
  ob_byte(&o, WHITE);

  ob_str(&o, "\r\n\r\n");
  ob_flush(&o);
}

static void send_access_granted(modem_config *cfg, int fd, int delay_us) {
  /* If user supplied a file, show it (raw bytes). Otherwise fallback to default message. */
  if(cfg && cfg->access_granted_file[0]) {
    obuf_t o; ob_init(&o, fd);
    ob_byte(&o, 147);            /* PETSCII CLR/HOME */
    ob_flush(&o);
    (void)writeFile(cfg->access_granted_file, fd);
    { obuf_t t; ob_init(&t, fd); ob_str(&t, "\r\n\r\n"); ob_flush(&t); }
    if(delay_us > 0) usleep(delay_us);
    return;
  }

  /* Default: clear + centered ACCESS GRANTED (green, reverse video) */
  {
    const unsigned char CLS = 147;
    const unsigned char GREEN = 30;
    const unsigned char WHITE = 5;
    const unsigned char RVS_ON = 18;
    const unsigned char RVS_OFF = 146;

    obuf_t o; ob_init(&o, fd);
    ob_byte(&o, CLS);
    ob_byte(&o, GREEN);
    ob_goto(&o, 12, 13);
    ob_byte(&o, RVS_ON);
    ob_str(&o, "ACCESS GRANTED");
    ob_byte(&o, RVS_OFF);
    ob_byte(&o, WHITE);
    ob_str(&o, "\r\n\r\n");
    ob_flush(&o);
    if(delay_us > 0) usleep(delay_us);
  }
}



/* ---- pre-connect phase: banner, gate, baud menu --------------------------
 * Runs in its own thread per call (gate_thread) so the line's bridge thread
 * keeps serving the C64, DTR and the dashboards while a caller (or a bot)
 * sits in the gate. It used to run on the bridge thread: AT commands from the
 * BBS waited for the whole gate timeout. While it runs, this thread owns the
 * caller socket; the bridge thread never closes it (see mdm_hangup). */
typedef struct { modem_config *cfg; int fd; unsigned gen; } gate_ctx;

enum { GATE_FAIL = 0, GATE_PASS = 1, GATE_GONE = 2 };

/* Wait for caller input until deadline_ms (monotonic). Input goes through the
 * line's telnet parser, so a telnet client's opening negotiation is answered
 * and remembered instead of being thrown away (which left the whole call in
 * raw mode). Returns the number of data bytes in out (0 = only negotiation
 * arrived), -1 on timeout, -2 if the caller is gone or the gate was cancelled. */
static int gate_read(gate_ctx *g, long long deadline_ms, unsigned char *out) {
  for(;;) {
    long long left = deadline_ms - mono_ms();
    if(g->cfg->gate_abort) return -2;
    if(left <= 0) return -1;
    struct pollfd p; p.fd = g->fd; p.events = POLLIN; p.revents = 0;
    int rc = poll(&p, 1, left > 500 ? 500 : (int)left);
    if(rc < 0 && errno == EINTR) continue;   /* a signal is not a timeout */
    if(rc < 0) return -2;
    if(rc == 0) continue;

    unsigned char in[256];
    int n = ip_read(g->fd, in, sizeof(in));
    if(n == 0) return -2;                    /* peer closed */
    if(n < 0) {
      if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return -2;
    }
    int m = line_filter(&g->cfg->line_data, &g->cfg->dce_data, g->gen, 0, in, n, out);
    if(m < 0) return -2;
    return m;
  }
}

static int preconnect_gate(gate_ctx *g) {
  modem_config *cfg = g->cfg;
  int fd = g->fd;
  int timeout = cfg->gate_timeout > 0 ? cfg->gate_timeout : 20;
  long long deadline = mono_ms() + (long long)timeout * 1000;

  if(cfg->gate_mode == 0) return GATE_PASS;

  if(cfg->gate_mode == 2) {
    preconnect_write(fd, "enter code: ");
  }

  int esc_count = 0;
  char pwbuf[128];
  int pwlen = 0;
  pwbuf[0] = 0;

  /* Limit wrong password guesses *within* a single connection. Without this
   * an attacker can pipeline thousands of "guess\n" lines into one TCP segment
   * and brute-force the gate before the per-IP fail counter (-F) ever ticks
   * (it only increments once per connection). */
  int wrong_guesses = 0;

  char pwcfg[128];
  snprintf(pwcfg, sizeof(pwcfg), "%s", cfg->gate_password);
  norm_str_inplace(pwcfg);

  while(TRUE) {
    unsigned char filtered[260];
    int m = gate_read(g, deadline, filtered);
    if(m == -1) break;            /* timeout */
    if(m < 0) return GATE_FAIL;   /* peer closed */

    for(int i = 0; i < m; i++) {
      unsigned char ch = filtered[i];

      if(cfg->gate_mode == 1) {
        /* BACKSPACE / DEL: accept common variants across PETSCII terminals */
        if(ch == 0x08 || ch == 0x14 || ch == 0x7f) {
          esc_count++;
          if(esc_count >= 2) {
            send_access_granted(cfg, fd, 400000);
            return GATE_PASS;
          }
        }
      } else if(cfg->gate_mode == 2) {
        if(ch == '\r' || ch == '\n') {
          pwbuf[pwlen] = 0;
          if(pwlen > 0) {
            norm_str_inplace(pwbuf);
            if(ct_streq(pwbuf, pwcfg)) {
              send_access_granted(cfg, fd, 700000);
              return GATE_PASS;
            }
            /* Non-empty wrong password counts as a guess. */
            wrong_guesses++;
            if(wrong_guesses >= GATE_MAX_GUESSES_PER_CONN) {
              preconnect_write(fd, "\r\nToo many attempts. Disconnecting.\r\n");
              return GATE_FAIL;
            }
            /* Throttle: slow down intra-connection brute force. */
            usleep(1500000);
          }
          // reset on wrong password or empty line
          pwlen = 0;
          pwbuf[0] = 0;
          preconnect_write(fd, "\r\ntry again:   ");
        } else if(ch >= 0x20) {
          unsigned char a = petscii_norm(ch);
          if(a >= 0x20 && a <= 0x7e) {
            if(pwlen < (int)sizeof(pwbuf) - 1) {
              pwbuf[pwlen++] = (char)a;
            }
          }
        }
      }
    }
  }

  preconnect_write(fd, "\r\nTimeout. Disconnecting.\r\n");
  return GATE_FAIL;
}
/* Launch a script securely after a successful gate.
 * Scripts must be in TCPSER_SCRIPT_DIR and follow naming restrictions.
 * Useful for playing a sound, notifying, etc. Truly asynchronous: a detached
 * supervisor process enforces the 30 s timeout and logs the outcome. */
static void run_gate_success_script(modem_config *cfg) {
  if(cfg->on_gate_success_script[0] == 0) return;

  LOG(LOG_DEBUG, "Launching gate success script: %s", cfg->on_gate_success_script);

  int result = execute_script_secure(cfg->on_gate_success_script,
                                     cfg->line_data.peer_ip,
                                     30);

  switch(result) {
    case SCRIPT_OK:
      LOG(LOG_DEBUG, "Gate script launched");
      break;
    case SCRIPT_INVALID_NAME:
      LOG(LOG_ERROR, "Invalid script name: %s (use alphanumeric, dash, underscore only)",
          cfg->on_gate_success_script);
      break;
    case SCRIPT_NOT_EXECUTABLE:
      LOG(LOG_ERROR, "Script not found or not executable: %s", cfg->on_gate_success_script);
      break;
    case SCRIPT_EXEC_FAILED:
      LOG(LOG_ERROR, "Script execution failed: %s", cfg->on_gate_success_script);
      break;
    default:
      LOG(LOG_ERROR, "Unknown script error: %s", cfg->on_gate_success_script);
      break;
  }
}
/* On-connect caller baud selection (-b), part 1 (gate thread): show the TCP
 * caller a numbered speed menu and wait up to 20 s for a digit.
 * Returns the chosen index, -1 for "keep the current speed", -2 if the caller
 * is gone. */
static int baud_menu_ask(gate_ctx *g) {
  modem_config *cfg = g->cfg;
  int fd = g->fd;

  if(!cfg->baud_menu_enabled || cfg->baud_menu_count <= 0) return -1;

  /* Build and send the menu over TCP */
  {
    char menu[512];
    int p = 0;
    p += snprintf(menu + p, sizeof(menu) - p, "\r\nSelect connection speed:\r\n");
    for(int k = 0; k < cfg->baud_menu_count && p < (int)sizeof(menu) - 40; k++) {
      p += snprintf(menu + p, sizeof(menu) - p, "  %d) %d baud\r\n",
                    k + 1, cfg->baud_menu_speed[k]);
    }
    snprintf(menu + p, sizeof(menu) - p, "Choice: ");
    preconnect_write(fd, menu);
  }

  long long deadline = mono_ms() + 20000;
  for(;;) {
    unsigned char filtered[260];
    int m = gate_read(g, deadline, filtered);
    if(m == -1) return -1;
    if(m < 0) return -2;
    for(int i = 0; i < m; i++) {
      unsigned char ch = filtered[i];
      if(ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if(idx < cfg->baud_menu_count) return idx;
      }
    }
  }
}

/* Baud selection, part 2 (bridge thread, which owns the serial port): send
 * "POKE 838,<val>" to the C64 (CBase executes it from its input stream) and
 * retune tcpser's own serial port to match, so the session runs at that
 * speed. Runs before ringing, while both sides still agree on the old speed. */
static void baud_menu_apply(modem_config *cfg, int chosen) {
  int fd = cfg->line_data.fd;

  if(!cfg->baud_menu_enabled || cfg->baud_menu_count <= 0) return;
  if(chosen < 0 || chosen >= cfg->baud_menu_count) {
    preconnect_write(fd, "\r\nKeeping current speed.\r\n");
    LOG(LOG_INFO, "Baud menu: no valid selection, keeping %d baud",
        cfg->dce_data.port_speed);
    return;
  }

  int speed = cfg->baud_menu_speed[chosen];
  int pokeval = cfg->baud_menu_poke[chosen];

  {
    char confirm[64];
    snprintf(confirm, sizeof(confirm), "\r\nSetting %d baud...\r\n", speed);
    preconnect_write(fd, confirm);
  }

  if(speed == cfg->dce_data.port_speed) {
    LOG(LOG_INFO, "Baud menu: %d baud already current, no POKE needed", speed);
    /* Report the negotiated speed in the CONNECT result. */
    cfg->line_speed = speed;
  } else {
    /* Send "POKE 838,<val>" + RETURN to CBase over the serial link at the
     * CURRENT speed (both sides still agree). PETSCII uppercase letters and
     * digits share ASCII code points, so the literal ASCII bytes are correct. */
    char poke[32];
    int plen = snprintf(poke, sizeof(poke), "POKE 838,%d\r", pokeval);
    /* snprintf returns the would-have-written length; clamp so a truncated
     * result can never make dce_write read past the buffer. */
    if(plen < 0) plen = 0;
    if(plen >= (int)sizeof(poke)) plen = (int)sizeof(poke) - 1;
    LOG(LOG_INFO, "Baud menu: caller chose %d baud -> sending POKE 838,%d",
        speed, pokeval);
    dce_write(&cfg->dce_data, (unsigned char *)poke, plen);
    /* Give CBase time to receive and execute the POKE (re-init its UART) before
     * we retune. dce_set_speed uses TCSADRAIN, so the POKE bytes already queued
     * are flushed at the old speed first, reaching the C64 intact. */
    usleep(300000);
    /* Only report the new speed if the local port actually switched; on a
     * tcsetattr failure keep (and report) the current speed rather than lie
     * about the CONNECT rate. */
    if(dce_set_speed(&cfg->dce_data, speed) == 0) {
      cfg->line_speed = speed;
    } else {
      LOG(LOG_WARN, "Baud menu: retune to %d baud failed, keeping %d",
          speed, cfg->dce_data.port_speed);
      preconnect_write(fd, "\r\nSpeed change failed, keeping current.\r\n");
      cfg->line_speed = cfg->dce_data.port_speed;
    }
  }
}

static void *gate_thread(void *arg) {
  gate_ctx *g = (gate_ctx *)arg;
  modem_config *cfg = g->cfg;
  int result = GATE_PASS;
  int baud = -1;

  if(!cfg->gate_trusted) {
    // 1) banner immediately upon TCP accept (BEFORE any serial/modem traffic)
    send_preconnect_banner(cfg);
    // 2) optional bot gate
    result = preconnect_gate(g);
  }
  if(result == GATE_PASS) {
    baud = baud_menu_ask(g);
    if(baud == -2) result = GATE_GONE;
  }
  cfg->gate_baud = baud;
  cfg->gate_result = result;
  writePipe(cfg->cp[0][1], MSG_GATE_DONE);   /* the bridge thread takes over */
  free(g);
  return NULL;
}

/* MSG_CALLING: take the TCP connection, apply the instant checks (blocklist,
 * rate limit) and hand the caller to a gate thread. */
int accept_connection(modem_config *cfg) {
  LOG_ENTER();

  if(cfg->line_data.is_connected == TRUE || cfg->dial_pending || cfg->gate_pending) {
    /* raced with our own state change after main picked us: leave the
     * connection queued, main will route it again */
    writePipe(cfg->mp[0][1], MSG_BUSY);
    LOG_EXIT();
    return 0;
  }
  if(-1 == line_accept(&cfg->line_data)) {
    /* Accept failed — still release the parent's accept gate so it can resume
     * (otherwise accept_pending would stay set and freeze every port). */
    writePipe(cfg->mp[0][1], MSG_BUSY);
    LOG_EXIT();
    return 0;
  }

  /* The TCP connection is now ours (cfg->line_data.fd, is_connected == TRUE).
   * Release the global accept gate IMMEDIATELY. The parent only routes new
   * calls to modems whose line is not already connected, so it won't re-route
   * to us while the gate runs. */
  writePipe(cfg->mp[0][1], MSG_BUSY);

  snprintf(cfg->last_peer, sizeof(cfg->last_peer), "%s", cfg->line_data.peer_ip);
  dashboard_stat_connection(cfg->line_data.peer_ip);

  /* Allowlisted IPs skip blocklist, rate limit, and gate */
  int is_trusted = ip_is_allowed(cfg->line_data.peer_ip);
  if(is_trusted) {
    LOG(LOG_INFO, "Allowlisted IP %s — skipping gate", cfg->line_data.peer_ip);
  } else {
    /* Drop immediately if blocked (no banner, no gate, no serial) */
    if(ip_is_blocked(cfg->line_data.peer_ip)) {
      LOG(LOG_INFO, "Blocked IP %s tried to connect", cfg->line_data.peer_ip);
      dashboard_stat_blocked(cfg->line_data.peer_ip);
      connlog_event(cfg->line_data.peer_ip, CONNLOG_BLOCKED);
      line_disconnect(&cfg->line_data);
      LOG_EXIT();
      return 0;
    }

    /* Rate limit: enforce minimum delay between attempts from same IP. An IP
     * that keeps hammering (FLOOD_HITS rate-limited connects in a row) gets a
     * timed block: it is then dropped by the check above without touching
     * this code, and the block shows on the dashboards, where it can be
     * cleared. Timed blocks are not written to the blocklist file. */
    {
      pthread_mutex_lock(&g_blocks_mutex);
      ensure_blocks_init();
      ip_block_t *rl = find_or_alloc_ip(cfg->line_data.peer_ip);
      int too_fast = 0, flood = 0;
      if(rl) {
        time_t now = mono_now();
        if(rl->last_attempt > 0 && (now - rl->last_attempt) < RATE_LIMIT_SECONDS) {
          too_fast = 1;
          if(++rl->fast_hits >= FLOOD_HITS) {
            rl->fast_hits = 0;
            rl->blocked_until = now + FLOOD_BLOCK_SECONDS;
            flood = 1;
          }
        } else rl->fast_hits = 0;
        rl->last_attempt = now;
      }
      pthread_mutex_unlock(&g_blocks_mutex);
      if(flood) {
        LOG(LOG_WARN, "IP %s is flooding the line; blocked for %d minutes", cfg->line_data.peer_ip, FLOOD_BLOCK_SECONDS / 60);
        dashboard_note("blocked %s for %d min (connection flood)", cfg->line_data.peer_ip, FLOOD_BLOCK_SECONDS / 60);
      }
      if(too_fast) {
        LOG(LOG_INFO, "Rate limited IP %s (too fast)", cfg->line_data.peer_ip);
        dashboard_stat_blocked(cfg->line_data.peer_ip);
        connlog_event(cfg->line_data.peer_ip, CONNLOG_RATE_LIMIT);
        line_disconnect(&cfg->line_data);
        LOG_EXIT();
        return 0;
      }
    }
  }

  gate_ctx *g = (gate_ctx *)calloc(1, sizeof(*g));
  if(!g) { line_disconnect(&cfg->line_data); LOG_EXIT(); return 0; }
  g->cfg = cfg;
  g->fd = cfg->line_data.fd;
  g->gen = cfg->line_data.gen;
  cfg->gate_trusted = is_trusted;
  cfg->gate_abort = FALSE;
  cfg->gate_baud = -1;
  cfg->gate_pending = TRUE;
  spawn_thread((void *)gate_thread, (void *)g, "GATE");
  LOG_EXIT();
  return 0;
}

/* MSG_GATE_DONE: the gate thread is finished with the caller. */
static void gate_finish(modem_config *cfg) {
  if(!cfg->gate_pending) return;
  cfg->gate_pending = FALSE;
  /* the reader thread takes over watching the socket (it leaves it alone
   * while the gate thread owns it): make it look again */
  writePipe(cfg->cp[1][1], MSG_NOTIFY);

  /* copy the IP first: line_disconnect() clears peer_ip, and the fail2ban
   * filter (samples/fail2ban) keys on the log line below */
  char peer[INET_ADDRSTRLEN];
  snprintf(peer, sizeof(peer), "%s", cfg->line_data.peer_ip);

  if(cfg->gate_abort || cfg->gate_result == GATE_GONE) {
    /* we cancelled it (operator hangup, shutdown), or the caller left during
     * the baud menu: no strike */
    LOG(LOG_INFO, "Pre-connect phase ended for %s before the call was put through", peer);
    line_disconnect(&cfg->line_data);
    return;
  }
  if(cfg->gate_result != GATE_PASS) {
    dashboard_stat_gate(0, peer);
    connlog_event(peer, CONNLOG_GATE_FAIL);
    webhook_notify("GATE_FAIL", peer);
    ip_note_fail(peer, cfg->gate_max_fails, 600);
    line_disconnect(&cfg->line_data);
    LOG(LOG_INFO, "Pre-connect gate failed for %s; disconnected before touching serial", peer);
    return;
  }

  // 3) Gate passed: run optional external hook (non-blocking)
  dashboard_stat_gate(1, peer);
  connlog_event(peer, CONNLOG_CONNECTED);
  dashboard_line_state(cfg->idx, 1, peer);
  run_gate_success_script(cfg);

  /* On-connect caller baud selection (-b): apply the caller's choice. */
  baud_menu_apply(cfg, cfg->gate_baud);

  // 4) Start an answer timer so scanners don't hold the line forever. A BBS
  //    that auto-answers on ring S0 needs (S0 - 1) * 4 s just to get there.
  int wait = cfg->incoming_answer_timeout > 0 ? cfg->incoming_answer_timeout : 10;
  if(cfg->s[S_REG_RINGS] > 0 && cfg->s[S_REG_RINGS] * 4 + 2 > wait) wait = cfg->s[S_REG_RINGS] * 4 + 2;
  cfg->incoming_waiting_answer = TRUE;
  cfg->incoming_answer_deadline = mono_now() + wait;
  LOG(LOG_INFO, "Gate passed for %s; waiting for local answer (%ds)", peer, wait);

  /* Set grace period NOW so DTR drops during RING phase are ignored */
  cfg->conn_established = mono_now();

  if(cfg->direct_conn == TRUE) {
    cfg->conn_type = MDM_CONN_INCOMING;
    mdm_off_hook(cfg);
  } else {
    cfg->rings = 0;
    mdm_send_ring(cfg);
  }
}

/* Bytes from the network: through the telnet parser, then to the C64. gen is
 * the line generation the bytes were read under; if the call ended meanwhile
 * they are dropped rather than applied to the next caller. */
static void parse_ip_data(modem_config *cfg, unsigned gen, unsigned char *data, int len) {
  unsigned char out[260];

  if(len > 257) len = 257;
  int n = line_filter(&cfg->line_data, &cfg->dce_data, gen, 1, data, len, out);
  if(n > 0) mdm_write(cfg, out, n);
}

static void post_disconnect(modem_config *cfg, unsigned gen) {
  cfg->disc_gen = gen;
  writePipe(cfg->cp[0][1], MSG_DISCONNECT);
}

void *ip_thread(void *arg) {
  modem_config* cfg = (modem_config *)arg;

  int action_pending = FALSE;
  fd_set readfs;
  int max_fd;
  int res = 0;
  unsigned char buf[258];
  int rc;

  LOG_ENTER();
  while(TRUE) {
    /* One snapshot per pass: the bridge thread can disconnect at any moment,
     * and re-reading fd after the check could hand select() a -1 (which
     * aborts under glibc's fortify) or the next caller's descriptor. */
    int fd = cfg->line_data.fd;
    unsigned gen = cfg->line_data.gen;
    int connected = (cfg->line_data.is_connected == TRUE) && fd > -1 && fd < FD_SETSIZE;
    int data_mode = cfg->conn_type != MDM_CONN_NONE && cfg->is_cmd_mode == FALSE;
    int watch = !action_pending && connected && data_mode;
    /* Not in data mode (ringing, or online command mode after +++): nobody
     * reads the socket, so a caller who leaves would go unnoticed. Look once
     * a second. Not while the gate thread owns the socket. */
    int peek = !action_pending && connected && !data_mode && !cfg->gate_pending;
    struct timeval tv;

    FD_ZERO(&readfs);
    FD_SET(cfg->cp[1][0], &readfs);
    max_fd = cfg->cp[1][0];
    if(watch) {
      FD_SET(fd, &readfs);
      max_fd=MAX(max_fd, fd);
    }
    max_fd++;
    tv.tv_sec = 1; tv.tv_usec = 0;
    rc = select(max_fd, &readfs, NULL, NULL, peek ? &tv : NULL);
    if(rc == -1) {
      /* EBADF when the descriptor was closed under us: re-snapshot, no spin */
      if(errno != EINTR) usleep(10000);
      continue;
    }
    if(watch && FD_ISSET(fd, &readfs)) {  // socket
      LOG(LOG_DEBUG, "Data available on socket");
      /* Read at most sizeof(buf)-1 so there is room for a terminator. */
      res = ip_read(fd, buf, sizeof(buf) - 1);
      if(res == 0 || (res < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        LOG(LOG_INFO, "No socket data read, assume closed peer");
        post_disconnect(cfg, gen);
        action_pending = TRUE;
      } else if(res > 0) {
        LOG(LOG_DEBUG, "Read %d bytes from socket", res);
        buf[res] = 0;
        parse_ip_data(cfg, gen, buf, res);
      }
    } else if(peek && rc == 0) {
      unsigned char c;
      ssize_t r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
      if(r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        if(cfg->line_data.gen == gen) {
          LOG(LOG_INFO, "Caller closed the connection while not in data mode");
          post_disconnect(cfg, gen);
          action_pending = TRUE;
        }
      }
    }
    if (FD_ISSET(cfg->cp[1][0], &readfs)) {  // pipe
      res = readPipe(cfg->cp[1][0], buf, sizeof(buf) - 1);
      LOG(LOG_DEBUG, "IP thread notified");
      action_pending = FALSE;
    }
  }
  LOG_EXIT();
}

void *ctrl_thread(void *arg) {
  modem_config* cfg = (modem_config *)arg;
  int status;
  int new_status;
  int lost_reported = FALSE;

  LOG_ENTER();
  status = dce_get_control_lines(&cfg->dce_data);
  for(;;) {
    if(status < 0) {
      /* Serial status unavailable: the USB adapter vanished or the port is
       * being reopened by bridge_task. This used to exit(-1) the whole
       * process — which is why the advertised USB auto-reconnect never ran.
       * Tell bridge_task once, then poll until the port answers again. */
      if(!lost_reported) {
        LOG(LOG_WARN, "Control-line status unavailable on %s — serial port lost? Waiting for it to return",
            cfg->dce_data.tty);
        writePipe(cfg->wp[0][1], MSG_SER_LOST);
        lost_reported = TRUE;
      }
      usleep(500000);
      if(cfg->dce_data.is_connected && cfg->dce_data.fd >= 0)
        status = dce_get_control_lines(&cfg->dce_data);
      if(status >= 0) {
        LOG(LOG_INFO, "Control-line status restored on %s", cfg->dce_data.tty);
        lost_reported = FALSE;
      }
      continue;
    }
    new_status = dce_check_control_lines(&cfg->dce_data);
    if(new_status > -1 && status != new_status) {
      LOG(LOG_DEBUG, "Control Line Change");
      if((new_status & DCE_CL_DTR) != (status & DCE_CL_DTR)) {
        if((new_status & DCE_CL_DTR)) {
          LOG(LOG_INFO, "DTR has gone high");
          dashboard_line_dtr(cfg->idx, 1);
          writePipe(cfg->wp[0][1], MSG_DTR_UP);
        } else {
          LOG(LOG_INFO, "DTR has gone low");
          dashboard_line_dtr(cfg->idx, 0);
          writePipe(cfg->wp[0][1], MSG_DTR_DOWN);
        }
      }
      if((new_status & DCE_CL_LE) != (status & DCE_CL_LE)) {
        if((new_status & DCE_CL_LE)) {
          LOG(LOG_INFO, "Link has come up");
          writePipe(cfg->wp[0][1], MSG_LE_UP);
        } else {
          LOG(LOG_INFO, "Link has gone down");
          writePipe(cfg->wp[0][1], MSG_LE_DOWN);
        }
      }
    }
    status = new_status;
  }
  LOG_EXIT();
  return NULL;
}

/* Reconnect a lost serial port (USB adapter unplugged/glitch).
 * Closes the old fd and keeps waiting for the device to reappear — forever.
 * Exiting the process here only trades a recoverable outage for a restart
 * that loses live state; the operator is warned once a minute instead.
 * Returns 0 on success, -1 only for ip232 (nothing to reopen).
 */
static int serial_reconnect(dce_config *dce) {
  if(dce->is_ip232) return -1; /* only for real serial ports */

  LOG(LOG_WARN, "Serial port %s lost — attempting reconnect", dce->tty);
  dashboard_note("serial %s lost - waiting for it to return", dce->tty);

  dce->is_connected = FALSE;
  if(dce->fd >= 0) { close(dce->fd); dce->fd = -1; }

  for(int attempt = 1; ; attempt++) {
    usleep(2000000); /* 2 seconds */
    if(access(dce->tty, R_OK | W_OK) == 0) {
      int rc = dce_connect(dce);
      if(rc >= 0) {
        LOG(LOG_INFO, "Serial port %s reconnected (attempt %d)", dce->tty, attempt);
        dashboard_note("serial %s reconnected", dce->tty);
        return 0;
      }
    }
    if(attempt % 30 == 0)
      LOG(LOG_WARN, "Serial port %s still missing after %d s — waiting", dce->tty, attempt * 2);
    else
      LOG(LOG_DEBUG, "Waiting for %s... (attempt %d)", dce->tty, attempt);
  }
}

/* Common handling once bridge_task knows the serial port is gone. */
static void handle_serial_lost(modem_config *cfg) {
  if(cfg->dce_data.is_ip232) return;
  if(cfg->conn_type != MDM_CONN_NONE) {
    /* The caller's BBS just vanished under them: drop the call cleanly. */
    LOG(LOG_WARN, "Serial port lost during a call — hanging up the caller");
    mdm_disconnect(cfg, TRUE);
  }
  serial_reconnect(&cfg->dce_data);
  dashboard_set_serial_fd(cfg->dce_data.fd);
  /* the port was reopened raw: put the operator's flow control (&K3) back */
  dce_set_flow_control(&cfg->dce_data, cfg->dce_data.flow_control);
  mdm_set_control_lines(cfg);
}

/* A DTR drop that counts (outside the post-connect grace period). */
static void dtr_dropped(modem_config *cfg) {
  if(cfg->dtr_mode == 1) {
    /* &D1: back to command mode, keep the carrier */
    if(cfg->is_cmd_mode == FALSE) {
      cfg->is_cmd_mode = TRUE;
      mdm_send_response(MDM_RESP_OK, cfg);
    }
  } else {
    mdm_disconnect(cfg, FALSE);
    if(cfg->dtr_mode == 3) mdm_reset_profile(cfg);   /* &D3 */
  }
}

/* The local side never answered the ringing caller. */
static void no_answer(modem_config *cfg, int *consecutive_no_answer) {
  if(strlen(cfg->no_answer) == 0) {
    line_write(&cfg->line_data, (unsigned char *)MDM_NO_ANSWER, strlen(MDM_NO_ANSWER));
  } else {
    writeFile(cfg->no_answer, cfg->line_data.fd);
  }
  cfg->incoming_waiting_answer = FALSE;
  cfg->incoming_answer_deadline = 0;
  cfg->is_ringing = FALSE;
  (*consecutive_no_answer)++;
  LOG(LOG_INFO, "Answer timeout (%d consecutive no-answers)", *consecutive_no_answer);
  if(*consecutive_no_answer >= 3 && cfg->watchdog_timeout > 0) {
    LOG(LOG_WARN, "BBS appears hung — %d consecutive no-answers, toggling DTR",
        *consecutive_no_answer);
    dce_set_control_lines(&cfg->dce_data, 0);
    usleep(500000);
    mdm_set_control_lines(cfg);
    *consecutive_no_answer = 0;
  }
  dashboard_line_state(cfg->idx, 0, NULL);
  connlog_event(cfg->line_data.peer_ip, CONNLOG_NO_ANSWER);
  line_disconnect(&cfg->line_data);
}

void *bridge_task(void *arg) {
  modem_config *cfg = (modem_config *)arg;
  struct timeval timer;
  struct timeval *ptimer;
  int max_fd = 0;
  fd_set readfs;
  int res = 0;
  unsigned char buf[256];
  int rc = 0;

  int last_conn_type;
  int last_cmd_mode = cfg->is_cmd_mode;

  /* BBS watchdog: detect hung BBS after disconnect */
  time_t watchdog_start = 0;       /* 0 = inactive */
  int watchdog_dtr_attempts = 0;   /* how many DTR toggles we've tried */
  int consecutive_no_answer = 0;   /* callers that got no-answer in a row */

  LOG_ENTER();

  if(-1 == pipe(cfg->wp[0])) {
    ELOG(LOG_FATAL, "Control line watch task incoming IPC pipe could not be created");
    exit(-1);
  }
  if(-1 == pipe(cfg->cp[0])) {
    ELOG(LOG_FATAL, "IP thread incoming IPC pipe could not be created");
    exit(-1);
  }
  if(-1 == pipe(cfg->cp[1])) {
    ELOG(LOG_FATAL, "IP thread outgoing IPC pipe could not be created");
    exit(-1);
  }
  set_cloexec(cfg->wp[0][0]); set_cloexec(cfg->wp[0][1]);
  set_cloexec(cfg->cp[0][0]); set_cloexec(cfg->cp[0][1]);
  set_cloexec(cfg->cp[1][0]); set_cloexec(cfg->cp[1][1]);
  if(dce_connect(&cfg->dce_data) < 0) {
    ELOG(LOG_FATAL, "Could not open serial port %s", cfg->dce_data.tty);
    exit(-1);
  }

  /* Give dashboard access to modem config for interactive commands */
  dashboard_set_serial_fd(cfg->dce_data.fd);
  dashboard_set_modem_cfg((void *)cfg);
  dashboard_line_init(cfg->idx, cfg->dce_data.is_ip232 ? "(ip232)" : cfg->dce_data.tty,
                      cfg->dce_data.port_speed, (void *)cfg);

  spawn_thread((void *)ctrl_thread, (void *)cfg, "CTRL");
  spawn_thread((void *)ip_thread, (void *)cfg, "IP");

  mdm_set_control_lines(cfg);
  last_conn_type = cfg->conn_type;
  cfg->allow_transmit = FALSE;
  // call some functions behind the scenes
  if(cfg->cur_line_idx) {
    mdm_parse_cmd(cfg);
  }
  /* Everything the operator configured (-I, -i "&C0&K3E0", ...) is now in
   * effect: snapshot it so ATZ / AT&F restore it instead of factory defaults. */
  mdm_save_profile(cfg);
  if (cfg->direct_conn == TRUE) {
    if(strlen((char *)cfg->direct_conn_num) > 0 &&
       cfg->direct_conn_num[0] != ':') {
        // we have a direct number to connect to.
      snprintf(cfg->dialno, sizeof(cfg->dialno), "%s", cfg->direct_conn_num);
      if(0 != line_connect(&cfg->line_data, cfg->dialno)) {
        LOG(LOG_FATAL, "Cannot connect to Direct line address!");
        // probably should exit...
        exit(-1);
      } else {
        snprintf(cfg->last_peer, sizeof(cfg->last_peer), "%.*s", (int)sizeof(cfg->last_peer) - 1, cfg->dialno);
        cfg->conn_type = MDM_CONN_OUTGOING;
      }
    }
  }
  cfg->allow_transmit = TRUE;
  for(;;) {
    if(last_conn_type != cfg->conn_type) {
      LOG(LOG_ALL, "Connection status change, handling");
      writePipe(cfg->cp[1][1], MSG_NOTIFY);

      /* Watchdog: start timer when connection drops, clear when connected.
       * The webhooks use last_peer: line_data.peer_ip is already cleared when
       * the call has ended (DISCONNECTED used to carry an empty IP). */
      if(cfg->conn_type == MDM_CONN_NONE && last_conn_type != MDM_CONN_NONE) {
        webhook_notify("DISCONNECTED", cfg->last_peer);
        if(cfg->watchdog_timeout > 0) {
          watchdog_start = mono_now();
          watchdog_dtr_attempts = 0;
          LOG(LOG_INFO, "BBS watchdog started (%ds timeout)", cfg->watchdog_timeout);
        }
      } else if(cfg->conn_type != MDM_CONN_NONE) {
        webhook_notify("CONNECTED", cfg->last_peer);
        watchdog_start = 0;
        watchdog_dtr_attempts = 0;
        consecutive_no_answer = 0;  /* BBS answered, reset counter */
      }

      /* -c / -a go to the C64 (they used to be sent to the caller, the same
       * as -C / -A); -C / -A go to the remote side. */
      if(cfg->conn_type == MDM_CONN_OUTGOING) {
        if(strlen(cfg->local_connect) > 0) {
          mdm_write_file(cfg, cfg->local_connect);
        }
        if(strlen(cfg->remote_connect) > 0) {
          writeFile(cfg->remote_connect, cfg->line_data.fd);
        }
      } else if(cfg->conn_type == MDM_CONN_INCOMING) {
        if(strlen(cfg->local_answer) > 0) {
          mdm_write_file(cfg, cfg->local_answer);
        }
        if(strlen(cfg->remote_answer) > 0) {
          writeFile(cfg->remote_answer, cfg->line_data.fd);
        }
      }
      last_conn_type = cfg->conn_type;
    }
    if(last_cmd_mode != cfg->is_cmd_mode) {
      writePipe(cfg->cp[1][1], MSG_NOTIFY);
      last_cmd_mode = cfg->is_cmd_mode;
    }

    // If we were waiting for the local serial/BBS to answer an incoming call,
    // clear the "waiting" flag once we go off-hook (ATA / answer).
    if(cfg->incoming_waiting_answer == TRUE && cfg->is_off_hook == TRUE) {
      cfg->incoming_waiting_answer = FALSE;
    }
    LOG(LOG_ALL, "Waiting for modem/control line/timer/socket activity");
    LOG(LOG_ALL, "CMD:%d, DCE:%d, LINE:%d, TYPE:%d, HOOK:%d", cfg->is_cmd_mode, cfg->dce_data.is_connected, cfg->line_data.is_connected, cfg->conn_type, cfg->is_off_hook);
    FD_ZERO(&readfs);
    max_fd = cfg->mp[1][0];
    FD_SET(cfg->mp[1][0], &readfs);
    if(cfg->dce_data.is_connected && cfg->dce_data.fd >= 0) {
      max_fd = MAX(max_fd, cfg->dce_data.fd);
      FD_SET(cfg->dce_data.fd, &readfs);
    }
    if(cfg->dce_data.is_ip232) {
      /* ip232_thread pokes this pipe when an emulator attaches, so we pick
       * up the new socket immediately instead of on the next unrelated wake. */
      max_fd = MAX(max_fd, cfg->dce_data.dp[0][0]);
      FD_SET(cfg->dce_data.dp[0][0], &readfs);
    }
    max_fd = MAX(max_fd, cfg->wp[0][0]);
    FD_SET(cfg->wp[0][0], &readfs);
    max_fd = MAX(max_fd, cfg->cp[0][0]);
    FD_SET(cfg->cp[0][0], &readfs);

    /* Timers. The modem's own timer (escape guard time, inactivity, ring
     * cadence) is `mdm_ms`; the housekeeping deadlines (watchdog, session
     * limit, DTR re-check) only shorten the wait. A housekeeping wake must not
     * be mistaken for the modem timer: that sent a stray OK to an idle C64,
     * and an idle caller was never cut at the -m limit because nothing woke
     * the loop. */
    enum { TMR_NONE = 0, TMR_MODEM, TMR_RING };
    int tmr_kind = TMR_NONE;
    long long mdm_ms = -1, wait_ms = -1;
    time_t now = mono_now();
    if(cfg->is_cmd_mode == FALSE && cfg->conn_type != MDM_CONN_NONE) {
      if(cfg->pre_break_delay == FALSE || cfg->break_len == 3) {
        LOG(LOG_ALL, "Setting timer for break delay");
        mdm_ms = (long long)cfg->s[S_REG_GUARD_TIME] * 20;
      } else if(cfg->pre_break_delay == TRUE && cfg->break_len > 0) {
        LOG(LOG_ALL, "Setting timer for inter-break character delay");
        mdm_ms = 1000;
      } else if (cfg->s[S_REG_INACTIVITY_TIME] != 0) {
        LOG(LOG_ALL, "Setting timer for inactivity delay");
        mdm_ms = (long long)cfg->s[S_REG_INACTIVITY_TIME] * 10000;
      }
      if(mdm_ms >= 0) tmr_kind = TMR_MODEM;
    } else if(cfg->is_cmd_mode == TRUE
              && cfg->conn_type == MDM_CONN_NONE
              && cfg->line_data.is_connected == TRUE
              && !cfg->gate_pending
             ) {
        LOG(LOG_ALL, "Setting timer for rings");
        mdm_ms = 4000;
        tmr_kind = TMR_RING;
    }
    wait_ms = mdm_ms;
#define SOONER(ms) do { long long m_ = (ms); if(m_ < 0) m_ = 0; if(wait_ms < 0 || m_ < wait_ms) wait_ms = m_; } while(0)
    if(watchdog_start > 0) SOONER(5000);
    if(cfg->max_session_minutes > 0 && cfg->conn_type != MDM_CONN_NONE && cfg->conn_established > 0)
      SOONER(((long long)cfg->conn_established + (long long)cfg->max_session_minutes * 60 - now) * 1000 + 500);
    if(cfg->dtr_recheck_at > 0) SOONER(((long long)cfg->dtr_recheck_at - now) * 1000 + 200);
#undef SOONER
    ptimer = NULL;
    if(wait_ms >= 0) {
      timer.tv_sec = wait_ms / 1000;
      timer.tv_usec = (wait_ms % 1000) * 1000;
      ptimer = &timer;
    }
    max_fd++;
    rc = select(max_fd, &readfs, NULL, NULL, ptimer);
    if(rc == -1) {
      if(errno == EBADF && !cfg->dce_data.is_ip232) {
        LOG(LOG_WARN, "Select EBADF — serial port may be lost");
        handle_serial_lost(cfg);
        continue;
      }
      if(errno != EINTR) ELOG(LOG_WARN, "Select returned error");
      /* readfs is undefined after an error return — do NOT fall through to the
       * FD_ISSET checks below (EINTR etc. land here too). Re-loop instead. */
      continue;
    } else if(rc == 0 && tmr_kind != TMR_NONE && wait_ms == mdm_ms) {
      // the modem timer popped.
      if(tmr_kind == TMR_RING) {
        if(cfg->is_cmd_mode == TRUE
           && cfg->conn_type == MDM_CONN_NONE
           && cfg->line_data.is_connected == TRUE
           && !cfg->gate_pending) {
          // Enforce the "must answer" window, then ring again.
          if(cfg->incoming_waiting_answer == TRUE && cfg->incoming_answer_deadline != 0
             && mono_now() >= cfg->incoming_answer_deadline) {
            no_answer(cfg, &consecutive_no_answer);
            continue;
          }
          mdm_send_ring(cfg);
        }
      } else if(cfg->is_cmd_mode == FALSE) {
        mdm_handle_timeout(cfg);
      }
    }
    /* BBS watchdog: no serial data since disconnect */
    if(watchdog_start > 0 && cfg->conn_type == MDM_CONN_NONE
       && !cfg->line_data.is_connected) {
      time_t elapsed = mono_now() - watchdog_start;
      if(elapsed >= cfg->watchdog_timeout) {
        watchdog_dtr_attempts++;
        LOG(LOG_WARN, "BBS watchdog fired: no serial data for %lds, toggling DTR (attempt %d)",
            (long)elapsed, watchdog_dtr_attempts);
        /* Toggle DCD/DTR low for 500ms to signal carrier loss to 6551 */
        dce_set_control_lines(&cfg->dce_data, 0);
        usleep(500000);
        mdm_set_control_lines(cfg);  /* restore normal lines */
        if(watchdog_dtr_attempts >= 3) {
          /* After 3 attempts, send a break signal as last resort, then STOP:
           * a BBS that is healthy but silent (no caller, no prompt) used to
           * get a DTR pulse every -w seconds and a BREAK every third time,
           * forever (audit STAB-M5). The watchdog re-arms on the next
           * disconnect; the operator sees the give-up on both dashboards. */
          LOG(LOG_ERROR, "BBS watchdog: 3 DTR toggles failed, sending BREAK and giving up until the next call");
          if(!cfg->dce_data.is_ip232) tcsendbreak(cfg->dce_data.fd, 0);   /* no BREAK on a TCP socket */
          dashboard_note("line %d: BBS silent for %lds after disconnect; DTR x3 + BREAK sent, watchdog off",
                         cfg->idx, (long)(cfg->watchdog_timeout * 3));
          watchdog_dtr_attempts = 0;
          watchdog_start = 0;
          continue;
        }
        watchdog_start = mono_now();  /* reset for next attempt */
      }
    }
    /* Max session time enforcement */
    if(cfg->max_session_minutes > 0 && cfg->conn_type != MDM_CONN_NONE
       && cfg->conn_established > 0) {
      long session_secs = (long)(mono_now() - cfg->conn_established);
      if(session_secs >= cfg->max_session_minutes * 60) {
        LOG(LOG_INFO, "Max session time reached (%d min), disconnecting",
            cfg->max_session_minutes);
        dashboard_note("line %d: session limit (%d min) reached, caller disconnected", cfg->idx, cfg->max_session_minutes);
        mdm_disconnect(cfg, FALSE);
      }
    }
    /* A DTR drop inside the post-connect grace period was ignored (the BBS
     * may still be finishing its reset from the previous call). If DTR is
     * still low when the grace ends, it was a real hangup request. */
    if(cfg->dtr_recheck_at > 0 && mono_now() >= cfg->dtr_recheck_at) {
      cfg->dtr_recheck_at = 0;
      if(cfg->conn_type != MDM_CONN_NONE && cfg->dtr_mode != 0) {
        int st = dce_get_control_lines(&cfg->dce_data);
        if(st >= 0 && !(st & DCE_CL_DTR)) {
          LOG(LOG_INFO, "DTR still low after the grace period: acting on it");
          dtr_dropped(cfg);
        }
      }
    }
    if(rc <= 0) continue;   /* nothing readable: readfs is all-clear on timeout */

    /* fd is -1 in ip232 mode until the emulator attaches; FD_ISSET(-1) is UB
     * and aborts under glibc's _FORTIFY_SOURCE=2. Mirror the FD_SET guard. */
    if (cfg->dce_data.is_connected && cfg->dce_data.fd >= 0
        && FD_ISSET(cfg->dce_data.fd, &readfs)) {  // serial port
      LOG(LOG_DEBUG, "Data available on serial port");
      res = mdm_read(cfg, buf, sizeof(buf));
      if(res < 0 && !cfg->dce_data.is_ip232) {
        /* Serial port error — USB adapter may have disconnected */
        handle_serial_lost(cfg);
        continue;
      }
      if(res > 0) {
        /* BBS is alive — reset watchdog */
        if(watchdog_start > 0) {
          LOG(LOG_DEBUG, "BBS watchdog reset (serial data received)");
          watchdog_start = 0;
          watchdog_dtr_attempts = 0;
        }
        if(cfg->conn_type == MDM_CONN_NONE
           && !cfg->is_cmd_mode
           && cfg->is_off_hook) {
          /* Off hook with no connection: an outgoing call still being set
           * up, or ATD/ATH1 with nothing dialled. Any key aborts, as on a
           * real modem (the CR/LF that ends the ATD line does not count). */
          int abort_call = FALSE;
          for(int i = 0; i < res; i++) {
            unsigned char c = buf[i] & 0x7f;
            if(c != '\r' && c != '\n') abort_call = TRUE;
          }
          if(abort_call || !cfg->dial_pending) {
            LOG(LOG_INFO, "Call attempt aborted from the keyboard");
            mdm_disconnect(cfg, FALSE);      /* cancels the dial, back on hook */
            mdm_send_response(MDM_RESP_NO_CARRIER, cfg);
          }
        } else {
          mdm_parse_data(cfg, buf, res);
        }
      }
    }
    if (cfg->dce_data.is_ip232 && FD_ISSET(cfg->dce_data.dp[0][0], &readfs)) {
      readPipe(cfg->dce_data.dp[0][0], buf, sizeof(buf));   /* just a wake-up */
      LOG(LOG_INFO, "ip232 emulator attached");
      /* a fresh emulator assumes no carrier: tell it the real DCD state
       * (matters with &C0, or when it re-attaches during a call) */
      mdm_set_control_lines(cfg);
    }
    if (FD_ISSET(cfg->wp[0][0], &readfs)) {  // control pipe
      res = readPipe(cfg->wp[0][0], buf, sizeof(buf));
      if (res <= 0) {
        LOG(LOG_DEBUG, "Control pipe read returned %d", res);
      } else {
        LOG(LOG_DEBUG, "Received %d byte(s) from control line watch task", res);
        for (int i = 0; i < res; i++) {
          switch (buf[i]) {
            case MSG_DTR_DOWN:
              // DTR drop behavior depends on &D setting
              // 0 = ignore, 1 = command mode (keep carrier), 2 = hang up,
              // 3 = hang up and reset
              // IMPORTANT: Only act on DTR drop if there's an active connection.
              // During modem init, the BBS toggles DTR as part of its reset
              // sequence — this is NOT a hang-up request.
              // Grace period: a drop within 5 seconds of the connection is
              // not acted on at once (race with the BBS reset sequence from
              // the previous session) but re-checked when the grace ends.
              if (cfg->dtr_mode == 0 || cfg->conn_type == MDM_CONN_NONE) {
                // ignore (no connection or &D0)
              } else if (cfg->conn_established > 0 &&
                         (mono_now() - cfg->conn_established) < 5) {
                LOG(LOG_INFO, "DTR drop inside the 5 s grace period after connect; will re-check");
                cfg->dtr_recheck_at = cfg->conn_established + 5;
              } else {
                dtr_dropped(cfg);
              }
              break;
            case MSG_DTR_UP:
              cfg->dtr_recheck_at = 0;
              break;
            case MSG_LE_DOWN:
              /* ip232: the emulator went away. With &D0 nothing else would
               * ever end the call, and the caller would talk to nobody. */
              if(cfg->dce_data.is_ip232 && cfg->conn_type != MDM_CONN_NONE) {
                LOG(LOG_INFO, "ip232 emulator disconnected during a call: hanging up the caller");
                mdm_disconnect(cfg, TRUE);
              }
              break;
            case MSG_SER_LOST:
              /* ctrl_thread's TIOCMGET failed. Confirm before tearing down:
               * a transient error must not drop a healthy port. */
              if(!cfg->dce_data.is_ip232 && cfg->dce_data.is_connected &&
                 dce_get_control_lines(&cfg->dce_data) < 0) {
                handle_serial_lost(cfg);
              }
              break;
            default:
              break;
          }
        }
      }
    }
    if (FD_ISSET(cfg->cp[0][0], &readfs)) {  // ip / gate / dial thread pipe
      res = readPipe(cfg->cp[0][0], buf, sizeof(buf));
      /* several one-byte messages can arrive in one read: handle each */
      for(int i = 0; i < res; i++) {
        LOG(LOG_DEBUG, "Received %c from a helper thread", buf[i]);
        switch (buf[i]) {
          case MSG_GATE_DONE:
            gate_finish(cfg);
            break;
          case MSG_DIAL_DONE:
            mdm_dial_done(cfg);
            break;
          case MSG_DISCONNECT:
            if(cfg->disc_gen != cfg->line_data.gen || cfg->line_data.is_connected != TRUE
               || cfg->gate_pending) {
              /* about a call that has already ended (we hung up while the
               * reader was reporting): must not touch the line's next call */
              LOG(LOG_DEBUG, "Stale peer-closed notification ignored");
              writePipe(cfg->cp[1][1], MSG_NOTIFY);
            } else if(cfg->direct_conn == TRUE) {
              // what should we do here...
              LOG(LOG_ERROR, "Direct Connection Link broken, disconnecting and awaiting new direct connection");
              mdm_disconnect(cfg, TRUE);
              writePipe(cfg->cp[1][1], MSG_NOTIFY);
            } else {
              mdm_disconnect(cfg, FALSE);
              writePipe(cfg->cp[1][1], MSG_NOTIFY);
            }
            break;
        }
      }
    }
    if (FD_ISSET(cfg->mp[1][0], &readfs)) {  // parent pipe
      LOG(LOG_DEBUG, "Data available on incoming IPC pipe");
      res = readPipe(cfg->mp[1][0], buf, sizeof(buf));
      for(int i = 0; i < res; i++) {
        switch (buf[i]) {
          case MSG_CALLING:       // accept connection.
            accept_connection(cfg);
            break;
          case MSG_HANGUP:        // dashboard: force hangup
            LOG(LOG_INFO, "Dashboard hangup requested");
            mdm_disconnect(cfg, TRUE);
            break;
          case MSG_RESET:         // dashboard: soft reset (AT&F equivalent)
            LOG(LOG_INFO, "Dashboard reset requested");
            mdm_disconnect(cfg, TRUE);
            /* Reset modem emulation state only — preserve pipes, serial fd, etc. */
            cfg->is_cmd_mode = TRUE;
            cfg->is_off_hook = FALSE;
            cfg->is_ringing = FALSE;
            cfg->break_len = 0;
            cfg->pre_break_delay = FALSE;
            cfg->cur_line_idx = 0;
            cfg->rings = 0;
            mdm_reset_profile(cfg);   /* same path as ATZ / AT&F */
            break;
        }
      }
    }
  }
  LOG_EXIT();
  return NULL;
}
