#include <stdio.h>

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
  time_t blocked_until;
  int permanent;
  time_t last_attempt;        /* for rate limiting */
} ip_block_t;

#define RATE_LIMIT_SECONDS 5  /* minimum seconds between attempts from same IP */
#define GATE_MAX_GUESSES_PER_CONN 3  /* wrong password tries before we drop the connection */
/* ct_streq() (constant-time compare) now lives in util.c */

#define MAX_BLOCKS 512
static ip_block_t *g_blocks = NULL;
static int g_blocks_inited = 0;
static pthread_mutex_t g_blocks_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t blocks_once = PTHREAD_ONCE_INIT;

static void init_blocks_once(void) {
  g_blocks = (ip_block_t *)calloc(MAX_BLOCKS, sizeof(ip_block_t));
  if (!g_blocks) {
    LOG(LOG_FATAL, "Could not allocate blocklist memory");
    exit(-1);
  }
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
    static char safe_path[512];
    if (validate_file_path(env_path, safe_path, sizeof(safe_path)) == 0) {
      return safe_path;
    }
    LOG(LOG_WARN, "Invalid allowlist path in TCPSER_ALLOWLIST: %s", env_path);
  }

  /* Use secure fallback paths */
  if(access("data/tcpser.allowlist", F_OK) == 0) return "data/tcpser.allowlist";
  return "tcpser.allowlist";
}

