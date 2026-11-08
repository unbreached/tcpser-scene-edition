/*
 * TCPSER ELiTE Dashboard v2
 *
 * Full-screen retro terminal dashboard with:
 * - Modem-style activity LEDs
 * - Hex + ASCII traffic view (filtered: only CALLER<->BBS)
 * - Connection history
 * - Interactive keyboard controls
 * - Adapts to terminal size
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dashboard.h"
#include "tui.h"
#include <sys/time.h>

#include "version.h"
#include "modem_core.h"
#include "bridge.h"
#include "util.h"

dashboard_state_t g_dash;
static pthread_mutex_t dash_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct termios g_orig_term;
static int g_term_raw = 0;

/* interactive state (all access under dash_mutex or from the main thread) */
static int g_quit_requested = 0;
static char g_confirm = 0;     /* pending destructive hotkey awaiting y/N */
static time_t g_confirm_at = 0;
#define CONFIRM_TIMEOUT 10     /* seconds before a pending y/N prompt auto-cancels */
static void series_call(void);
static void series_fail(void);
static void series_bytes(int in, int len);
void term_restore(void);

static void term_raw_on(void) {
  if(g_term_raw) return;
  /* -M needs a real terminal on both stdin (keys) and stderr (drawing).
   * Under systemd / a pipe we would otherwise spew ANSI into a file and block
   * the main loop on read(STDIN). Fall back to headless capture (-H still works). */
  if(!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
    fprintf(stderr, "  -M: stdin/stderr is not a terminal; running the monitor headless (use -H for the web dashboard).\n");
    return;
  }
  struct termios t;
  if(tcgetattr(STDIN_FILENO, &g_orig_term) != 0) return;
  atexit(term_restore);   /* any exit path leaves the terminal usable */
  t = g_orig_term;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &t);
  g_term_raw = 1;
  tui_init();
}

void term_restore(void) {
  if(g_term_raw) {
    tui_shutdown();
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    g_term_raw = 0;
  }
}
static char g_stats_path[512] = {0};
static time_t g_last_autosave = 0;
#define AUTOSAVE_INTERVAL 60  /* seconds */

/* ---- terminal size / raw mode ------------------------------------------ */
static int W = 80, H = 24;
static volatile int g_winch = 0;
static void handle_winch(int sig) { (void)sig; g_winch = 1; }

static void term_raw_on(void);
void term_restore(void);

/* ---- UI state (protected by dash_mutex like everything else) ----------- */
enum { PG_MAIN = 0, PG_BLOCKS, PG_HELP };
static int g_page = PG_MAIN;
static char g_msg[128] = "";
static time_t g_msg_at = 0;
static struct timeval g_last_frame = {0, 0};

static void set_msg(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
  va_end(ap);
  g_msg_at = time(NULL);
}