static int ip_is_allowed(const char *ip) {
  if(!ip || !ip[0]) return FALSE;
  FILE *f = fopen(allowlist_path(), "r");
  if(!f) return FALSE;
  char line[256];
  while(fgets(line, sizeof(line), f)) {
    /* Strip whitespace and skip comments */
    char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == '\n' || *p == '\0') continue;
    size_t n = strlen(p);
    while(n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';
    if(strcmp(p, ip) == 0) { fclose(f); return TRUE; }
  }
  fclose(f);
  return FALSE;
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
    static char safe_path[512];
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

static void blocklist_append_ip(const char *ip) {
  if(!ip || !ip[0]) return;

  /* Avoid duplicates: scan the file quickly (with a shared lock). */
  {
    int rfd = open(blocklist_path(), O_RDONLY);
    if(rfd >= 0) {
      if(flock(rfd, LOCK_SH) == 0) {
        FILE *rf = fdopen(rfd, "r");
        if(rf) {
          char line[256];
          while(fgets(line, sizeof(line), rf)) {
            trim_line(line);
            if(line[0] == 0 || line[0] == '#') continue;
            if(0 == strcmp(line, ip)) {
              fclose(rf); /* also closes rfd */
              return;
            }
          }
          fclose(rf);
          /* rfd closed */
        } else {
          close(rfd);
        }
      } else {
        close(rfd);
      }
    }
  }

  /* Append under an exclusive lock. */
  int wfd = open(blocklist_path(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if(wfd < 0) return;
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
  time_t now = time(NULL);
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
  for(int i = 0; i < MAX_BLOCKS; i++) {
    if(g_blocks[i].ip[0] && g_blocks[i].permanent == TRUE) {
      g_blocks[i].permanent = FALSE;
    }
  }

  FILE *f = fopen(blocklist_path(), "r");
  if(!f) return;

  char line[256];
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
    }
  }
  fclose(f);
}

static ip_block_t* find_or_alloc_ip(const char *ip) {
  ensure_blocks_init();
  int free_i = -1;
  int oldest_i = -1;
  time_t oldest_t = 0;
  for(int i = 0; i < MAX_BLOCKS; i++) {
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

  if(free_i == -1) {
    /* Table full. Evict the oldest timed/rate-limit entry so we keep tracking
     * new offenders instead of silently default-allowing them. Permanent blocks
     * are never evicted (they are also persisted to the blocklist file). */
    if(oldest_i != -1) {
      LOG(LOG_WARN, "IP blocklist full (%d), evicting oldest entry %s to track %s",
          MAX_BLOCKS, g_blocks[oldest_i].ip, ip);
      free_i = oldest_i;
    } else {
      LOG(LOG_WARN, "IP blocklist full (%d, all permanent), cannot track %s",
          MAX_BLOCKS, ip);
      return NULL;
    }
  }

  snprintf(g_blocks[free_i].ip, sizeof(g_blocks[free_i].ip), "%s", ip);
  g_blocks[free_i].fails = 0;
  g_blocks[free_i].blocked_until = 0;
  g_blocks[free_i].permanent = FALSE;
  g_blocks[free_i].last_attempt = 0;
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
    else if(e->blocked_until != 0 && time(NULL) < e->blocked_until) result = TRUE;
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
  time_t now = time(NULL);
  for(int i = 0; i < MAX_BLOCKS; i++) {
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
  time_t now = time(NULL);
  for(int i = 0; i < MAX_BLOCKS && count < max; i++) {
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

/* Clear all timed (non-permanent) blocks. Returns number cleared. */
int blocklist_unblock_all_timed(void) {
  int cleared = 0;
  pthread_mutex_lock(&g_blocks_mutex);
  ensure_blocks_init();
  for(int i = 0; i < MAX_BLOCKS; i++) {
    if(g_blocks[i].ip[0] && !g_blocks[i].permanent && g_blocks[i].blocked_until != 0) {
      g_blocks[i].ip[0] = 0;
      g_blocks[i].blocked_until = 0;
      g_blocks[i].fails = 0;
      cleared++;
    }
  }
  pthread_mutex_unlock(&g_blocks_mutex);
  return cleared;
}

/* --- PETSCII helpers ---------------------------------------------------- */
static void petscii_send_byte(int fd, unsigned char b) {
  (void)ip_write(fd, &b, 1);
}

static void petscii_repeat(int fd, unsigned char b, int n) {
  for(int i = 0; i < n; i++) (void)ip_write(fd, &b, 1);
}

static void petscii_goto(int fd, int row, int col) {
  /* row: 0-24, col: 0-39 */
  const unsigned char HOME = 19;  /* $13 */
  const unsigned char DOWN = 17;  /* $11 */
  const unsigned char RIGHT = 29; /* $1D */
  petscii_send_byte(fd, HOME);
  if(row > 0) petscii_repeat(fd, DOWN, row);
  if(col > 0) petscii_repeat(fd, RIGHT, col);
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

  petscii_send_byte(fd, CLS);
  petscii_send_byte(fd, WHITE);

  /* Header */
  petscii_goto(fd, 2, 7);
  petscii_send_byte(fd, RVS_ON);
  petscii_send_byte(fd, GREEN);
  preconnect_write(fd, "   UNBREACHED BBS GATEWAY   ");
  petscii_send_byte(fd, RVS_OFF);
  petscii_send_byte(fd, WHITE);

  /* Boot lines */
  petscii_goto(fd, 6, 4);
  petscii_send_byte(fd, YELLOW);
  preconnect_write(fd, "Loading BBS... Please Wait");
  usleep(250000);

  petscii_goto(fd, 8, 4);
  petscii_send_byte(fd, WHITE);
  preconnect_write(fd, "Enabling RS232 connection... ");
  usleep(200000);
  petscii_send_byte(fd, GREEN);
  preconnect_write(fd, "DONE");
  petscii_send_byte(fd, WHITE);

  petscii_goto(fd, 10, 4);
  petscii_send_byte(fd, WHITE);
  preconnect_write(fd, "Connecting to Commodore 64... ");
  usleep(200000);
  petscii_send_byte(fd, GREEN);
  preconnect_write(fd, "DONE");
  petscii_send_byte(fd, WHITE);

  petscii_goto(fd, 13, 4);
  petscii_send_byte(fd, GREEN);
  preconnect_write(fd, "READY.");
  petscii_send_byte(fd, WHITE);

  preconnect_write(fd, "\r\n\r\n");
}

static void send_access_granted(modem_config *cfg, int fd, int delay_us) {
  /* If user supplied a file, show it (raw bytes). Otherwise fallback to default message. */
  if(cfg && cfg->access_granted_file[0]) {
    const unsigned char CLS = 147; /* PETSCII CLR/HOME */
    petscii_send_byte(fd, CLS);
    (void)writeFile(cfg->access_granted_file, fd);
    preconnect_write(fd, "\r\n\r\n");
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

    petscii_send_byte(fd, CLS);
    petscii_send_byte(fd, GREEN);
    petscii_goto(fd, 12, 13);
    petscii_send_byte(fd, RVS_ON);
    preconnect_write(fd, "ACCESS GRANTED");
    petscii_send_byte(fd, RVS_OFF);
    petscii_send_byte(fd, WHITE);
    preconnect_write(fd, "\r\n\r\n");
    if(delay_us > 0) usleep(delay_us);
  }
}



// Very small telnet filter so a real telnet client can still satisfy the gate.
// We ignore IAC negotiation and subnegotiation sequences.
static int gate_consume_telnet(unsigned char *buf, int len, unsigned char *out, int out_max) {
  int i = 0;
  int o = 0;
  int in_sb = 0;

  while(i < len && o < out_max) {
    unsigned char ch = buf[i++];
    if(in_sb) {
      // swallow until IAC SE
      if(ch == 0xff && i < len) {
        unsigned char next = buf[i];
        if(next == 0xf0) { // SE
          i++;
          in_sb = 0;
        }
      }
      continue;
    }

    if(ch == 0xff) { // IAC
      if(i >= len) break;
      unsigned char cmd = buf[i++];
      if(cmd == 0xfa) { // SB
        in_sb = 1;
        continue;
      }
      // WILL/WONT/DO/DONT have 1 option byte
      if((cmd == 251 || cmd == 252 || cmd == 253 || cmd == 254) && i < len) {
        i++;
      }
      continue;
    }

    out[o++] = ch;
  }
  return o;
}

static int preconnect_gate(modem_config *cfg) {
  int fd = cfg->line_data.fd;
  int timeout = cfg->gate_timeout > 0 ? cfg->gate_timeout : 20;
  struct timeval start, now;
  gettimeofday(&start, NULL);

  if(cfg->gate_mode == 0) return TRUE;

  if(cfg->gate_mode == 1) {
   // preconnect_write(fd, "Press BACKSPACE twice to launch the BBS...\r\n");
  } else if(cfg->gate_mode == 2) {
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
    gettimeofday(&now, NULL);
    int elapsed = (int)(now.tv_sec - start.tv_sec);
    int remaining = timeout - elapsed;
    if(remaining <= 0) break;

    fd_set readfs;
    FD_ZERO(&readfs);
    FD_SET(fd, &readfs);
    struct timeval tv;
    tv.tv_sec = remaining;
    tv.tv_usec = 0;

    int rc = select(fd + 1, &readfs, NULL, NULL, &tv);
    if(rc <= 0) break; // timeout or error
    if(!FD_ISSET(fd, &readfs)) continue;

    unsigned char inbuf[256];
    int n = ip_read(fd, inbuf, sizeof(inbuf));
    if(n <= 0) return FALSE; // peer closed

    unsigned char filtered[256];
    int m = gate_consume_telnet(inbuf, n, filtered, sizeof(filtered));

    for(int i = 0; i < m; i++) {
      unsigned char ch = filtered[i];

      if(cfg->gate_mode == 1) {
        /* BACKSPACE / DEL: accept common variants across PETSCII terminals */
        if(ch == 0x08 || ch == 0x14 || ch == 0x7f) {
          esc_count++;
          if(esc_count >= 2) {
            send_access_granted(cfg, fd, 400000);
            return TRUE;
          }
        }
      } else if(cfg->gate_mode == 2) {
        if(ch == '\r' || ch == '\n') {
          pwbuf[pwlen] = 0;
          if(pwlen > 0) {
            norm_str_inplace(pwbuf);
            if(ct_streq(pwbuf, pwcfg)) {
              send_access_granted(cfg, fd, 700000);
              return TRUE;
            }
            /* Non-empty wrong password counts as a guess. */
            wrong_guesses++;
            if(wrong_guesses >= GATE_MAX_GUESSES_PER_CONN) {
              preconnect_write(fd, "\r\nToo many attempts. Disconnecting.\r\n");
              return FALSE;
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
  return FALSE;
}
/* Execute a script securely after a successful gate.
 * Scripts must be in TCPSER_SCRIPT_DIR and follow naming restrictions.
 * Useful for playing a sound, notifying, etc. Does not block. */
static void run_gate_success_script(modem_config *cfg) {
  if(cfg->on_gate_success_script[0] == 0) return;

  LOG(LOG_DEBUG, "Executing gate success script: %s", cfg->on_gate_success_script);

  /* Execute script securely with 30-second timeout */
  int result = execute_script_secure(cfg->on_gate_success_script,
                                     cfg->line_data.peer_ip,
                                     30);

  switch(result) {
    case SCRIPT_OK:
      LOG(LOG_DEBUG, "Gate script executed successfully");
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
int accept_connection(modem_config *cfg) {
  LOG_ENTER();

  if(-1 == line_accept(&cfg->line_data)) {
    /* Accept failed — still release the parent's accept gate so it can resume
     * (otherwise accept_pending would stay set and freeze every port). */
    writePipe(cfg->mp[0][1], MSG_BUSY);
    LOG_EXIT();
    return 0;
  }

  /* The TCP connection is now ours (cfg->line_data.fd, is_connected == TRUE).
   * Release the global accept gate IMMEDIATELY, before the slow banner/gate,
   * so a single silent caller can no longer freeze every port/modem for the
   * full gate timeout (cheapest remote DoS). The parent only routes new calls
   * to modems whose line is not already connected, so it won't re-route to us
   * while we run the gate. */
  writePipe(cfg->mp[0][1], MSG_BUSY);

  {
    int is_trusted;
    dashboard_stat_connection(cfg->line_data.peer_ip);

    /* Allowlisted IPs skip blocklist, rate limit, and gate */
    is_trusted = ip_is_allowed(cfg->line_data.peer_ip);
    if(is_trusted) {
      LOG(LOG_INFO, "Allowlisted IP %s — skipping gate", cfg->line_data.peer_ip);
      connlog_event(cfg->line_data.peer_ip, CONNLOG_CONNECTED);
      dashboard_stat_gate(1, cfg->line_data.peer_ip);
      dashboard_set_connected(1, cfg->line_data.peer_ip);
      run_gate_success_script(cfg);
      goto start_ringing;
    }

    /* Drop immediately if blocked (no banner, no gate, no serial) */
    if(ip_is_blocked(cfg->line_data.peer_ip)) {
      LOG(LOG_INFO, "Blocked IP %s tried to connect", cfg->line_data.peer_ip);
      dashboard_stat_blocked(cfg->line_data.peer_ip);
      connlog_event(cfg->line_data.peer_ip, CONNLOG_BLOCKED);
      line_disconnect(&cfg->line_data);
      LOG_EXIT();
      return 0;
    }

    /* Rate limit: enforce minimum delay between attempts from same IP */
    {
      pthread_mutex_lock(&g_blocks_mutex);
      ensure_blocks_init();
      ip_block_t *rl = find_or_alloc_ip(cfg->line_data.peer_ip);
      int too_fast = 0;
      if(rl) {
        time_t now = time(NULL);
        if(rl->last_attempt > 0 && (now - rl->last_attempt) < RATE_LIMIT_SECONDS) {
          too_fast = 1;
        }
        rl->last_attempt = now;
      }
      pthread_mutex_unlock(&g_blocks_mutex);
      if(too_fast) {
        LOG(LOG_INFO, "Rate limited IP %s (too fast)", cfg->line_data.peer_ip);
        dashboard_stat_blocked(cfg->line_data.peer_ip);
        connlog_event(cfg->line_data.peer_ip, CONNLOG_RATE_LIMIT);
        line_disconnect(&cfg->line_data);
        LOG_EXIT();
        return 0;
      }
    }

    // 1) Send banner immediately upon TCP accept (still BEFORE serial/modem traffic)
    send_preconnect_banner(cfg);

    // 2) Optional bot gate (still BEFORE serial/modem traffic)
    if(preconnect_gate(cfg) == FALSE) {
      dashboard_stat_gate(0, cfg->line_data.peer_ip);
      connlog_event(cfg->line_data.peer_ip, CONNLOG_GATE_FAIL);
      webhook_notify("GATE_FAIL", cfg->line_data.peer_ip);
      ip_note_fail(cfg->line_data.peer_ip, cfg->gate_max_fails, 600);
      line_disconnect(&cfg->line_data);
      LOG(LOG_INFO, "Pre-connect gate failed; disconnected before touching serial");
      LOG_EXIT();
      return 0;
    }

    // 3) Gate passed: run optional external hook (non-blocking)
    dashboard_stat_gate(1, cfg->line_data.peer_ip);
    connlog_event(cfg->line_data.peer_ip, CONNLOG_CONNECTED);
    dashboard_set_connected(1, cfg->line_data.peer_ip);
    run_gate_success_script(cfg);

start_ringing:
    // 4) Start a short answer timer so scanners don't hold the line forever.
    cfg->incoming_waiting_answer = TRUE;
    cfg->incoming_answer_deadline = time(NULL) + (cfg->incoming_answer_timeout > 0 ? cfg->incoming_answer_timeout : 10);
    LOG(LOG_INFO, "Gate passed for %s; waiting for local answer (%ds)",
        cfg->line_data.peer_ip, (cfg->incoming_answer_timeout > 0 ? cfg->incoming_answer_timeout : 10));

    /* Set grace period NOW so DTR drops during RING phase are ignored */
    cfg->conn_established = time(NULL);

    if(cfg->direct_conn == TRUE) {
      cfg->conn_type = MDM_CONN_INCOMING;
      mdm_off_hook(cfg);
    } else {
      cfg->rings = 0;
      mdm_send_ring(cfg);
    }
    /* Note: the parent's accept gate was already released right after
     * line_accept() above, so there is no MSG_BUSY to send here. */
  }
  LOG_EXIT();
  return 0;
}

int parse_ip_data(modem_config *cfg, unsigned char *data, int len) {
  // I'm going to cheat and assume it comes in chunks.
  int i = 0;
  unsigned char ch;
  unsigned char text[1025];
  int text_len = 0;

  if(cfg->line_data.is_data_received == FALSE) {
    cfg->line_data.is_data_received = TRUE;
    if((data[0] == 0xff) || (data[0] == 0x1a)) {
      LOG(LOG_INFO, "Detected telnet client");
      cfg->line_data.is_telnet = TRUE;
      /* Send full RFC 854 proactive negotiation batch:
       * WILL ECHO, WILL/DO SGA, DO NAWS, DO TERMINAL_TYPE, binary mode.
       * This handles Linux telnet, PuTTY, SyncTERM, CGTerm, etc. */
      nvt_negotiate_proactive(cfg->line_data.fd, &cfg->line_data.nvt_data);
    }
  }

  if(cfg->line_data.is_telnet == TRUE) {
    /* Binary mode: if parity is set, retract binary offer.
     * Proactive negotiation already sent WILL/DO BINARY, so we only
     * need to send WONT/DONT if the connection isn't 8-bit clean. */
    if(cfg->is_binary_negotiated == FALSE) {
      if(dce_get_parity(&cfg->dce_data)) {
        LOG(LOG_INFO, "Telnet: parity set, retracting binary mode");
        send_nvt_command(cfg->line_data.fd,
                         &cfg->line_data.nvt_data,
                         NVT_WONT,
                         NVT_OPT_TRANSMIT_BINARY
                        );
        send_nvt_command(cfg->line_data.fd,
                         &cfg->line_data.nvt_data,
                         NVT_DONT,
                         NVT_OPT_TRANSMIT_BINARY
                        );
      }
      cfg->is_binary_negotiated = TRUE;
    }
    while(i < len) {
      ch = data[i];
      if(NVT_IAC == ch) {
        // Ensure we don't read past the end on truncated IAC sequences
        if (i + 1 >= len) {
          break;
        }
        ch = data[i + 1];
        switch(ch) {
          case NVT_WILL:
          case NVT_DO:
          case NVT_WONT:
          case NVT_DONT:
            // Ensure option byte exists
            if (i + 2 >= len) {
              i = len;
              break;
            }
            LOG(LOG_INFO, "Parsing nvt command");
            parse_nvt_command(&cfg->dce_data,
                              cfg->line_data.fd,
                              &cfg->line_data.nvt_data,
                              ch,
                              data[i + 2]
                             );
            i += 3;
            break;
          case NVT_SB:      // sub negotiation
            // again, overflow...
            i += parse_nvt_subcommand(&cfg->dce_data, 
                                      cfg->line_data.fd, 
                                      &cfg->line_data.nvt_data, 
                                      data + i, 
                                      len - i
                                     );
            break;
          case NVT_IAC:
            if (cfg->line_data.nvt_data.binary_recv) {
              if (text_len >= 1024) {
                text[text_len] = 0;
                dce_write(&cfg->dce_data, text, text_len);
                text_len = 0;
              }
              text[text_len++] = NVT_IAC;
            }
            /* fall through */
          default:
            // ignore...
            i += 2;
        }
      } else {
        text[text_len++] = data[i++];
      }
      if(text_len == 1024) {
        text[text_len] = 0;
        // write to serial...
        mdm_write(cfg, text, text_len);
        text_len = 0;
      }
    }
    if(text_len) {
      text[text_len] = 0;
      // write to serial...
      mdm_write(cfg, text, text_len);
    }
  } else {
    mdm_write(cfg, data, len);
  }
  return 0;
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
    FD_ZERO(&readfs);
    FD_SET(cfg->cp[1][0], &readfs);
    max_fd = cfg->cp[1][0];
    if(action_pending == FALSE
       && cfg->conn_type != MDM_CONN_NONE
       && cfg->is_cmd_mode == FALSE
       && cfg->line_data.fd > -1
       && cfg->line_data.is_connected == TRUE
      ) {
      FD_SET(cfg->line_data.fd, &readfs); 
      max_fd=MAX(max_fd, cfg->line_data.fd);
    }
    max_fd++;
    rc = select(max_fd, &readfs, NULL, NULL, NULL);
    if(rc == -1) {
      ELOG(LOG_WARN, "Select returned error");
      // handle error
    } else {
      // we got data
      if (cfg->line_data.is_connected == TRUE && FD_ISSET(cfg->line_data.fd, &readfs)) {  // socket
        LOG(LOG_DEBUG, "Data available on socket");
        res = line_read(&cfg->line_data, buf, sizeof(buf));
        //res = recv(cfg->line_data.fd, buf, sizeof(buf), 0);
        if(0 >= res) {
          LOG(LOG_INFO, "No socket data read, assume closed peer");
          writePipe(cfg->cp[0][1], MSG_DISCONNECT);
          action_pending = TRUE;
        } else {
          LOG(LOG_DEBUG, "Read %d bytes from socket", res);
          buf[res] = 0;
          parse_ip_data(cfg, buf, res);
        }
      }
      if (FD_ISSET(cfg->cp[1][0], &readfs)) {  // pipe

        res = readPipe(cfg->cp[1][0], buf, sizeof(buf) - 1);
        LOG(LOG_DEBUG, "IP thread notified");
        action_pending = FALSE;
      }
    }
  }
  LOG_EXIT();
}

void *ctrl_thread(void *arg) {
  modem_config* cfg = (modem_config *)arg;
  int status;
  int new_status;

  LOG_ENTER();
  status = dce_get_control_lines(&cfg->dce_data);
  while(status > -1) {
    new_status = dce_check_control_lines(&cfg->dce_data);
    if(new_status > -1 && status != new_status) {
      LOG(LOG_DEBUG, "Control Line Change");
      if((new_status & DCE_CL_DTR) != (status & DCE_CL_DTR)) {
        if((new_status & DCE_CL_DTR)) {
          LOG(LOG_INFO, "DTR has gone high");
          writePipe(cfg->wp[0][1], MSG_DTR_UP);
        } else {
          LOG(LOG_INFO, "DTR has gone low");
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
  // need to quit application, as status cannot be obtained.
  exit(-1);
}

/* Try to reconnect a lost serial port (USB adapter unplugged/glitch).
 * Closes the old fd, waits for the device to reappear, and reopens it.
 * Returns 0 on success, -1 if the device never came back.
 */
static int serial_reconnect(dce_config *dce) {
  if(dce->is_ip232) return -1; /* only for real serial ports */

  LOG(LOG_WARN, "Serial port %s lost — attempting reconnect", dce->tty);

  if(dce->fd >= 0) { close(dce->fd); dce->fd = -1; }
  dce->is_connected = FALSE;

  /* Wait up to 60 seconds for the device to reappear */
  for(int attempt = 0; attempt < 30; attempt++) {
    usleep(2000000); /* 2 seconds */
    if(access(dce->tty, R_OK | W_OK) == 0) {
      int rc = dce_connect(dce);
      if(rc >= 0) {
        LOG(LOG_INFO, "Serial port %s reconnected (attempt %d)", dce->tty, attempt + 1);
        return 0;
      }
    }
    LOG(LOG_DEBUG, "Waiting for %s... (%d/30)", dce->tty, attempt + 1);
  }
  LOG(LOG_FATAL, "Serial port %s did not reappear after 60s", dce->tty);
  return -1;
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
  if(dce_connect(&cfg->dce_data) < 0) {
    ELOG(LOG_FATAL, "Could not open serial port %s", cfg->dce_data.tty);
    exit(-1);
  }

  /* Give dashboard access to modem config for interactive commands */
  dashboard_set_serial_fd(cfg->dce_data.fd);
  dashboard_set_modem_cfg((void *)cfg);

  spawn_thread((void *)ctrl_thread, (void *)cfg, "CTRL");
  spawn_thread((void *)ip_thread, (void *)cfg, "IP");

  mdm_set_control_lines(cfg);
  last_conn_type = cfg->conn_type;
  cfg->allow_transmit = FALSE;
  // call some functions behind the scenes
  if(cfg->cur_line_idx) {
    mdm_parse_cmd(cfg);
  }
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
        cfg->conn_type = MDM_CONN_OUTGOING;
      }
    }
  }
  cfg->allow_transmit = TRUE;
  for(;;) {
    if(last_conn_type != cfg->conn_type) {
      LOG(LOG_ALL, "Connection status change, handling");
      writePipe(cfg->cp[1][1], MSG_NOTIFY);

      /* Watchdog: start timer when connection drops, clear when connected */
      if(cfg->conn_type == MDM_CONN_NONE && last_conn_type != MDM_CONN_NONE) {
        webhook_notify("DISCONNECTED", cfg->line_data.peer_ip);
        if(cfg->watchdog_timeout > 0) {
          watchdog_start = time(NULL);
          watchdog_dtr_attempts = 0;
          LOG(LOG_INFO, "BBS watchdog started (%ds timeout)", cfg->watchdog_timeout);
        }
      } else if(cfg->conn_type != MDM_CONN_NONE) {
        webhook_notify("CONNECTED", cfg->line_data.peer_ip);
        watchdog_start = 0;
        watchdog_dtr_attempts = 0;
        consecutive_no_answer = 0;  /* BBS answered, reset counter */
      }

      if(cfg->conn_type == MDM_CONN_OUTGOING) {
        if(strlen(cfg->local_connect) > 0) {
          writeFile(cfg->local_connect, cfg->line_data.fd);
        }
        if(strlen(cfg->remote_connect) > 0) {
          writeFile(cfg->remote_connect, cfg->line_data.fd);
        }
      } else if(cfg->conn_type == MDM_CONN_INCOMING) {
        if(strlen(cfg->local_answer) > 0) {
          writeFile(cfg->local_answer, cfg->line_data.fd);
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
    if(cfg->dce_data.is_connected) {
      max_fd = MAX(max_fd, cfg->dce_data.fd);
      FD_SET(cfg->dce_data.fd, &readfs);
    }
    max_fd = MAX(max_fd, cfg->wp[0][0]);
    FD_SET(cfg->wp[0][0], &readfs);
    max_fd = MAX(max_fd, cfg->cp[0][0]);
    FD_SET(cfg->cp[0][0], &readfs);
    ptimer = NULL;
    if(cfg->is_cmd_mode == FALSE) {
      if(cfg->pre_break_delay == FALSE || cfg->break_len == 3) {
        LOG(LOG_ALL, "Setting timer for break delay");
        long long usec;
        usec = cfg->s[S_REG_GUARD_TIME] * 20000;
        timer.tv_sec = usec / 1000000;
        timer.tv_usec = usec % 1000000;
        ptimer = &timer;
      } else if(cfg->pre_break_delay == TRUE && cfg->break_len > 0) {
        LOG(LOG_ALL, "Setting timer for inter-break character delay");
        timer.tv_sec = 1;   // 1 second
        timer.tv_usec = 0;
        ptimer = &timer;
      } else if (cfg->s[30] != 0) {
        LOG(LOG_ALL, "Setting timer for inactivity delay");
        timer.tv_sec = cfg->s[S_REG_INACTIVITY_TIME] * 10;
        timer.tv_usec = 0;
        ptimer = &timer;
      }
    } else if(cfg->is_cmd_mode == TRUE
              && cfg->conn_type == MDM_CONN_NONE
              && cfg->line_data.is_connected == TRUE
             ) {
        LOG(LOG_ALL, "Setting timer for rings");
        timer.tv_sec = 4;
        timer.tv_usec = 0;
        ptimer = &timer;
    }
    /* If watchdog is active and no shorter timer is set, use a 5s poll */
    if(watchdog_start > 0 && ptimer == NULL) {
      timer.tv_sec = 5;
      timer.tv_usec = 0;
      ptimer = &timer;
    }
    max_fd++;
    rc = select(max_fd, &readfs, NULL, NULL, ptimer);
    if(rc == -1) {
      if(errno == EBADF && !cfg->dce_data.is_ip232) {
        LOG(LOG_WARN, "Select EBADF — serial port may be lost");
        if(serial_reconnect(&cfg->dce_data) < 0) {
          LOG(LOG_FATAL, "Serial port permanently lost, exiting");
          exit(-1);
        }
        mdm_set_control_lines(cfg);
        continue;
      }
      ELOG(LOG_WARN, "Select returned error");
      /* readfs is undefined after an error return — do NOT fall through to the
       * FD_ISSET checks below (EINTR etc. land here too). Re-loop instead. */
      continue;
    } else if(rc == 0) {
      // timer popped.
      if(cfg->is_cmd_mode == TRUE
         && cfg->conn_type == MDM_CONN_NONE
         && cfg->line_data.is_connected == TRUE
        ) {
        // If this incoming call was gated, enforce a short "must answer" window.
        if(cfg->incoming_waiting_answer == TRUE && cfg->incoming_answer_deadline != 0) {
          time_t now = time(NULL);
          if(now >= cfg->incoming_answer_deadline) {
            // not answered in time, send no-answer message/file to TCP side and disconnect.
            if(strlen(cfg->no_answer) == 0) {
              line_write(&cfg->line_data, (unsigned char *)MDM_NO_ANSWER, strlen(MDM_NO_ANSWER));
            } else {
              writeFile(cfg->no_answer, cfg->line_data.fd);
            }
            cfg->incoming_waiting_answer = FALSE;
            cfg->incoming_answer_deadline = 0;
            cfg->is_ringing = FALSE;
            consecutive_no_answer++;
            LOG(LOG_INFO, "Answer timeout (%d consecutive no-answers)", consecutive_no_answer);
            if(consecutive_no_answer >= 3 && cfg->watchdog_timeout > 0) {
              LOG(LOG_WARN, "BBS appears hung — %d consecutive no-answers, toggling DTR",
                  consecutive_no_answer);
              dce_set_control_lines(&cfg->dce_data, 0);
              usleep(500000);
              mdm_set_control_lines(cfg);
              consecutive_no_answer = 0;
            }
            dashboard_set_connected(0, NULL);
            connlog_event(cfg->line_data.peer_ip, CONNLOG_NO_ANSWER);
            line_disconnect(&cfg->line_data);
            continue;
          }
        }

        if(cfg->s[0] == 0 && cfg->rings == 10) {
          // not going to answer, send some data back to IP and disconnect.
          if(strlen(cfg->no_answer) == 0) {
            line_write(&cfg->line_data, (unsigned char *)MDM_NO_ANSWER, strlen(MDM_NO_ANSWER));
          } else {
            writeFile(cfg->no_answer, cfg->line_data.fd);
          }
          cfg->is_ringing = FALSE;
          consecutive_no_answer++;
          LOG(LOG_INFO, "No answer (%d consecutive)", consecutive_no_answer);
          if(consecutive_no_answer >= 3 && cfg->watchdog_timeout > 0) {
            LOG(LOG_WARN, "BBS appears hung — %d consecutive no-answers, toggling DTR",
                consecutive_no_answer);
            dce_set_control_lines(&cfg->dce_data, 0);  /* DTR low (DCD low to C64) */
            usleep(500000);
            mdm_set_control_lines(cfg);                 /* restore normal state */
            consecutive_no_answer = 0;
          }
          dashboard_set_connected(0, NULL);
          connlog_event(cfg->line_data.peer_ip, CONNLOG_NO_ANSWER);
        } else
          mdm_send_ring(cfg);
      } else
        mdm_handle_timeout(cfg);
    }
    /* BBS watchdog: no serial data since disconnect */
    if(watchdog_start > 0 && cfg->conn_type == MDM_CONN_NONE
       && !cfg->line_data.is_connected) {
      time_t elapsed = time(NULL) - watchdog_start;
      if(elapsed >= cfg->watchdog_timeout) {
        watchdog_dtr_attempts++;
        LOG(LOG_WARN, "BBS watchdog fired: no serial data for %lds, toggling DTR (attempt %d)",
            (long)elapsed, watchdog_dtr_attempts);
        /* Toggle DCD/DTR low for 500ms to signal carrier loss to 6551 */
        dce_set_control_lines(&cfg->dce_data, 0);
        usleep(500000);
        mdm_set_control_lines(cfg);  /* restore normal lines */
        if(watchdog_dtr_attempts >= 3) {
          /* After 3 attempts, send a break signal as last resort */
          LOG(LOG_WARN, "BBS watchdog: 3 DTR toggles failed, sending BREAK");
          tcsendbreak(cfg->dce_data.fd, 0);
          watchdog_dtr_attempts = 0;
        }
        watchdog_start = time(NULL);  /* reset for next attempt */
      }
    }
    /* Max session time enforcement */
    if(cfg->max_session_minutes > 0 && cfg->conn_type != MDM_CONN_NONE
       && cfg->conn_established > 0) {
      long session_secs = (long)(time(NULL) - cfg->conn_established);
      if(session_secs >= cfg->max_session_minutes * 60) {
        LOG(LOG_INFO, "Max session time reached (%d min), disconnecting",
            cfg->max_session_minutes);
        mdm_disconnect(cfg, FALSE);
      }
    }
    if (FD_ISSET(cfg->dce_data.fd, &readfs)) {  // serial port
      LOG(LOG_DEBUG, "Data available on serial port");
      res = mdm_read(cfg, buf, sizeof(buf));
      if(res < 0 && !cfg->dce_data.is_ip232) {
        /* Serial port error — USB adapter may have disconnected */
        if(serial_reconnect(&cfg->dce_data) < 0) {
          LOG(LOG_FATAL, "Serial port permanently lost, exiting");
          exit(-1);
        }
        mdm_set_control_lines(cfg);
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
          // this handles the case where atdt/ata goes off hook, but no
          // connection
          mdm_disconnect(cfg, FALSE);
        } else {
          mdm_parse_data(cfg, buf, res);
        }
      }
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
              // 0 = ignore, 1 = command mode (keep carrier), 2 = hang up
              // IMPORTANT: Only act on DTR drop if there's an active connection.
              // During modem init, the BBS toggles DTR as part of its reset
              // sequence — this is NOT a hang-up request.
              // Grace period: ignore DTR drops within 3 seconds of connection
              // to avoid race with BBS reset sequence from previous session.
              if (cfg->dtr_mode == 0 || cfg->conn_type == MDM_CONN_NONE) {
                // ignore (no connection or &D0)
              } else if (cfg->conn_established > 0 &&
                         (time(NULL) - cfg->conn_established) < 5) {
                LOG(LOG_INFO, "DTR drop ignored (within 3s grace period after connect)");
              } else if (cfg->dtr_mode == 1) {
                cfg->is_cmd_mode = TRUE;
              } else {
                mdm_disconnect(cfg, FALSE);
              }
              break;
            case MSG_DTR_UP:
              // no-op by default
              break;
            default:
              break;
          }
        }
      }
    }
    if (FD_ISSET(cfg->cp[0][0], &readfs)) {  // ip thread pipe
      res = readPipe(cfg->cp[0][0], buf, sizeof(buf));
      LOG(LOG_DEBUG, "Received %c from ip thread", buf[0]);
      switch (buf[0]) {
        case MSG_DISCONNECT:
          if(cfg->direct_conn == TRUE) {
            // what should we do here...
            LOG(LOG_ERROR, "Direct Connection Link broken, disconnecting and awaiting new direct connection");
            mdm_disconnect(cfg, TRUE);
          } else {
            mdm_disconnect(cfg, FALSE);
          }
          break;
      }
    }
    if (FD_ISSET(cfg->mp[1][0], &readfs)) {  // parent pipe
      LOG(LOG_DEBUG, "Data available on incoming IPC pipe");
      res = readPipe(cfg->mp[1][0], buf, sizeof(buf));
      if(res <= 0) continue; /* pipe error, retry */
      switch (buf[0]) {
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
          cfg->send_responses = TRUE;
          cfg->connect_response = 0;
          cfg->response_code_level = 4;
          cfg->text_responses = TRUE;
          cfg->is_echo = TRUE;
          cfg->is_cmd_mode = TRUE;
          cfg->is_off_hook = FALSE;
          cfg->is_ringing = FALSE;
          cfg->dtr_mode = 2;
          cfg->break_len = 0;
          cfg->pre_break_delay = FALSE;
          cfg->cur_line_idx = 0;
          cfg->rings = 0;
          for(int j = 0; j < 100; j++) cfg->s[j] = 0;
          cfg->s[S_REG_BREAK] = 43; cfg->s[S_REG_CR] = 13;
          cfg->s[S_REG_LF] = 10; cfg->s[S_REG_BS] = 8;
          cfg->s[S_REG_BLIND_WAIT] = 2; cfg->s[S_REG_CARRIER_WAIT] = 50;
          cfg->s[S_REG_COMMA_PAUSE] = 2; cfg->s[S_REG_CARRIER_TIME] = 6;
          cfg->s[S_REG_CARRIER_LOSS] = 14; cfg->s[S_REG_DTMF_TIME] = 95;
          cfg->s[S_REG_GUARD_TIME] = 50;
          cfg->crlf[0] = 13; cfg->crlf[1] = 10; cfg->crlf[2] = 0;
          mdm_set_control_lines(cfg);
          break;
      }
    }
  }
  LOG_EXIT();
  return NULL;
}