/* ---- formatting helpers ------------------------------------------------- */
static const char *fmt_bytes(unsigned long b, char *buf, int sz) {
  if(b < 1024)         snprintf(buf, sz, "%luB", b);
  else if(b < 1048576) snprintf(buf, sz, "%.1fK", (double)b / 1024.0);
  else                 snprintf(buf, sz, "%.1fM", (double)b / 1048576.0);
  return buf;
}
static const char *fmt_uptime(time_t start, char *buf, int sz) {
  long s = (long)(time(NULL) - start);
  long d=s/86400, h=(s%86400)/3600, m=(s%3600)/60;
  if(d > 0) snprintf(buf, sz, "%ldD %02ld:%02ld", d, h, m);
  else      snprintf(buf, sz, "%02ld:%02ld:%02ld", h, m, s % 60);
  return buf;
}
static const char *fmt_dur(int secs, char *buf, int sz) {
  if(secs < 0) secs = 0;
  if(secs < 3600) snprintf(buf, sz, "%02d:%02d", secs / 60, secs % 60);
  else            snprintf(buf, sz, "%d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
  return buf;
}
static const char *fmt_clock(time_t t, char *buf, int sz) {
  struct tm tmb; if(!localtime_r(&t, &tmb)) { snprintf(buf, sz, "--:--:--"); return buf; }
  strftime(buf, sz, "%H:%M:%S", &tmb); return buf;
}
static const char *result_str(int r) {
  switch(r) { case 0: return "BLOCKED"; case 1: return "GATE FAIL"; case 2: return "GATE PASS"; case 3: return "CONNECTED"; default: return "?"; }
}
static int result_color(int r) { return r == 3 || r == 2 ? TC_OK : r == 1 ? TC_WARN : TC_BAD; }
/* a traffic byte as text: 7-bit, lowercase folded to uppercase (PETSCII set 1), controls as '.' */
static char text_byte(unsigned char c) {
  c &= 0x7f;
  if(c >= 0x61 && c <= 0x7a) c = (unsigned char)(c - 0x20);
  return (c >= 0x20 && c < 0x7f) ? (char)c : '.';
}
/* pad by display cells, not bytes (UTF-8 arrows/dots are one cell) */
static int cells_of(const char *s) { int n = 0; for(; *s; s++) if(((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static void put_padded(int row, int col, int color, const char *s, int width) {
  tui_putn(row, col, color, s, width);
  int n = cells_of(s); if(n > width) n = width;
  if(n < width) tui_fill(row, col + n, width - n, color, " ");
}
static void put_right(int row, int col, int color, const char *s, int width) {
  char buf[256]; snprintf(buf, sizeof(buf), "%*.*s", width, width, s); tui_putn(row, col, color, buf, width);
}

/* ---- pages ---------------------------------------------------------------- */
static void draw_lines_box(int top, int *next) {
  int n = 0;
  for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited) n++;
  if(n == 0) n = 1;
  int h = n + 3;
  tui_box(top, 0, h, W, "LINES");
  /* column table: {offset, width}; the header is built from the same table so
   * it cannot drift from the rows. Total 78 cells: fits an 80-column terminal. */
  enum { C_LN = 0, C_DEV = 3, C_SPD = 18, C_ST = 24, C_CALL = 33, C_DUR = 50, C_IN = 56, C_OUT = 63, C_DCD = 71, C_DTR = 75 };
  const int X = 2;
  tui_put(top + 1, X + C_LN, TC_DIM, "LN"); tui_put(top + 1, X + C_DEV, TC_DIM, "DEVICE"); tui_put(top + 1, X + C_SPD, TC_DIM, "SPEED");
  tui_put(top + 1, X + C_ST, TC_DIM, "STATE"); tui_put(top + 1, X + C_CALL, TC_DIM, "CALLER"); put_right(top + 1, X + C_DUR, TC_DIM, "DUR", 5);
  put_right(top + 1, X + C_IN, TC_DIM, "IN", 6); put_right(top + 1, X + C_OUT, TC_DIM, "OUT", 7); tui_put(top + 1, X + C_DCD, TC_DIM, "DCD"); tui_put(top + 1, X + C_DTR, TC_DIM, "DTR");
  int r = top + 2, shown = 0;
  char b1[32], line[64];
  for(int i = 0; i < DASH_MAX_LINES; i++) {
    dash_line_t *l = &g_dash.lines[i];
    if(!l->inited) continue;
    snprintf(line, sizeof(line), "%2d", i); tui_put(r, X + C_LN, TC_VALUE, line);
    put_padded(r, X + C_DEV, TC_VALUE, l->device, 14);
    snprintf(line, sizeof(line), "%5d", l->speed); tui_put(r, X + C_SPD, TC_VALUE, line);
    put_padded(r, X + C_ST, l->connected ? TC_ACCENT : TC_DIM, l->connected ? "CARRIER" : "IDLE", 8);
    if(l->connected) {
      put_padded(r, X + C_CALL, TC_VALUE, l->peer_ip, 16);
      put_right(r, X + C_DUR, TC_VALUE, fmt_dur((int)(time(NULL) - l->session_start), b1, sizeof(b1)), 5);
      put_right(r, X + C_IN, TC_VALUE, fmt_bytes(l->bytes_in, b1, sizeof(b1)), 6);
      put_right(r, X + C_OUT, TC_VALUE, fmt_bytes(l->bytes_out, b1, sizeof(b1)), 7);
    }
    tui_put(r, X + C_DCD + 1, l->connected ? TC_OK : TC_DIM, tui_glyph(l->connected ? G_DOT_ON : G_DOT_OFF));
    tui_put(r, X + C_DTR + 1, l->dtr ? TC_OK : TC_DIM, tui_glyph(l->dtr ? G_DOT_ON : G_DOT_OFF));
    r++; shown++;
  }
  if(!shown) tui_put(r, 2, TC_DIM, "NO LINES CONFIGURED");
  *next = top + h;
}

static void draw_traffic_box(int top, int h) {
  if(h < 3) return;
  tui_box(top, 0, h, W, "TRAFFIC");
  int rows = h - 3;
  int dir_w = 11, size_w = 5;
  int avail = W - 2 - dir_w - 1 - size_w - 4;     /* borders, dir, gap, size, separators */
  int hex_bytes = avail / 4; if(hex_bytes > 32) hex_bytes = 32; if(hex_bytes < 4) hex_bytes = 4;
  int hex_w = hex_bytes * 3 - 1, txt_w = hex_bytes;
  int col_dir = 2, col_hex = col_dir + dir_w + 1, col_txt = col_hex + hex_w + 2, col_size = W - 2 - size_w;
  tui_put(top + 1, col_dir, TC_DIM, "DIRECTION");
  tui_put(top + 1, col_hex, TC_DIM, "HEX");
  tui_put(top + 1, col_txt, TC_DIM, "TEXT");
  put_right(top + 1, col_size, TC_DIM, "SIZE", size_w);
  int show = g_dash.traffic_count < DASH_TRAFFIC_LINES ? g_dash.traffic_count : DASH_TRAFFIC_LINES;
  if(show > rows) show = rows;
  for(int r = 0; r < show; r++) {
    int idx = (g_dash.traffic_head - show + r + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
    dash_traffic_line_t *t = &g_dash.traffic[idx];
    int row = top + 2 + r;
    int in = (t->type == 16);
    char dir[24]; snprintf(dir, sizeof(dir), in ? "CALLER%sBBS" : "BBS%sCALLER", tui_glyph(G_ARROW_R));
    put_padded(row, col_dir, in ? TC_ACCENT : TC_ACCENT2, dir, dir_w);
    int nb = t->rawlen; if(nb > hex_bytes) nb = hex_bytes;
    char hex[128], txt[64]; int hp = 0;
    for(int i = 0; i < nb; i++) { hp += snprintf(hex + hp, sizeof(hex) - (size_t)hp, "%02X%s", t->raw[i], i + 1 < nb ? " " : ""); txt[i] = text_byte(t->raw[i]); }
    txt[nb] = 0;
    put_padded(row, col_hex, TC_LABEL, hex, hex_w);
    put_padded(row, col_txt, TC_VALUE, txt, txt_w);
    char sz[16]; snprintf(sz, sizeof(sz), "%d", t->bytes);
    put_right(row, col_size, TC_DIM, sz, size_w);
  }
  if(show == 0) tui_put(top + 2, 2, TC_DIM, "NO TRAFFIC YET");
}

static void draw_callers_box(int top, int h) {
  if(h < 3) return;
  tui_box(top, 0, h, W, "RECENT CALLERS");
  tui_put(top + 1, 2, TC_DIM, "TIME     CALLER           RESULT     DUR      IN     OUT");
  int rows = h - 3;
  int hshow = g_dash.history_count < DASH_HISTORY_LINES ? g_dash.history_count : DASH_HISTORY_LINES;
  if(hshow > rows) hshow = rows;
  char b1[32], b2[32], b3[32], line[160];
  for(int r = 0; r < hshow; r++) {
    int idx = (g_dash.history_head - 1 - r + DASH_HISTORY_LINES) % DASH_HISTORY_LINES;   /* newest first */
    dash_history_entry_t *e = &g_dash.history[idx];
    int row = top + 2 + r;
    snprintf(line, sizeof(line), "%s %-16.16s", fmt_clock(e->connect_time, b1, sizeof(b1)), e->ip);
    tui_put(row, 2, TC_VALUE, line);
    put_padded(row, 28, result_color(e->result), result_str(e->result), 10);
    if(e->result >= 2) {
      snprintf(line, sizeof(line), "%5s %6s %7s", fmt_dur(e->duration, b1, sizeof(b1)),
               fmt_bytes((unsigned long)e->bytes_in, b2, sizeof(b2)), fmt_bytes((unsigned long)e->bytes_out, b3, sizeof(b3)));
      tui_put(row, 39, TC_VALUE, line);
    }
  }
  if(hshow == 0) tui_put(top + 2, 2, TC_DIM, "NO CALLS YET");
}

static void draw_events_box(int top, int h) {
  if(h < 3) return;
  tui_box(top, 0, h, W, "EVENTS");
  int rows = h - 2;
  int n = g_dash.events_count < DASH_EVENTS ? g_dash.events_count : DASH_EVENTS;
  if(n > rows) n = rows;
  char b1[32], line[200];
  for(int i = 0; i < n; i++) {
    int idx = (g_dash.events_head - 1 - i + DASH_EVENTS) % DASH_EVENTS;
    snprintf(line, sizeof(line), "%s ", fmt_clock(g_dash.events[idx].when, b1, sizeof(b1)));
    tui_put(top + 1 + i, 2, TC_DIM, line);
    put_padded(top + 1 + i, 11, TC_VALUE, g_dash.events[idx].text, W - 13);
  }
  if(n == 0) tui_put(top + 1, 2, TC_DIM, "NOTHING YET");
}

static void draw_blocklist_page(int top, int h) {
  tui_box(top, 0, h, W, "BLOCKLIST");
  block_info_t bl[64];
  int n = blocklist_list_ex(bl, 64);
  tui_put(top + 1, 2, TC_DIM, "IP               KIND       UNTIL");
  int rows = h - 3; if(n > rows) n = rows;
  char b1[32], line[128];
  for(int i = 0; i < n; i++) {
    snprintf(line, sizeof(line), "%-16.16s ", bl[i].ip);
    tui_put(top + 2 + i, 2, TC_VALUE, line);
    tui_put(top + 2 + i, 19, bl[i].permanent ? TC_BAD : TC_WARN, bl[i].permanent ? "PERMANENT " : "TIMED     ");
    if(!bl[i].permanent) tui_put(top + 2 + i, 30, TC_VALUE, fmt_clock(bl[i].until, b1, sizeof(b1)));
  }
  if(n == 0) tui_put(top + 2, 2, TC_DIM, "NOTHING BLOCKED");
  tui_put(top + h - 2, 2, TC_DIM, "U=CLEAR TIMED BLOCKS   S OR ESC=BACK");
}

static void draw_help_page(int top, int h) {
  tui_box(top, 0, h, W, "HELP");
  static const char *lines[] = {
    "H  HANG UP THE CURRENT CALLER (ASKS Y/N)",
    "R  RESET THE MODEM EMULATION (ATZ)",
    "B  BLOCKLIST PAGE        U  CLEAR TIMED BLOCKS (ASKS Y/N)",
    "P  PHOSPHOR GREEN / AMBER / WHITE",
    "S  BACK TO THE MAIN SCREEN / REPAINT",
    "Q  QUIT TCPSER (ASKS Y/N): HANGS UP CALLERS, SAVES STATS",
    "",
    "TERMINAL: 60X16 MINIMUM; TRAFFIC TAKES EVERY SPARE ROW.",
    "COLOURS: 256 / 16 / MONO FROM $TERM; NO_COLOR AND TCPSER_TUI_ASCII",
    "ARE HONOURED; TCPSER_TUI_PHOSPHOR=GREEN|AMBER|WHITE.",
    "WARNINGS AND ERRORS FROM THE LOG APPEAR IN THE EVENTS PANE.",
  };
  int n = (int)(sizeof(lines) / sizeof(lines[0]));
  for(int i = 0; i < n && i < h - 2; i++) tui_put(top + 1 + i, 2, i == 0 || i > 6 ? TC_VALUE : TC_LABEL, lines[i]);
}

/* ---- the frame ------------------------------------------------------------- */
static void render(void) {
  if(!g_term_raw || !tui_active()) return;
  tui_size(&W, &H);
  tui_begin();
  if(W < 60 || H < 16) {
    tui_put(0, 0, TC_WARN, "TERMINAL TOO SMALL: NEED 60X16");
    tui_flush();
    return;
  }
  char b1[64], b2[64];
  /* system line */
  tui_put(0, 1, TC_VALUE, "TCPSER " VERSION);
  {
    const char *name = EDITION;
    int len = (int)strlen(name);
    tui_put(0, (W - len) / 2, TC_VALUE, name);
    time_t now = time(NULL); struct tm tmb; localtime_r(&now, &tmb);
    strftime(b1, sizeof(b1), "%d/%m/%y %H:%M:%S", &tmb);
    tui_put(0, W - 1 - (int)strlen(b1), TC_LABEL, b1);
  }
  tui_hrule(1, 0, W);

  /* status strip: uptime, port, counters, blocked */
  snprintf(b2, sizeof(b2), "UP %s  PORT :%d  CALLS %lu  PASS %lu  FAIL %lu  DROP %lu  IN %s",
           fmt_uptime(g_dash.start_time, b1, sizeof(b1)), g_dash.port, g_dash.total_connections,
           g_dash.gate_pass, g_dash.gate_fail, g_dash.blocked, fmt_bytes(g_dash.bytes_in, (char[32]){0}, 32));
  tui_putn(2, 1, TC_LABEL, b2, W - 2);
  {
    char o[48]; snprintf(o, sizeof(o), "OUT %s  BLOCKED %d", fmt_bytes(g_dash.bytes_out, b1, sizeof(b1)), blocklist_count());
    if((int)strlen(b2) + (int)strlen(o) + 4 < W) tui_put(2, (int)strlen(b2) + 3, TC_LABEL, o);
  }

  int top = 3, next = 3;
  int msg_row = H - 2, keys_row = H - 1;
  if(g_page == PG_BLOCKS)      draw_blocklist_page(top, msg_row - top);
  else if(g_page == PG_HELP)   draw_help_page(top, msg_row - top);
  else {
    draw_lines_box(top, &next);
    /* rows for traffic + callers + events; on a short terminal the panes are
     * dropped from the bottom up: events first, then callers. Traffic keeps at
     * least two rows, callers a header plus one row, events one row. */
    int remaining = msg_row - next;
    int ev_h = remaining >= 14 ? 4 : 3;
    int cal_h = remaining >= 16 ? 6 : remaining >= 12 ? 5 : 4;
    int traf_h = remaining - ev_h - cal_h;
    if(traf_h < 4) { ev_h = 0; traf_h = remaining - cal_h; }
    if(traf_h < 4) { cal_h = 0; traf_h = remaining; }
    if(traf_h >= 3) { draw_traffic_box(next, traf_h); next += traf_h; }
    if(cal_h)       { draw_callers_box(next, cal_h);  next += cal_h; }
    if(ev_h)        draw_events_box(next, ev_h);
  }

  /* message line + hotkeys */
  if(g_msg[0] && time(NULL) - g_msg_at > 8 && !g_confirm) g_msg[0] = 0;
  if(g_msg[0]) tui_putn(msg_row, 1, g_confirm ? TC_WARN : TC_VALUE, g_msg, W - 2);
  {
    static const char *keys_long[][2]  = { {"H", "HANG UP"}, {"R", "RESET"}, {"B", "BLOCKLIST"}, {"U", "UNBLOCK"}, {"P", "PHOSPHOR"}, {"?", "HELP"}, {"Q", "QUIT"} };
    static const char *keys_short[][2] = { {"H", "HANG"}, {"R", "RESET"}, {"B", "BLOCK"}, {"U", "UNBLK"}, {"P", "PHOS"}, {"?", "HELP"}, {"Q", "QUIT"} };
    const char *(*keys)[2] = W >= 78 ? keys_long : keys_short;   /* every key stays visible down to 60 columns */
    const char *ph = tui_phosphor();
    int ph_col = W - 1 - (int)strlen(ph), c = 1;
    for(unsigned i = 0; i < sizeof(keys_long) / sizeof(keys_long[0]); i++) {
      int need = 2 + (int)strlen(keys[i][1]);
      if(c + need > ph_col - 2) { ph_col = -1; if(c + need > W - 1) break; }   /* keys win over the phosphor name */
      char k[8]; snprintf(k, sizeof(k), "%s=", keys[i][0]);
      tui_put(keys_row, c, TC_VALUE, k); c += 2;
      tui_put(keys_row, c, TC_LABEL, keys[i][1]); c += (int)strlen(keys[i][1]) + 2;
    }
    if(ph_col > 0) tui_put(keys_row, ph_col, TC_DIM, ph);
  }
  tui_flush();
  gettimeofday(&g_last_frame, NULL);
}

/* render at most ~10x/s from the traffic path; the periodic refresh in the
 * main loop fills in the rest */
static void render_throttled(void) {
  if(!g_term_raw) return;
  struct timeval now; gettimeofday(&now, NULL);
  long ms = (now.tv_sec - g_last_frame.tv_sec) * 1000 + (now.tv_usec - g_last_frame.tv_usec) / 1000;
  if(ms >= 100 || ms < 0) render();
}

/* Keyboard — send commands through pipe protocol (thread-safe) */
static void do_hangup(void) {
  modem_config *cfg = (modem_config *)g_dash.modem_cfg;
  if(!cfg) return;
  writePipe(cfg->mp[1][1], MSG_HANGUP);
}

static void do_reset(void) {
  modem_config *cfg = (modem_config *)g_dash.modem_cfg;
  if(!cfg) return;
  writePipe(cfg->mp[1][1], MSG_RESET);
}

/* Public API */

static void dashboard_init_state(const char *tty, int speed, int port, int modem_count) {
  memset(&g_dash, 0, sizeof(g_dash));
  snprintf(g_dash.tty, sizeof(g_dash.tty), "%s", tty);
  g_dash.speed = speed;
  g_dash.port = port;
  g_dash.modem_count = modem_count;
  g_dash.start_time = time(NULL);
  g_dash.dcd_on = 1;
  g_dash.dsr_on = 1;
  g_dash.serial_fd = -1;
  g_dash.enabled = 1;
}

/* Headless init for the web dashboard (-H) used WITHOUT the terminal dashboard
 * (-M): populate/collect stats + traffic in g_dash without taking over the
 * terminal (no raw mode, no rendering, no SIGWINCH). If -M already ran the full
 * init, this is a no-op. */
void dashboard_init_headless(const char *tty, int speed, int port, int modem_count) {
  if(g_dash.enabled) return;
  dashboard_init_state(tty, speed, port, modem_count);
}

/* True once either the terminal (-M) or web (-H) dashboard has initialised, so
 * the I/O trace path knows whether to capture traffic into g_dash. */
int dashboard_capture_active(void) { return g_dash.enabled; }

void dashboard_init(const char *tty, int speed, int port, int modem_count) {
  dashboard_init_state(tty, speed, port, modem_count);
  signal(SIGWINCH, handle_winch);
  term_raw_on();
  render();
}

/* Expose the dashboard mutex so other readers of g_dash (e.g. the web
 * dashboard threads) can take a consistent snapshot. */
void dashboard_lock(void) { pthread_mutex_lock(&dash_mutex); }
void dashboard_unlock(void) { pthread_mutex_unlock(&dash_mutex); }

void dashboard_set_serial_fd(int fd) {
  pthread_mutex_lock(&dash_mutex);
  g_dash.serial_fd = fd;
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_set_modem_cfg(void *cfg) {
  pthread_mutex_lock(&dash_mutex);
  g_dash.modem_cfg = cfg;
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_event(int type, unsigned char *data, int len) {
  if(!g_dash.enabled || len == 0) return;

  /* Only show TCP<->SER in traffic log. Update stats for all. */
  if(type != 16 && type != 32) {
    pthread_mutex_lock(&dash_mutex);
    if(type == 4) { g_dash.bytes_in += (unsigned long)len; g_dash.session_bytes_in += (unsigned long)len; }
    if(type == 8) { g_dash.bytes_out += (unsigned long)len; g_dash.session_bytes_out += (unsigned long)len; }
    /* Update LED timestamps for internal hops too */
    time_t now = time(NULL);
    if(type == 4) g_dash.last_bbs_rx = now;
    if(type == 8) g_dash.last_bbs_tx = now;
    pthread_mutex_unlock(&dash_mutex);
    return;
  }

  pthread_mutex_lock(&dash_mutex);

  time_t now = time(NULL);
  {
    /* attribute session bytes to the connected line (one line connected is the common case) */
    for(int li = 0; li < DASH_MAX_LINES; li++) {
      if(g_dash.lines[li].inited && g_dash.lines[li].connected) {
        if(type == 16) g_dash.lines[li].bytes_in += (unsigned long)len; else g_dash.lines[li].bytes_out += (unsigned long)len;
        break;
      }
    }
  }
  series_bytes(type == 16, len);
  if(type == 16) {
    g_dash.bytes_in += (unsigned long)len;
    g_dash.session_bytes_in += (unsigned long)len;
    g_dash.last_caller_rx = now;
    g_dash.last_bbs_tx = now;   /* caller data goes to BBS */
  }
  if(type == 32) {
    g_dash.bytes_out += (unsigned long)len;
    g_dash.session_bytes_out += (unsigned long)len;
    g_dash.last_bbs_rx = now;
    g_dash.last_caller_tx = now; /* BBS data goes to caller */
  }

  /* Try to append to previous line if same direction and room left */
  int prev_idx = (g_dash.traffic_head - 1 + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
  dash_traffic_line_t *prev = &g_dash.traffic[prev_idx];
  if(g_dash.traffic_count > 0 && prev->type == type &&
     prev->rawlen + len <= (int)sizeof(prev->raw)) {
    /* Append to existing line */
    int cplen = len;
    if(prev->rawlen + cplen > (int)sizeof(prev->raw))
      cplen = (int)sizeof(prev->raw) - prev->rawlen;
    memcpy(prev->raw + prev->rawlen, data, (size_t)cplen);
    prev->rawlen += cplen;
    prev->bytes += len;
  } else {
    /* New line */
    dash_traffic_line_t *t = &g_dash.traffic[g_dash.traffic_head];
    switch(type) {
      case 16: snprintf(t->dir, sizeof(t->dir), "CALLER-->BBS"); break;
      case 32: snprintf(t->dir, sizeof(t->dir), "BBS-->CALLER"); break;
      default: snprintf(t->dir, sizeof(t->dir), "???"); break;
    }
    int cplen = len > (int)sizeof(t->raw) ? (int)sizeof(t->raw) : len;
    memcpy(t->raw, data, (size_t)cplen);
    t->rawlen = cplen;
    t->bytes = len;
    t->type = type;
    g_dash.traffic_head = (g_dash.traffic_head + 1) % DASH_TRAFFIC_LINES;
    if(g_dash.traffic_count < DASH_TRAFFIC_LINES) g_dash.traffic_count++;
  }

  render_throttled();
  pthread_mutex_unlock(&dash_mutex);
}


/* ---- activity series (call with dash_mutex held) ---------------------- */
static void series_hour_touch(time_t now) {
  long period = (long)(now / 3600);
  int h = (int)(period % 24);
  if(g_dash.hour_stamp[h] != period) { g_dash.hour_stamp[h] = period; g_dash.hour_calls[h] = 0; g_dash.hour_fail[h] = 0; }
}
static void series_min_touch(time_t now) {
  long period = (long)(now / 60);
  int m = (int)(period % 60);
  if(g_dash.min_stamp[m] != period) { g_dash.min_stamp[m] = period; g_dash.min_in[m] = 0; g_dash.min_out[m] = 0; }
}
static void series_call(void)  { time_t now = time(NULL); series_hour_touch(now); g_dash.hour_calls[(now / 3600) % 24]++; }
static void series_fail(void)  { time_t now = time(NULL); series_hour_touch(now); g_dash.hour_fail[(now / 3600) % 24]++; }
static void series_bytes(int in, int len) {
  time_t now = time(NULL); series_min_touch(now);
  int m = (int)((now / 60) % 60);
  if(in) g_dash.min_in[m] += (unsigned long)len; else g_dash.min_out[m] += (unsigned long)len;
}

/* ---- per-line state ------------------------------------------------------
 * The TUI still renders the legacy single-line fields (connected/peer_ip/
 * session_*); they mirror "the most recently active line". The web dashboard
 * reads lines[] directly. */
static void sync_legacy_from_lines(void) {
  g_dash.connected = 0; g_dash.peer_ip[0] = 0;
  for(int i = 0; i < DASH_MAX_LINES; i++) {
    if(g_dash.lines[i].inited && g_dash.lines[i].connected) {
      g_dash.connected = 1;
      snprintf(g_dash.peer_ip, sizeof(g_dash.peer_ip), "%s", g_dash.lines[i].peer_ip);
      g_dash.session_start = g_dash.lines[i].session_start;
      g_dash.session_bytes_in = g_dash.lines[i].bytes_in;
      g_dash.session_bytes_out = g_dash.lines[i].bytes_out;
      break;
    }
  }
}

void dashboard_line_init(int idx, const char *device, int speed, void *cfg) {
  if(idx < 0 || idx >= DASH_MAX_LINES) return;
  pthread_mutex_lock(&dash_mutex);
  dash_line_t *l = &g_dash.lines[idx];
  memset(l, 0, sizeof(*l));
  l->inited = 1;
  snprintf(l->device, sizeof(l->device), "%s", device ? device : "?");
  l->speed = speed;
  l->dtr = 1;
  l->cfg = cfg;
  if(!g_dash.modem_cfg) g_dash.modem_cfg = cfg;
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_line_state(int idx, int connected, const char *peer_ip) {
  if(!g_dash.enabled || idx < 0 || idx >= DASH_MAX_LINES) return;
  pthread_mutex_lock(&dash_mutex);
  dash_line_t *l = &g_dash.lines[idx];
  if(!l->inited) l->inited = 1;
  l->connected = connected;
  if(connected) {
    snprintf(l->peer_ip, sizeof(l->peer_ip), "%s", peer_ip ? peer_ip : "?");
    l->session_start = time(NULL);
    l->bytes_in = l->bytes_out = 0;
  }
  sync_legacy_from_lines();
  render();
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_line_dtr(int idx, int dtr) {
  if(!g_dash.enabled || idx < 0 || idx >= DASH_MAX_LINES) return;
  pthread_mutex_lock(&dash_mutex);
  g_dash.lines[idx].dtr = dtr;
  g_dash.dsr_on = dtr;
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_line_closed(int idx) {
  if(!g_dash.enabled || idx < 0 || idx >= DASH_MAX_LINES) return;
  pthread_mutex_lock(&dash_mutex);
  dash_line_t *l = &g_dash.lines[idx];
  if(l->connected) {
    dash_history_entry_t *h = &g_dash.history[g_dash.history_head];
    snprintf(h->ip, sizeof(h->ip), "%s", l->peer_ip[0] ? l->peer_ip : "?");
    h->connect_time = l->session_start;
    h->disconnect_time = time(NULL);
    h->duration = (int)(h->disconnect_time - l->session_start);
    h->result = 3;
    h->bytes_in = (int)l->bytes_in;
    h->bytes_out = (int)l->bytes_out;
    g_dash.history_head = (g_dash.history_head + 1) % DASH_HISTORY_LINES;
    if(g_dash.history_count < DASH_HISTORY_LINES) g_dash.history_count++;
  }
  l->connected = 0; l->peer_ip[0] = 0;
  sync_legacy_from_lines();
  render();
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_note(const char *fmt, ...) {
  if(!g_dash.enabled) return;
  char text[80];
  va_list ap; va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&dash_mutex);
  dash_event_t *e = &g_dash.events[g_dash.events_head];
  e->when = time(NULL);
  snprintf(e->text, sizeof(e->text), "%s", text);
  g_dash.events_head = (g_dash.events_head + 1) % DASH_EVENTS;
  if(g_dash.events_count < DASH_EVENTS) g_dash.events_count++;
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_stat_connection(const char *ip) {
  if(!g_dash.enabled) return;
  pthread_mutex_lock(&dash_mutex);
  g_dash.total_connections++;
  series_call();
  render();
  pthread_mutex_unlock(&dash_mutex);
  (void)ip;
}

void dashboard_stat_blocked(const char *ip) {
  if(!g_dash.enabled) return;
  pthread_mutex_lock(&dash_mutex);
  g_dash.blocked++;
  series_fail();
  dash_history_entry_t *h = &g_dash.history[g_dash.history_head];
  snprintf(h->ip, sizeof(h->ip), "%s", ip ? ip : "?");
  h->connect_time = time(NULL); h->duration = 0; h->result = 0; h->bytes_in = 0; h->bytes_out = 0;
  g_dash.history_head = (g_dash.history_head + 1) % DASH_HISTORY_LINES;
  if(g_dash.history_count < DASH_HISTORY_LINES) g_dash.history_count++;
  render();
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_stat_gate(int passed, const char *ip) {
  if(!g_dash.enabled) return;
  pthread_mutex_lock(&dash_mutex);
  if(passed) g_dash.gate_pass++; else { g_dash.gate_fail++; series_fail(); }
  if(!passed) {
    dash_history_entry_t *h = &g_dash.history[g_dash.history_head];
    snprintf(h->ip, sizeof(h->ip), "%s", ip ? ip : "?");
    h->connect_time = time(NULL); h->duration = 0; h->result = 1; h->bytes_in = 0; h->bytes_out = 0;
    g_dash.history_head = (g_dash.history_head + 1) % DASH_HISTORY_LINES;
    if(g_dash.history_count < DASH_HISTORY_LINES) g_dash.history_count++;
  }
  render();
  pthread_mutex_unlock(&dash_mutex);
}

void dashboard_render(void) {
  if(!g_dash.enabled || !g_term_raw) return;
  pthread_mutex_lock(&dash_mutex); render(); pthread_mutex_unlock(&dash_mutex);
}

int dashboard_check_winch(void) {
  if(g_winch) { g_winch = 0; tui_invalidate(); dashboard_render(); return 1; }
  return 0;
}

int dashboard_quit_requested(void) { return g_quit_requested; }

static void ask_confirm(char key, const char *what) {
  g_confirm = key;
  g_confirm_at = time(NULL);
  snprintf(g_msg, sizeof(g_msg), "%s? (Y/N)", what);
  g_msg_at = g_confirm_at;
}

void dashboard_check_keyboard(void) {
  /* Interactive keys only make sense when the terminal dashboard owns the tty
   * in raw mode. In headless (web-only) mode, never read stdin. */
  if(!g_dash.enabled || !g_term_raw) return;
  char ch;
  int changed = 0;
  while(read(STDIN_FILENO, &ch, 1) == 1) {
    changed = 1;
    if(g_confirm) {
      if(time(NULL) - g_confirm_at > CONFIRM_TIMEOUT) g_confirm = 0;   /* stale prompt: fall through */
    }
    if(g_confirm) {
      char pending = g_confirm;
      g_confirm = 0;
      if(ch == 'y' || ch == 'Y') {
        switch(pending) {
          case 'h': do_hangup(); set_msg("HANG UP REQUESTED"); break;
          case 'u': { int n = blocklist_unblock_all_timed(); set_msg("CLEARED %d TIMED BLOCK(S)", n); break; }
          case 'q': g_quit_requested = 1; return;
        }
      } else set_msg("CANCELLED");
      continue;
    }
    switch(ch) {
      case 'h': case 'H': {
        int online = 0; for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited && g_dash.lines[i].connected) online = 1;
        if(online) ask_confirm('h', "HANG UP THE CURRENT CALLER"); else set_msg("NO CALLER ONLINE");
        break; }
      case 'r': case 'R': do_reset(); set_msg("MODEM RESET SENT (ATZ)"); break;
      case 'b': case 'B': g_page = (g_page == PG_BLOCKS) ? PG_MAIN : PG_BLOCKS; tui_invalidate(); break;
      case '?': case '/': g_page = (g_page == PG_HELP) ? PG_MAIN : PG_HELP; tui_invalidate(); break;
      case 's': case 'S': case 27: g_page = PG_MAIN; g_msg[0] = 0; tui_invalidate(); break;
      case 'u': case 'U': ask_confirm('u', "CLEAR ALL TIMED BLOCKS"); break;
      case 'p': case 'P': set_msg("PHOSPHOR: %s", tui_next_phosphor()); break;
      case 'q': case 'Q': ask_confirm('q', "QUIT TCPSER (HANGS UP CALLERS, SAVES STATS)"); break;
      default: break;
    }
  }
  if(changed) dashboard_render();
}

void dashboard_set_stats_path(const char *path) {
  if(!path || !path[0]) return;
  snprintf(g_stats_path, sizeof(g_stats_path), "%s", path);
}

const char *dashboard_get_stats_path(void) {
  return g_stats_path[0] ? g_stats_path : NULL;
}

void dashboard_save_stats(const char *path) {
  int i, idx;
  FILE *f;
  if(!path || !path[0]) return;
  f = fopen(path, "w");
  if(!f) return;
  pthread_mutex_lock(&dash_mutex);
  /* The persisted history includes caller IP addresses (PII), so keep the
   * stats file owner-only regardless of umask. */
  (void)chmod(path, S_IRUSR | S_IWUSR);
  fprintf(f, "# TCPSER statistics (auto-saved)\n");
  fprintf(f, "connections=%lu\n", g_dash.total_connections);
  fprintf(f, "blocked=%lu\n", g_dash.blocked);
  fprintf(f, "gate_pass=%lu\n", g_dash.gate_pass);
  fprintf(f, "gate_fail=%lu\n", g_dash.gate_fail);
  fprintf(f, "bytes_in=%lu\n", g_dash.bytes_in);
  fprintf(f, "bytes_out=%lu\n", g_dash.bytes_out);
  /* Persist connection history (newest first) */
  if(g_dash.history_count > 0) {
    fprintf(f, "# Connection history\n");
    for(i = 0; i < g_dash.history_count && i < DASH_HISTORY_LINES; i++) {
      idx = (g_dash.history_head - 1 - i + DASH_HISTORY_LINES) % DASH_HISTORY_LINES;
      fprintf(f, "history=%s,%ld,%ld,%d,%d,%d,%d\n",
              g_dash.history[idx].ip,
              (long)g_dash.history[idx].connect_time,
              (long)g_dash.history[idx].disconnect_time,
              g_dash.history[idx].duration,
              g_dash.history[idx].result,
              g_dash.history[idx].bytes_in,
              g_dash.history[idx].bytes_out);
    }
  }
  pthread_mutex_unlock(&dash_mutex);
  fclose(f);
}

void dashboard_load_stats(const char *path) {
  FILE *f;
  char line[512];
  if(!path || !path[0]) return;
  f = fopen(path, "r");
  if(!f) return;
  pthread_mutex_lock(&dash_mutex);
  g_dash.history_head = 0;
  g_dash.history_count = 0;
  while(fgets(line, sizeof(line), f)) {
    unsigned long val;
    char ip[64];
    long ct, dt;
    int dur, res, bi, bo;
    if(line[0] == '#') continue;
    if(sscanf(line, "connections=%lu", &val) == 1) g_dash.total_connections = val;
    else if(sscanf(line, "blocked=%lu", &val) == 1) g_dash.blocked = val;
    else if(sscanf(line, "gate_pass=%lu", &val) == 1) g_dash.gate_pass = val;
    else if(sscanf(line, "gate_fail=%lu", &val) == 1) g_dash.gate_fail = val;
    else if(sscanf(line, "bytes_in=%lu", &val) == 1) g_dash.bytes_in = val;
    else if(sscanf(line, "bytes_out=%lu", &val) == 1) g_dash.bytes_out = val;
    else if(sscanf(line, "history=%63[^,],%ld,%ld,%d,%d,%d,%d", ip, &ct, &dt, &dur, &res, &bi, &bo) == 7) {
      if(g_dash.history_count < DASH_HISTORY_LINES) {
        int idx = g_dash.history_head;
        snprintf(g_dash.history[idx].ip, sizeof(g_dash.history[idx].ip), "%s", ip);
        g_dash.history[idx].connect_time = (time_t)ct;
        g_dash.history[idx].disconnect_time = (time_t)dt;
        g_dash.history[idx].duration = dur;
        g_dash.history[idx].result = res;
        g_dash.history[idx].bytes_in = bi;
        g_dash.history[idx].bytes_out = bo;
        g_dash.history_head = (g_dash.history_head + 1) % DASH_HISTORY_LINES;
        g_dash.history_count++;
      }
    }
  }
  pthread_mutex_unlock(&dash_mutex);
  fclose(f);
}

void dashboard_autosave(void) {
  time_t now;
  if(!g_stats_path[0]) return;
  now = time(NULL);
  if(g_last_autosave == 0) g_last_autosave = now;
  if((now - g_last_autosave) >= AUTOSAVE_INTERVAL) {
    g_last_autosave = now;
    dashboard_save_stats(g_stats_path);
  }
}
