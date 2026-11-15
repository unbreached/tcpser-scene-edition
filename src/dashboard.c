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
#include <fcntl.h>

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
enum { PG_MAIN = 0, PG_BLOCKS, PG_HELP, PG_GRAPHS };
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

static int g_sel_line = -1;      /* line picked with a digit key; -1 = follow the caller */
static int g_confirm_line = -1;  /* line a pending Y/N question is about */
static char g_confirm_ip[20];    /* address a pending allow / disallow question is about */
static int target_line(void);

/* ---- pages ----------------------------------------------------------------
 * The main screen is a small network operations wall for one BBS gateway:
 *
 *   system line     name, ON AIR lamp, clock
 *   counters        tiles (tall screens) or one strip
 *   DATA FLOW       callers > shield > gate > modem > C64, arrows move while
 *                   bytes do; a drop or a failed gate flashes its stage
 *   graphs          bytes per second (60 s) and calls per hour (24 h)
 *   LINES           one row per modem line
 *   TRAFFIC / RECENT CALLERS / EVENTS   side by side on a wide screen
 *
 * Every block is sized from the terminal, from 60x16 up; what does not fit is
 * dropped from the least important end and stays reachable on its own page
 * (G graphs, B lists). */
static const char *U(const char *utf8, const char *ascii) { return tui_is_utf8() ? utf8 : ascii; }
static int anim_phase(void) { struct timeval t; gettimeofday(&t, NULL); return (int)(((long)t.tv_sec * 4 + t.tv_usec / 250000) & 0x3fffffff); }
/* text clipped to a box: never past column `right` (exclusive) */
static void put_clip(int row, int col, int color, const char *s, int right) { if(right > col) tui_putn(row, col, color, s, right - col); }
static void put_center(int row, int left, int width, int color, const char *s) {
  int n = cells_of(s); if(n > width) n = width;
  tui_putn(row, left + (width - n) / 2, color, s, width);
}
/* centred on column `mid`, kept inside the frame */
static void put_mid(int row, int mid, int color, const char *s) {
  int n = cells_of(s), c = mid - n / 2;
  if(c + n > W - 2) c = W - 2 - n;
  if(c < 2) c = 2;
  tui_putn(row, c, color, s, W - 2 - c);
}
static int lines_online(int *total) {
  int on = 0, n = 0;
  for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited) { n++; if(g_dash.lines[i].connected) on++; }
  if(total) *total = n;
  return on;
}

/* ---- counters -------------------------------------------------------------- */
typedef struct { const char *label; char val[48]; int color; } kpi_t;
#define NKPI 8
static void build_kpis(kpi_t *k) {
  int nblk = blocklist_count();
  k[0] = (kpi_t){ "UPTIME", "", TC_VALUE };   fmt_uptime(g_dash.start_time, k[0].val, sizeof(k[0].val));
  k[1] = (kpi_t){ "CALLS", "", TC_VALUE };    snprintf(k[1].val, sizeof(k[1].val), "%lu", g_dash.total_connections);
  k[2] = (kpi_t){ "PASS", "", g_dash.gate_pass ? TC_OK : TC_VALUE };   snprintf(k[2].val, sizeof(k[2].val), "%lu", g_dash.gate_pass);
  k[3] = (kpi_t){ "FAIL", "", g_dash.gate_fail ? TC_WARN : TC_VALUE }; snprintf(k[3].val, sizeof(k[3].val), "%lu", g_dash.gate_fail);
  k[4] = (kpi_t){ "DROP", "", g_dash.blocked ? TC_BAD : TC_VALUE };    snprintf(k[4].val, sizeof(k[4].val), "%lu", g_dash.blocked);
  k[5] = (kpi_t){ "BLOCKED", "", nblk ? TC_BAD : TC_VALUE };           snprintf(k[5].val, sizeof(k[5].val), "%d", nblk);
  k[6] = (kpi_t){ "RX", "", TC_ACCENT };      fmt_bytes(g_dash.bytes_in, k[6].val, sizeof(k[6].val));
  k[7] = (kpi_t){ "TX", "", TC_ACCENT2 };     fmt_bytes(g_dash.bytes_out, k[7].val, sizeof(k[7].val));
}
static void draw_kpi_strip(int row) {
  kpi_t k[NKPI]; build_kpis(k);
  int c = 1;
  for(int i = 0; i < NKPI; i++) {
    int need = (int)strlen(k[i].label) + 1 + (int)strlen(k[i].val);
    if(c + need > W - 1) break;
    tui_put(row, c, TC_LABEL, k[i].label); c += (int)strlen(k[i].label) + 1;
    tui_put(row, c, k[i].color, k[i].val); c += (int)strlen(k[i].val) + 2;
  }
}
/* tall screens: label over value in even columns, no frames */
static void draw_kpi_tiles(int top) {
  kpi_t k[NKPI]; build_kpis(k);
  int tw = (W - 2) / NKPI;
  for(int i = 0; i < NKPI; i++) {
    int left = 1 + i * tw;
    put_clip(top, left, TC_HEAD, k[i].label, left + tw - 1);
    put_clip(top + 1, left, k[i].color, k[i].val, left + tw - 1);
  }
}

/* ---- data flow ------------------------------------------------------------- */
typedef struct { const char *title; char detail[48]; char status[64]; int frame; int scolor; } flow_node_t;
#define NFLOW 5
static void build_flow(flow_node_t *n, time_t now) {
  int total = 0, on = lines_online(&total);
  int nblk = blocklist_count();
  const char *gate = "OPEN";
  const char *dev = "-"; const char *peer = ""; int speed = 0, dtr = 0;
  for(int i = 0; i < DASH_MAX_LINES; i++) {
    dash_line_t *l = &g_dash.lines[i];
    if(!l->inited) continue;
    if(dev[0] == '-') {
      dev = l->device; speed = l->speed;
      modem_config *c = (modem_config *)l->cfg;
      if(c) gate = c->gate_mode == 1 ? "BACKSPACE" : c->gate_mode == 2 ? "PASSWORD" : "OPEN";
    }
    if(l->dtr) dtr = 1;
    if(l->connected && !peer[0]) { peer = l->peer_ip; dev = l->device; speed = l->speed; }
  }
  int attempt = now - g_dash.last_attempt <= 2, drop = now - g_dash.last_drop <= 3;
  int gfail = now - g_dash.last_gate_fail <= 3, gpass = now - g_dash.last_gate_pass <= 3;

  n[0] = (flow_node_t){ "CALLERS", "", "", on || attempt ? TC_ACCENT : TC_FRAME, TC_VALUE };
  snprintf(n[0].detail, sizeof(n[0].detail), "TCP :%d", g_dash.port);
  snprintf(n[0].status, sizeof(n[0].status), "%lu CALLS", g_dash.total_connections);

  n[1] = (flow_node_t){ "SHIELD", "", "", drop ? TC_BAD : TC_FRAME, g_dash.blocked ? TC_BAD : TC_DIM };
  snprintf(n[1].detail, sizeof(n[1].detail), "%d BLOCKED", nblk);
  snprintf(n[1].status, sizeof(n[1].status), "%lu DROPPED", g_dash.blocked);

  n[2] = (flow_node_t){ "GATE", "", "", gfail ? TC_WARN : gpass ? TC_OK : TC_FRAME, g_dash.gate_fail ? TC_WARN : TC_OK };
  snprintf(n[2].detail, sizeof(n[2].detail), "%s", gate);
  snprintf(n[2].status, sizeof(n[2].status), "%lu PASS %lu FAIL", g_dash.gate_pass, g_dash.gate_fail);

  n[3] = (flow_node_t){ "MODEM", "", "", on ? TC_ACCENT : TC_FRAME, on ? TC_ACCENT : TC_DIM };
  snprintf(n[3].detail, sizeof(n[3].detail), "%d/%d ONLINE", on, total);
  snprintf(n[3].status, sizeof(n[3].status), "%s", on ? (peer[0] ? peer : "CARRIER") : "IDLE");

  n[4] = (flow_node_t){ "C64 BBS", "", "", on ? TC_ACCENT : dtr ? TC_OK : TC_FRAME, dtr ? TC_OK : TC_DIM };
  snprintf(n[4].detail, sizeof(n[4].detail), "%.14s", dev);
  snprintf(n[4].status, sizeof(n[4].status), "%d BAUD DTR %s", speed, tui_glyph(dtr ? G_DOT_ON : G_DOT_OFF));
}
/* one run of arrows between two stages. dir > 0: towards the BBS */
static void flow_arrows(int row, int col, int n, int dir, int active, int carrier, int phase) {
  for(int x = 0; x < n; x++) {
    if(active) {
      int lit = dir > 0 ? ((x - phase) % 4 + 4) % 4 == 0 : (x + phase) % 4 == 0;
      tui_putn(row, col + x, lit ? TC_VALUE : (dir > 0 ? TC_ACCENT : TC_ACCENT2), dir > 0 ? U("\xc2\xbb", ">") : U("\xc2\xab", "<"), 1);
    } else tui_putn(row, col + x, carrier ? TC_DIM : TC_FRAME, carrier ? U("\xe2\x95\x90", "=") : U("\xc2\xb7", "."), 1);
  }
}
static void draw_flow(int top, int h) {
  time_t now = time(NULL);
  flow_node_t n[NFLOW]; build_flow(n, now);
  int phase = anim_phase(), carrier = lines_online(NULL) > 0;
  int rx = carrier && now - g_dash.last_caller_rx <= 1, tx = carrier && now - g_dash.last_caller_tx <= 1;
  int knock = now - g_dash.last_attempt <= 2;      /* someone is at the door */
  tui_box_c(top, 0, h, W, "DATA FLOW", TC_REV_INFO);
  {
    const char *lamp = rx && tx ? "RX+TX" : rx ? "RX" : tx ? "TX" : carrier ? "CARRIER" : knock ? "INCOMING" : "QUIET";
    char t[24]; snprintf(t, sizeof(t), " %s ", lamp);
    tui_put(top, W - 3 - (int)strlen(t), rx || tx ? TC_VALUE : carrier ? TC_ACCENT : knock ? TC_WARN : TC_DIM, t);
  }
  int inner = W - 4;
  int nw = W >= 118 ? 16 : W >= 96 ? 14 : W >= 78 ? 12 : 10;
  int gw = (inner - NFLOW * nw) / (NFLOW - 1); if(gw > 18) gw = 18; if(gw < 1) gw = 1;
  int x0 = 2 + (inner - (NFLOW * nw + (NFLOW - 1) * gw)) / 2;
  if(h >= 5) {
    /* three plain rows per stage: name (lit while it is doing something),
     * what it is, and its running count. Arrows to the BBS run beside the
     * names, arrows back to the caller beside the second row. */
    for(int i = 0; i < NFLOW; i++) {
      int x = x0 + i * (nw + gw), f = n[i].frame;
      { char nm[24]; snprintf(nm, sizeof(nm), "[%s]", n[i].title); put_center(top + 1, x, nw, f == TC_FRAME ? TC_HEAD : f, nm); }
      put_center(top + 2, x, nw, TC_VALUE, n[i].detail);
      put_mid(top + 3, x + nw / 2, n[i].scolor, n[i].status);
      if(i < NFLOW - 1 && gw >= 3) {
        int before_gate = i < 2;
        flow_arrows(top + 1, x + nw + 1, gw - 2, +1, rx || (before_gate && knock), carrier, phase);
        flow_arrows(top + 2, x + nw + 1, gw - 2, -1, tx, carrier, phase);
      }
    }
  } else {
    for(int i = 0; i < NFLOW; i++) {
      int x = x0 + i * (nw + gw), f = n[i].frame;
      { char nm[24]; snprintf(nm, sizeof(nm), "[%s]", n[i].title); put_center(top + 1, x, nw, f == TC_FRAME ? TC_HEAD : f, nm); }
      if(h >= 4) put_mid(top + 2, x + nw / 2, n[i].scolor, i == 4 ? n[i].detail : n[i].status);
      if(i < NFLOW - 1) {
        int before_gate = i < 2;
        if(tx && !(rx && (phase & 4))) flow_arrows(top + 1, x + nw, gw, -1, 1, carrier, phase);
        else flow_arrows(top + 1, x + nw, gw, +1, rx || (before_gate && knock), carrier, phase);
      }
    }
  }
}

/* ---- graphs ---------------------------------------------------------------- */
static const char *bar_up(int eighths) {
  static const char *g[9] = { " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };
  if(eighths < 0) eighths = 0;
  if(eighths > 8) eighths = 8;
  if(!tui_is_utf8()) return eighths >= 5 ? "#" : eighths >= 1 ? "." : " ";
  return g[eighths];
}
static const char *bar_down(int eighths) {
  if(eighths <= 0) return " ";
  if(!tui_is_utf8()) return eighths >= 5 ? "#" : "'";
  return eighths >= 5 ? "\xe2\x96\x88" : "\xe2\x96\x80";
}
/* one bar, `colw` cells wide, growing up from `base` (rows above it) or down */
static void vbar(int base, int col, int colw, int rows, unsigned long v, unsigned long max, int color, int down) {
  if(rows < 1 || max == 0) return;
  long level = (long)((double)v * rows * 8 / (double)max + 0.5);
  if(v > 0 && level < 1) level = 1;
  for(int r = 0; r < rows; r++) {
    int e = (int)(level - (long)r * 8);
    if(e <= 0) break;
    for(int x = 0; x < colw; x++) tui_putn(down ? base + r : base - r, col + x, color, down ? bar_down(e) : bar_up(e), 1);
  }
}
/* mirrored chart: `in` grows up from the middle, `out` grows down. n values, oldest first. */
static void draw_mirror(int top, int left, int h, int w, const char *title, const unsigned long *in, const unsigned long *out, int n, const char *unit, const char *span) {
  if(h < 4 || w < 16) return;
  tui_box_c(top, left, h, w, title, TC_REV_ACCENT);
  int rows = h - 2, up = (rows + 1) / 2, dn = rows - up;
  /* a wide box stretches the series over its whole width; a narrow one shows the newest part */
  int inner = w - 4, cols = inner, first = inner >= n ? 0 : n - inner;
  /* each half has its own scale, so a quiet direction is still readable next to a busy one */
  unsigned long max_in = 1, max_out = 1, cur_in = in[n - 1], cur_out = out[n - 1], peak = 0;
  for(int i = first; i < n; i++) { if(in[i] > max_in) max_in = in[i]; if(out[i] > max_out) max_out = out[i]; if(in[i] + out[i] > peak) peak = in[i] + out[i]; }
  int mid = top + up;                       /* last row of the upper half */
  for(int c = 0; c < cols; c++) {
    int v = inner >= n ? c * n / inner : first + c, col = left + 2 + c;
    vbar(mid, col, 1, up, in[v], max_in, TC_ACCENT, 0);
    if(dn > 0) vbar(mid + 1, col, 1, dn, out[v], max_out, TC_ACCENT2, 1);
    if(in[v] == 0) tui_putn(mid, col, TC_FRAME, U("\xc2\xb7", "."), 1);   /* baseline */
  }
  /* figures set into the frame: current values on top, scale and span below */
  char b1[32], b2[32], t[96];
  snprintf(t, sizeof(t), " RX %s%s ", fmt_bytes(cur_in, b1, sizeof(b1)), unit);
  int c = left + w - 2 - (int)strlen(t);
  snprintf(b2, sizeof(b2), " TX %s%s ", fmt_bytes(cur_out, b1, sizeof(b1)), unit);
  c -= (int)strlen(b2);
  if(c > left + 4 + (int)strlen(title)) { tui_put(top, c, TC_ACCENT, t); tui_put(top, c + (int)strlen(t), TC_ACCENT2, b2); }
  snprintf(t, sizeof(t), " %s  PEAK %s%s ", span, fmt_bytes(peak, b1, sizeof(b1)), unit);
  if((int)strlen(t) < w - 4) tui_put(top + h - 1, left + w - 2 - (int)strlen(t), TC_DIM, t);
}
static void series_seconds(unsigned long *in, unsigned long *out, int n) {
  time_t now = time(NULL);
  for(int i = 0; i < n; i++) {
    time_t t = now - (n - 1 - i); int s = (int)(t % 60);
    int ok = g_dash.sec_stamp[s] == t;
    in[i] = ok ? g_dash.sec_in[s] : 0; out[i] = ok ? g_dash.sec_out[s] : 0;
  }
}
static void series_minutes(unsigned long *in, unsigned long *out, int n) {
  long now = (long)(time(NULL) / 60);
  for(int i = 0; i < n; i++) {
    long p = now - (n - 1 - i); int m = (int)(p % 60);
    int ok = g_dash.min_stamp[m] == p;
    in[i] = ok ? g_dash.min_in[m] : 0; out[i] = ok ? g_dash.min_out[m] : 0;
  }
}
static void draw_throughput(int top, int left, int h, int w) {
  unsigned long in[60], out[60]; series_seconds(in, out, 60);
  draw_mirror(top, left, h, w, "THROUGHPUT", in, out, 60, "/S", "LAST 60 S");
}
static void draw_minutes(int top, int left, int h, int w) {
  unsigned long in[60], out[60]; series_minutes(in, out, 60);
  draw_mirror(top, left, h, w, "TRAFFIC", in, out, 60, "/MIN", "LAST 60 MIN");
}
/* calls per hour, the failures and drops of the same hour beside them */
static void draw_calls24(int top, int left, int h, int w) {
  if(h < 4 || w < 30) return;
  tui_box_c(top, left, h, w, "CALLS 24 H", TC_REV_WARN);
  long hp = (long)(time(NULL) / 3600);
  unsigned calls[24], fails[24], max = 1, sc = 0, sf = 0;
  for(int i = 0; i < 24; i++) {
    long p = hp - (23 - i); int hh = (int)(p % 24); int ok = g_dash.hour_stamp[hh] == p;
    calls[i] = ok ? g_dash.hour_calls[hh] : 0; fails[i] = ok ? g_dash.hour_fail[hh] : 0;
    if(calls[i] > max) max = calls[i];
    if(fails[i] > max) max = fails[i];
    sc += calls[i]; sf += fails[i];
  }
  int inner = w - 4, bw = inner / 24; if(bw > 5) bw = 5; if(bw < 1) bw = 1;
  int rows = h - 3, x0 = left + 2 + (inner - 24 * bw) / 2, base = top + rows;
  struct tm tmb; time_t now = time(NULL); localtime_r(&now, &tmb);
  for(int i = 0; i < 24; i++) {
    int col = x0 + i * bw, cw = bw >= 2 ? bw - 1 : 1;
    int half = bw >= 3 ? (cw + 1) / 2 : cw;
    vbar(base, col, half, rows, calls[i], max, TC_ACCENT, 0);
    if(bw >= 3) vbar(base, col + half, cw - half, rows, fails[i], max, TC_BAD, 0);
    else if(fails[i] > calls[i]) vbar(base, col, half, rows, fails[i], max, TC_BAD, 0);
    if(calls[i] == 0 && fails[i] == 0) tui_putn(base, col, TC_FRAME, U("\xc2\xb7", "."), 1);
    if(i % 6 == 5 || (i == 0 && bw >= 2)) {
      char lb[8]; snprintf(lb, sizeof(lb), "%02d", ((tmb.tm_hour - (23 - i)) % 24 + 24) % 24);
      if(bw >= 2 || i % 6 == 5) put_clip(base + 1, col, TC_DIM, lb, left + w - 1);
    }
  }
  char t[64]; snprintf(t, sizeof(t), " %u CALLS ", sc);
  char f[32]; snprintf(f, sizeof(f), " %u FAIL/DROP ", sf);
  int c = left + w - 2 - (int)strlen(t) - (int)strlen(f);
  if(c > left + 16) { tui_put(top, c, TC_ACCENT, t); tui_put(top, c + (int)strlen(t), sf ? TC_BAD : TC_DIM, f); }
}
/* who calls most: the recent history folded by address */
static void draw_sources(int top, int left, int h, int w) {
  if(h < 4 || w < 30) return;
  tui_box_c(top, left, h, w, "TOP SOURCES", TC_REV_BAD);
  struct { char ip[20]; int n, bad, ok; } s[DASH_HISTORY_LINES]; int ns = 0;
  int cnt = g_dash.history_count < DASH_HISTORY_LINES ? g_dash.history_count : DASH_HISTORY_LINES;
  for(int r = 0; r < cnt; r++) {
    dash_history_entry_t *e = &g_dash.history[(g_dash.history_head - 1 - r + DASH_HISTORY_LINES) % DASH_HISTORY_LINES];
    int k = 0; for(; k < ns; k++) if(strncmp(s[k].ip, e->ip, sizeof(s[k].ip) - 1) == 0) break;
    if(k == ns) { snprintf(s[ns].ip, sizeof(s[ns].ip), "%.19s", e->ip); s[ns].n = s[ns].bad = s[ns].ok = 0; ns++; }
    s[k].n++; if(e->result >= 2) s[k].ok++; else s[k].bad++;
  }
  for(int a = 0; a < ns; a++) for(int b = a + 1; b < ns; b++) if(s[b].n > s[a].n) { __typeof__(s[0]) t = s[a]; s[a] = s[b]; s[b] = t; }
  tui_put(top + 1, left + 2, TC_HEAD, "ADDRESS          CALLS");
  int rows = h - 3, max = ns ? s[0].n : 1, barw = w - 4 - 27; if(barw < 0) barw = 0;
  for(int i = 0; i < ns && i < rows; i++) {
    char line[48]; int row = top + 2 + i, col = s[i].ok == 0 ? TC_BAD : s[i].bad ? TC_WARN : TC_OK;
    snprintf(line, sizeof(line), "%-16.16s", s[i].ip); put_clip(row, left + 2, allowlist_has(s[i].ip) ? TC_OK : TC_VALUE, line, left + w - 1);
    snprintf(line, sizeof(line), "%4d", s[i].n); put_clip(row, left + 19, col, line, left + w - 1);
    int len = barw * s[i].n / max; if(len < 1 && barw > 0) len = 1;
    tui_fill(row, left + 25, len, col, U("\xe2\x96\x88", "#"));
  }
  if(ns == 0) tui_put(top + 2, left + 2, TC_DIM, "NO CALLS YET");
}
static void draw_graph_row(int top, int h) {
  int lw = W / 2;
  draw_throughput(top, 0, h, lw);
  draw_calls24(top, lw, h, W - lw);
}
static void draw_graphs_page(int top, int h) {
  int a = h / 3, c = h - 2 * a, lw = W * 58 / 100;
  draw_throughput(top, 0, a, W);
  draw_minutes(top + a, 0, a, W);
  if(W >= 100) { draw_calls24(top + 2 * a, 0, c, lw); draw_sources(top + 2 * a, lw, c, W - lw); }
  else draw_calls24(top + 2 * a, 0, c, W);
}

/* ---- session banner ---------------------------------------------------------
 * The one thing the operator must see from across the room: is somebody on
 * the BBS right now. Big block letters (a 5x5 font drawn with half blocks,
 * three text rows), the caller underneath, a scanner while the line waits. */
typedef struct { char ch; const char *rows[5]; } big_glyph_t;
static const big_glyph_t BIG[] = {
  { 'A', { " ### ", "#   #", "#####", "#   #", "#   #" } },
  { 'B', { "#### ", "#   #", "#### ", "#   #", "#### " } },
  { 'D', { "#### ", "#   #", "#   #", "#   #", "#### " } },
  { 'E', { "#####", "#    ", "#### ", "#    ", "#####" } },
  { 'I', { "###", " # ", " # ", " # ", "###" } },
  { 'L', { "#    ", "#    ", "#    ", "#    ", "#####" } },
  { 'N', { "#   #", "##  #", "# # #", "#  ##", "#   #" } },
  { 'O', { " ### ", "#   #", "#   #", "#   #", " ### " } },
  { 'S', { " ####", "#    ", " ### ", "    #", "#### " } },
  { 'T', { "#####", "  #  ", "  #  ", "  #  ", "  #  " } },
  { 'Y', { "#   #", " # # ", "  #  ", "  #  ", "  #  " } },
};
static const big_glyph_t *big_find(char c) {
  for(unsigned i = 0; i < sizeof(BIG) / sizeof(BIG[0]); i++) if(BIG[i].ch == c) return &BIG[i];
  return NULL;
}
static int big_width(const char *s) {
  int w = 0;
  for(; *s; s++) { const big_glyph_t *g = big_find(*s); w += (g ? (int)strlen(g->rows[0]) : 3) + 1; }
  return w > 0 ? w - 1 : 0;
}
static void big_text(int top, int left, int color, const char *s) {
  int x = left;
  for(; *s; s++) {
    const big_glyph_t *g = big_find(*s);
    if(!g) { x += 4; continue; }
    int gw = (int)strlen(g->rows[0]);
    for(int r = 0; r < 3; r++) for(int c = 0; c < gw; c++) {
      int up = g->rows[r * 2][c] == '#', dn = r * 2 + 1 < 5 && g->rows[r * 2 + 1][c] == '#';
      const char *cell = up && dn ? U("\xe2\x96\x88", "#") : up ? U("\xe2\x96\x80", "\"") : dn ? U("\xe2\x96\x84", "_") : NULL;
      if(cell) tui_putn(top + r, x + c, color, cell, 1);
    }
    x += gw + 1;
  }
}
/* a light sweeping back and forth: the line is listening */
static void scanner(int row, int col, int n, int phase, int mirror) {
  static const char *shade[4] = { "\xe2\x96\x88", "\xe2\x96\x93", "\xe2\x96\x92", "\xe2\x96\x91" };
  static const char *ashade[4] = { "#", "=", "-", "." };
  int span = 2 * (n - 1), p = span > 0 ? phase % span : 0;
  if(p >= n) p = span - p;
  if(mirror) p = n - 1 - p;
  for(int i = 0; i < n; i++) {
    int d = i > p ? i - p : p - i;
    if(d < 4) tui_putn(row, col + i, d == 0 ? TC_VALUE : TC_INFO, tui_is_utf8() ? shade[d] : ashade[d], 1);
    else tui_putn(row, col + i, TC_FRAME, U("\xc2\xb7", "."), 1);
  }
}
static void draw_session(int top, int left, int h, int w) {
  int total = 0, on = lines_online(&total);
  int phase = anim_phase();
  tui_box_c(top, left, h, w, on ? "SESSION" : "SESSION", on ? TC_REV_ACCENT : TC_REV_INFO);
  const char *word = on ? "ONLINE" : "STANDBY";
  int bw = big_width(word), inner = w - 4;
  int brow = top + 1 + (h - 2 - 4) / 2;                 /* banner (3 rows) + info row, centred */
  if(bw <= inner) big_text(brow, left + 2 + (inner - bw) / 2, on ? TC_ACCENT : TC_INFO, word);
  else put_center(brow + 1, left + 2, inner, on ? TC_REV_ACCENT : TC_INFO, word);
  char info[160], b1[32], b2[32], b3[32];
  int irow = brow + 3;
  if(on) {
    dash_line_t *l = NULL; int ln = -1;
    int t = target_line();
    if(t >= 0 && g_dash.lines[t].connected) { l = &g_dash.lines[t]; ln = t; }
    for(int i = 0; i < DASH_MAX_LINES && !l; i++) if(g_dash.lines[i].inited && g_dash.lines[i].connected) { l = &g_dash.lines[i]; ln = i; }
    if(l) {
      snprintf(info, sizeof(info), "%s   %s   RX %s  TX %s", l->peer_ip, fmt_dur((int)(time(NULL) - l->session_start), b1, sizeof(b1)),
               fmt_bytes(l->bytes_in, b2, sizeof(b2)), fmt_bytes(l->bytes_out, b3, sizeof(b3)));
      put_center(irow, left + 2, inner, allowlist_has(l->peer_ip) ? TC_OK : TC_VALUE, info);
      snprintf(info, sizeof(info), on > 1 ? " LINE %d  +%d MORE " : " LINE %d ", ln, on - 1);
      tui_put(top, left + w - 3 - (int)strlen(info), TC_ACCENT, info);
    }
    /* the carrier lamp: blinks, like the CD light it stands for */
    int lc = left + (w - 18) / 2;
    tui_put(top + h - 1, lc, TC_ACCENT, " ");
    tui_put(top + h - 1, lc + 1, (phase & 2) ? TC_ACCENT : TC_DIM, tui_glyph(G_DOT_ON));
    tui_put(top + h - 1, lc + 2, TC_ACCENT, " CARRIER DETECT ");
  } else {
    /* who was here last, and how long ago */
    const dash_history_entry_t *last = NULL;
    int cnt = g_dash.history_count < DASH_HISTORY_LINES ? g_dash.history_count : DASH_HISTORY_LINES;
    for(int r = 0; r < cnt && !last; r++) {
      const dash_history_entry_t *e = &g_dash.history[(g_dash.history_head - 1 - r + DASH_HISTORY_LINES) % DASH_HISTORY_LINES];
      if(e->result >= 2) last = e;
    }
    if(last) {
      long ago = (long)(time(NULL) - last->connect_time) - last->duration; if(ago < 0) ago = 0;
      if(ago < 3600) snprintf(b1, sizeof(b1), "%ld MIN AGO", ago / 60); else if(ago < 86400) snprintf(b1, sizeof(b1), "%ld H AGO", ago / 3600); else snprintf(b1, sizeof(b1), "%ld D AGO", ago / 86400);
      snprintf(info, sizeof(info), "WAITING FOR A CALL   LAST %s  %s", last->ip, b1);
    } else snprintf(info, sizeof(info), "WAITING FOR A CALL");
    int len = cells_of(info), sw = (inner - len - 4) / 2; if(sw > 12) sw = 12;
    int c0 = left + 2 + (inner - len) / 2;
    put_center(irow, left + 2, inner, TC_DIM, info);
    if(sw >= 4) { scanner(irow, c0 - 2 - sw, sw, phase, 0); scanner(irow, c0 + len + 2, sw, phase, 1); }
    int lc = left + (w - 14) / 2;
    tui_put(top + h - 1, lc, TC_DIM, " ");
    tui_put(top + h - 1, lc + 1, TC_DIM, tui_glyph(G_DOT_OFF));
    tui_put(top + h - 1, lc + 2, TC_DIM, " NO CARRIER ");
  }
}

/* ---- modem front panel -------------------------------------------------------
 * The eight lights of a Hayes modem for every line, read left to right as on
 * the real thing: MR modem ready, TR terminal ready (DTR from the BBS),
 * SD / RD send / receive data (they flicker with the traffic), OH off hook,
 * CD carrier detect, AA auto answer (S0 > 0), HS high speed. */
#define NLED 8
static const char *LED_NAME[NLED] = { "MR", "TR", "SD", "RD", "OH", "CD", "AA", "HS" };
static void line_leds(const dash_line_t *l, int phase, int *on, int *color) {
  time_t now = time(NULL);
  const modem_config *c = (const modem_config *)l->cfg;
  int tx = l->connected && now - g_dash.last_caller_tx <= 1, rx = l->connected && now - g_dash.last_caller_rx <= 1;
  on[0] = 1;                         color[0] = TC_OK;
  on[1] = l->dtr;                    color[1] = TC_OK;
  on[2] = tx && (phase & 1);         color[2] = TC_ACCENT2;
  on[3] = rx && !(phase & 1);        color[3] = TC_ACCENT;
  on[4] = l->connected;              color[4] = TC_WARN;
  on[5] = l->connected;              color[5] = TC_ACCENT;
  on[6] = c && c->s[0] > 0;          color[6] = TC_WARN;
  on[7] = l->speed >= 2400;          color[7] = TC_OK;
}
static void put_leds(int row, int col, const dash_line_t *l, int phase) {
  int on[NLED], color[NLED]; line_leds(l, phase, on, color);
  for(int i = 0; i < NLED; i++) tui_put(row, col + i * 3, on[i] ? color[i] : TC_FRAME, tui_glyph(on[i] ? G_DOT_ON : G_DOT_OFF));
}
static const char *short_dev(const char *d) { return strncmp(d, "/dev/", 5) == 0 ? d + 5 : d; }
static void draw_lines_panel(int top, int left, int h, int w) {
  int right = left + w - 1, phase = anim_phase();
  int total = 0, online = lines_online(&total);
  tui_box_c(top, left, h, w, "LINES", TC_REV_ACCENT);
  { char t[32]; snprintf(t, sizeof(t), " %d/%d ONLINE ", online, total); tui_put(top, right - 2 - (int)strlen(t), online ? TC_ACCENT : TC_DIM, t); }
  int led_col = right - 1 - NLED * 3;                   /* lights right aligned */
  int x = left + 2, room = led_col - x - 2;              /* cells for the text columns */
  /* the caller always fits; duration, baud and state give way first on a
   * narrow panel (the lights and the session banner say the same) */
  int show_dur = room >= 3 + 8 + 6 + 9 + 17 + 6, show_baud = room >= 3 + 8 + 6 + 9 + 17, show_state = room >= 3 + 8 + 9 + 17;
  int devw = room - 3 - 17 - (show_dur ? 6 : 0) - (show_baud ? 6 : 0) - (show_state ? 9 : 0);
  if(devw > 16) devw = 16;
  if(devw < 4) devw = 4;
  int c_dev = x + 3, c_spd = c_dev + devw + 1, c_st = c_spd + (show_baud ? 6 : 0), c_call = c_st + (show_state ? 9 : 0), c_dur = c_call + 17;
  tui_put(top + 1, x, TC_HEAD, "LN"); tui_putn(top + 1, c_dev, TC_HEAD, "DEVICE", devw);
  if(show_baud) tui_put(top + 1, c_spd, TC_HEAD, " BAUD");
  if(show_state) tui_put(top + 1, c_st, TC_HEAD, "STATE");
  put_clip(top + 1, c_call, TC_HEAD, "CALLER", led_col - 1);
  if(show_dur) tui_put(top + 1, c_dur, TC_HEAD, "  DUR");
  for(int i = 0; i < NLED; i++) tui_put(top + 1, led_col + i * 3, TC_HEAD, LED_NAME[i]);
  int r = top + 2, shown = 0, target = total > 1 ? target_line() : -1;
  char b1[32], line[64];
  for(int i = 0; i < DASH_MAX_LINES && r < top + h - 1; i++) {
    dash_line_t *l = &g_dash.lines[i];
    if(!l->inited) continue;
    tui_put(r, left + 1, TC_ACCENT, i == target ? ">" : " ");
    snprintf(line, sizeof(line), "%2d", i); tui_put(r, x, TC_VALUE, line);
    tui_putn(r, c_dev, TC_VALUE, short_dev(l->device), devw);
    if(show_baud) { snprintf(line, sizeof(line), "%5d", l->speed); tui_put(r, c_spd, TC_VALUE, line); }
    if(show_state) tui_put(r, c_st, l->connected ? TC_REV_ACCENT : TC_DIM, l->connected ? " ONLINE " : "IDLE");
    if(l->connected) {
      snprintf(line, sizeof(line), "%-16.16s", l->peer_ip);
      put_clip(r, c_call, allowlist_has(l->peer_ip) ? TC_OK : TC_VALUE, line, led_col - 1);
      if(show_dur) { snprintf(line, sizeof(line), "%5s", fmt_dur((int)(time(NULL) - l->session_start), b1, sizeof(b1))); tui_put(r, c_dur, TC_VALUE, line); }
    } else if(!show_state) put_clip(r, c_call, TC_DIM, "IDLE", led_col - 1);
    put_leds(r, led_col, l, phase);
    r++; shown++;
  }
  if(!shown) tui_put(r, x, TC_DIM, "NO LINES CONFIGURED");
  /* spare rows: what the lights mean, set into the bottom of the panel */
  if(r < top + h - 2) {
    const char *legend = "MR READY  TR DTR  SD SEND  RD RECV  OH OFF HOOK  CD CARRIER  AA AUTO ANSWER  HS HIGH SPEED";
    if((int)strlen(legend) <= w - 4) put_center(top + h - 2, left + 2, w - 4, TC_DIM, legend);
    else put_center(top + h - 2, left + 2, w - 4, TC_DIM, "SD SEND  RD RECV  OH OFF HOOK  CD CARRIER  AA AUTO ANSWER");
  }
}

/* ---- lines ----------------------------------------------------------------- */
static void draw_lines_box(int top, int *next) {
  int n = 0;
  for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited) n++;
  if(n == 0) n = 1;
  int h = n + 3;
  tui_box_c(top, 0, h, W, "LINES", TC_REV_ACCENT);
  /* column table: {offset, width}; the header is built from the same table so
   * it cannot drift from the rows. Total 78 cells: fits an 80-column terminal. */
  enum { C_LN = 0, C_DEV = 3, C_SPD = 18, C_ST = 24, C_CALL = 33, C_DUR = 50, C_IN = 56, C_OUT = 63, C_DCD = 71, C_DTR = 75 };
  const int X = 2;
  tui_put(top + 1, X + C_LN, TC_HEAD, "LN"); tui_put(top + 1, X + C_DEV, TC_HEAD, "DEVICE"); tui_put(top + 1, X + C_SPD, TC_HEAD, "SPEED");
  tui_put(top + 1, X + C_ST, TC_HEAD, "STATE"); tui_put(top + 1, X + C_CALL, TC_HEAD, "CALLER"); put_right(top + 1, X + C_DUR, TC_HEAD, "DUR", 5);
  put_right(top + 1, X + C_IN, TC_HEAD, "IN", 6); put_right(top + 1, X + C_OUT, TC_HEAD, "OUT", 7); tui_put(top + 1, X + C_DCD, TC_HEAD, "DCD"); tui_put(top + 1, X + C_DTR, TC_HEAD, "DTR");
  if(W >= 108) for(int i = 0; i < NLED; i++) tui_put(top + 1, W - 2 - NLED * 3 + i * 3, TC_HEAD, LED_NAME[i]);
  int r = top + 2, shown = 0;
  int target = n > 1 ? target_line() : -1;   /* marker only when there is a choice */
  char b1[32], line[64];
  for(int i = 0; i < DASH_MAX_LINES; i++) {
    dash_line_t *l = &g_dash.lines[i];
    if(!l->inited) continue;
    tui_put(r, 1, TC_ACCENT, i == target ? ">" : " ");
    snprintf(line, sizeof(line), "%2d", i); tui_put(r, X + C_LN, TC_VALUE, line);
    put_padded(r, X + C_DEV, TC_VALUE, l->device, 14);
    snprintf(line, sizeof(line), "%5d", l->speed); tui_put(r, X + C_SPD, TC_VALUE, line);
    put_padded(r, X + C_ST, l->connected ? TC_REV_ACCENT : TC_DIM, l->connected ? " ONLINE" : "IDLE", 8);
    if(l->connected) {
      put_padded(r, X + C_CALL, allowlist_has(l->peer_ip) ? TC_OK : TC_VALUE, l->peer_ip, 16);
      put_right(r, X + C_DUR, TC_VALUE, fmt_dur((int)(time(NULL) - l->session_start), b1, sizeof(b1)), 5);
      put_right(r, X + C_IN, TC_ACCENT, fmt_bytes(l->bytes_in, b1, sizeof(b1)), 6);
      put_right(r, X + C_OUT, TC_ACCENT2, fmt_bytes(l->bytes_out, b1, sizeof(b1)), 7);
    }
    tui_put(r, X + C_DCD + 1, l->connected ? TC_OK : TC_DIM, tui_glyph(l->connected ? G_DOT_ON : G_DOT_OFF));
    tui_put(r, X + C_DTR + 1, l->dtr ? TC_OK : TC_DIM, tui_glyph(l->dtr ? G_DOT_ON : G_DOT_OFF));
    if(W >= 108) put_leds(r, W - 2 - NLED * 3, l, anim_phase());   /* the modem lights, when there is room */
    r++; shown++;
  }
  if(!shown) tui_put(r, 2, TC_DIM, "NO LINES CONFIGURED");
  *next = top + h;
}

/* ---- traffic, callers, events (each fits the box it is given) ------------- */
static void draw_traffic_box(int top, int left, int h, int w) {
  if(h < 3) return;
  tui_box_c(top, left, h, w, "TRAFFIC", TC_REV_ACCENT2);
  int rows = h - 3;
  int dir_w = 11, size_w = 5;
  int avail = w - 2 - dir_w - 1 - size_w - 4;     /* borders, dir, gap, size, separators */
  int hex_bytes = avail / 4; if(hex_bytes > 32) hex_bytes = 32; if(hex_bytes < 4) hex_bytes = 4;
  int hex_w = hex_bytes * 3 - 1, txt_w = hex_bytes;
  int col_dir = left + 2, col_hex = col_dir + dir_w + 1, col_txt = col_hex + hex_w + 2, col_size = left + w - 2 - size_w;
  tui_put(top + 1, col_dir, TC_HEAD, "DIRECTION");
  tui_put(top + 1, col_hex, TC_HEAD, "HEX");
  tui_put(top + 1, col_txt, TC_HEAD, "TEXT");
  put_right(top + 1, col_size, TC_HEAD, "SIZE", size_w);
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
  if(show == 0) tui_put(top + 2, left + 2, TC_DIM, "NO TRAFFIC YET");
}

static void draw_callers_box(int top, int left, int h, int w) {
  if(h < 3) return;
  int right = left + w - 1;
  tui_box_c(top, left, h, w, "RECENT CALLERS", TC_REV);
  put_clip(top + 1, left + 2, TC_HEAD, "TIME     CALLER           RESULT     DUR      IN     OUT", right);
  int rows = h - 3;
  int hshow = g_dash.history_count < DASH_HISTORY_LINES ? g_dash.history_count : DASH_HISTORY_LINES;
  if(hshow > rows) hshow = rows;
  char b1[32], b2[32], b3[32], line[160];
  for(int r = 0; r < hshow; r++) {
    int idx = (g_dash.history_head - 1 - r + DASH_HISTORY_LINES) % DASH_HISTORY_LINES;   /* newest first */
    dash_history_entry_t *e = &g_dash.history[idx];
    int row = top + 2 + r;
    put_clip(row, left + 2, TC_INFO, fmt_clock(e->connect_time, b1, sizeof(b1)), right);
    snprintf(line, sizeof(line), "%-16.16s", e->ip);
    put_clip(row, left + 11, allowlist_has(e->ip) ? TC_OK : TC_VALUE, line, right);
    snprintf(line, sizeof(line), "%-10s", result_str(e->result));
    put_clip(row, left + 28, result_color(e->result), line, right);
    if(e->result >= 2) {
      put_clip(row, left + 39, TC_VALUE, fmt_dur(e->duration, b1, sizeof(b1)), right);
      snprintf(line, sizeof(line), "%6s", fmt_bytes((unsigned long)e->bytes_in, b2, sizeof(b2)));
      put_clip(row, left + 45, TC_ACCENT, line, right);
      snprintf(line, sizeof(line), "%7s", fmt_bytes((unsigned long)e->bytes_out, b3, sizeof(b3)));
      put_clip(row, left + 52, TC_ACCENT2, line, right);
    }
  }
  if(hshow == 0) put_clip(top + 2, left + 2, TC_DIM, "NO CALLS YET", right);
}

/* an event takes the colour of what happened: red for blocks and floods,
 * yellow for warnings and failures, green for calls that got through */
static int has_word(const char *s, const char *w) {
  size_t n = strlen(w);
  for(; *s; s++) { size_t i = 0; while(i < n && s[i] && (s[i] | 0x20) == w[i]) i++; if(i == n) return 1; }
  return 0;
}
static int event_color(const char *t) {
  static const char *bad[] = { "block", "flood", "rate", "denied", "refused", "error" };
  static const char *warn[] = { "warn", "fail", "timeout", "hang", "reset", "not reading", "dropped" };
  static const char *ok[] = { "connect", "pass", "answer", "unblock", "cleared", "listening", "allowlist" };
  for(unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) if(has_word(t, bad[i])) return TC_BAD;
  for(unsigned i = 0; i < sizeof(warn) / sizeof(warn[0]); i++) if(has_word(t, warn[i])) return TC_WARN;
  for(unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if(has_word(t, ok[i])) return TC_OK;
  return TC_VALUE;
}

static void draw_events_box(int top, int left, int h, int w) {
  if(h < 3) return;
  int right = left + w - 1;
  tui_box_c(top, left, h, w, "EVENTS", TC_REV_WARN);
  int rows = h - 2;
  int n = g_dash.events_count < DASH_EVENTS ? g_dash.events_count : DASH_EVENTS;
  if(n > rows) n = rows;
  char b1[32];
  for(int i = 0; i < n; i++) {
    int idx = (g_dash.events_head - 1 - i + DASH_EVENTS) % DASH_EVENTS;
    put_clip(top + 1 + i, left + 2, TC_INFO, fmt_clock(g_dash.events[idx].when, b1, sizeof(b1)), right);
    put_clip(top + 1 + i, left + 11, event_color(g_dash.events[idx].text), g_dash.events[idx].text, right - 1);
  }
  if(n == 0) put_clip(top + 1, left + 2, TC_DIM, "NOTHING YET", right);
}

/* the lists page: blocklist on top, allowlist below */
static void draw_blocklist_page(int top, int h) {
  int ah = h / 2; if(ah < 4) ah = h >= 8 ? 4 : 0;
  int bh = h - ah;
  tui_box_c(top, 0, bh, W, "BLOCKLIST", TC_REV_BAD);
  block_info_t bl[64];
  int n = blocklist_list_ex(bl, 64);
  tui_put(top + 1, 2, TC_HEAD, "IP               KIND       UNTIL");
  int rows = bh - 3; if(n > rows) n = rows;
  char b1[32], line[128];
  for(int i = 0; i < n; i++) {
    snprintf(line, sizeof(line), "%-16.16s ", bl[i].ip);
    tui_put(top + 2 + i, 2, TC_VALUE, line);
    tui_put(top + 2 + i, 19, bl[i].permanent ? TC_BAD : TC_WARN, bl[i].permanent ? "PERMANENT " : "TIMED     ");
    if(!bl[i].permanent) tui_put(top + 2 + i, 30, TC_VALUE, fmt_clock(bl[i].until, b1, sizeof(b1)));
  }
  if(n == 0) tui_put(top + 2, 2, TC_DIM, "NOTHING BLOCKED");
  if(ah) {
    int at = top + bh;
    tui_box_c(at, 0, ah, W, "ALLOWLIST", TC_REV);
    char al[64][16];
    int m = allowlist_list(al, 64), per = (W - 4) / 17, arows = ah - 3;
    if(per < 1) per = 1;
    for(int i = 0; i < m && i / per < arows; i++) tui_put(at + 1 + i / per, 2 + (i % per) * 17, TC_OK, al[i]);
    if(m == 0) tui_putn(at + 1, 2, TC_DIM, "NOBODY ALLOWED YET: THESE ADDRESSES SKIP THE GATE AND THE BLOCKLIST", W - 4);
    tui_putn(at + ah - 2, 2, TC_DIM, "A=ALLOW THE LATEST CALLER   X=REMOVE THE LAST ENTRY   U=CLEAR TIMED BLOCKS   S OR ESC=BACK", W - 4);
  } else tui_put(top + bh - 2, 2, TC_DIM, "U=CLEAR TIMED BLOCKS   S OR ESC=BACK");
}

static void draw_help_page(int top, int h) {
  tui_box_c(top, 0, h, W, "HELP", TC_REV_INFO);
  static const char *lines[] = {
    "H  HANG UP THE CALLER ON THE MARKED LINE (ASKS Y/N)",
    "R  RESET THE MARKED LINE'S MODEM, LIKE ATZ (ASKS Y/N)",
    "0-7  MARK A LINE FOR H AND R (MULTI-LINE; DEFAULT: THE CALLER)",
    "A  ALLOW THE CALLER (OR THE LATEST ONE): SKIPS GATE AND BLOCKLIST",
    "G  GRAPHS PAGE: THROUGHPUT, TRAFFIC PER MINUTE, CALLS, TOP SOURCES",
    "B  BLOCKLIST + ALLOWLIST PAGE   U  CLEAR TIMED BLOCKS (ASKS Y/N)",
    "P  PALETTE COLOR / GREEN / AMBER / WHITE",
    "S  BACK TO THE MAIN SCREEN / REPAINT",
    "Q  QUIT TCPSER (ASKS Y/N): HANGS UP CALLERS, SAVES STATS",
    "",
    "SESSION SHOWS ONLINE OR STANDBY. LINES HAS THE MODEM LIGHTS: MR READY,",
    "TR DTR, SD SEND, RD RECEIVE, OH OFF HOOK, CD CARRIER, AA AUTO ANSWER,",
    "HS HIGH SPEED. DATA FLOW: ARROWS MOVE WHILE BYTES DO, TURQUOISE TO THE",
    "BBS, PINK TO THE CALLER; SHIELD FLASHES RED ON A DROP, GATE YELLOW ON",
    "A FAILURE. GREEN ADDRESSES ARE ON THE ALLOWLIST.",
    "TERMINAL: 60X16 MINIMUM; A TALLER SCREEN SHOWS MORE PANELS.",
    "COLOURS: 256 / 16 / MONO FROM $TERM; NO_COLOR AND TCPSER_TUI_ASCII",
    "ARE HONOURED; TCPSER_TUI_PHOSPHOR=COLOR|GREEN|AMBER|WHITE.",
  };
  int n = (int)(sizeof(lines) / sizeof(lines[0]));
  for(int i = 0; i < n && i < h - 2; i++) tui_putn(top + 1 + i, 2, i > 9 ? TC_DIM : TC_LABEL, lines[i], W - 4);
  for(int i = 0; i < 9 && i < h - 2; i++) tui_putn(top + 1 + i, 2, TC_KEY, lines[i], lines[i][1] == '-' ? 3 : 1);
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
  char b1[64];
  /* system line: product, name between scene-style brackets, carrier lamp, clock */
  {
    int on = lines_online(NULL), fade = tui_is_utf8() && W >= 100;
    int c = 1;
    if(fade) { tui_put(0, 1, TC_ACCENT, "\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93"); c = 5; }
    tui_put(0, c, TC_ACCENT, "TCPSER " VERSION);
    const char *name = EDITION;
    int len = (int)strlen(name), nc = (W - len) / 2;
    tui_put(0, nc, TC_VALUE, name);
    if(W >= 90) { tui_put(0, nc - 4, TC_KEY, "-=["); tui_put(0, nc + len + 1, TC_KEY, "]=-"); len += 8; }
    time_t now = time(NULL); struct tm tmb; localtime_r(&now, &tmb);
    strftime(b1, sizeof(b1), "%d/%m/%y %H:%M:%S", &tmb);
    int cc = W - 1 - (int)strlen(b1) - (fade ? 4 : 0);
    tui_put(0, cc, TC_KEY, b1);
    if(fade) tui_put(0, W - 4, TC_ACCENT, "\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91");
    const char *lamp = on ? " ON AIR " : " NO CARRIER ";
    int lc = cc - 2 - (int)strlen(lamp);
    if(lc > (W + len) / 2 + 2) tui_put(0, lc, on ? ((anim_phase() & 2) ? TC_REV_ACCENT : TC_REV) : TC_DIM, lamp);
  }
  tui_hrule(1, 0, W);

  int top = 2;
  int msg_row = H - 2, keys_row = H - 1;
  if(g_page == PG_BLOCKS)      draw_blocklist_page(top, msg_row - top);
  else if(g_page == PG_HELP)   draw_help_page(top, msg_row - top);
  else if(g_page == PG_GRAPHS) { draw_kpi_strip(top); draw_graphs_page(top + 1, msg_row - top - 1); }
  else {
    /* Height budget. Start from the least every screen gets (counter strip,
     * compact flow, lines, a bottom area) and spend what is left on upgrades,
     * most valuable first. */
    int nl = 0; lines_online(&nl); if(nl == 0) nl = 1;
    int band = W >= 120;                              /* room for the banner beside the modem panel */
    int lines_h = band ? (nl + 3 > 6 ? nl + 3 : 6) : nl + 3;
    int R = msg_row - top;
    int kpi_h = 1, flow_h = 4, graph_h = 0;
    int bottom = R - kpi_h - flow_h - lines_h;
    /* rows the bottom area must keep: less when its panes sit side by side */
    int minb = W >= 124 ? 8 : W >= 100 ? 10 : 12;
    if(bottom < 5) { bottom += flow_h; flow_h = 0; }                     /* tiny screen: no flow */
    if(flow_h && bottom - 1 >= minb) { flow_h = 5; bottom -= 1; }        /* both arrow rows */
    if(bottom - 5 >= minb) { graph_h = 5; bottom -= 5; }                 /* graphs */
    if(W >= 96 && bottom - 2 >= minb) { kpi_h = 3; bottom -= 2; }        /* counters as two rows + a blank */
    if(graph_h && bottom - 2 >= minb + 3) { graph_h += 2; bottom -= 2; }
    if(graph_h && bottom - 2 >= minb + 6) { graph_h += 2; bottom -= 2; }

    int y = top, next = top;
    if(kpi_h == 3) draw_kpi_tiles(y); else draw_kpi_strip(y);
    y += kpi_h;
    /* who is on the line comes first */
    if(band) { int lw = W / 2; draw_session(y, 0, lines_h, lw); draw_lines_panel(y, lw, lines_h, W - lw); y += lines_h; }
    else { draw_lines_box(y, &next); y = next; }
    if(flow_h) { draw_flow(y, flow_h); y += flow_h; }
    if(graph_h) { draw_graph_row(y, graph_h); y += graph_h; }
    int remaining = msg_row - y;
    if(W >= 124 && remaining >= 8) {
      /* wide: traffic on the left, callers over events on the right */
      int lw = W * 56 / 100, rw = W - lw, ch = (remaining + 1) / 2;
      draw_traffic_box(y, 0, remaining, lw);
      draw_callers_box(y, lw, ch, rw);
      draw_events_box(y + ch, lw, remaining - ch, rw);
    } else if(W >= 100 && remaining >= 9) {
      /* medium: traffic across, then callers and events side by side */
      int low = remaining >= 14 ? 6 : 5, cw = 60;
      draw_traffic_box(y, 0, remaining - low, W);
      draw_callers_box(y + remaining - low, 0, low, cw);
      draw_events_box(y + remaining - low, cw, low, W - cw);
    } else {
      /* stacked; on a short terminal the panes are dropped from the bottom
       * up: events first, then callers. Traffic keeps at least two rows. */
      int ev_h = remaining >= 14 ? 4 : remaining >= 12 ? 3 : 0;
      int cal_h = remaining >= 16 ? 6 : remaining >= 12 ? 5 : 4;
      int traf_h = remaining - ev_h - cal_h;
      if(traf_h < 4) { ev_h = 0; traf_h = remaining - cal_h; }
      if(traf_h < 4) { cal_h = 0; traf_h = remaining; }
      if(traf_h >= 3) { draw_traffic_box(y, 0, traf_h, W); y += traf_h; }
      if(cal_h)       { draw_callers_box(y, 0, cal_h, W);  y += cal_h; }
      if(ev_h)        draw_events_box(y, 0, ev_h, W);
    }
  }

  /* message line + hotkeys */
  if(g_msg[0] && time(NULL) - g_msg_at > 8 && !g_confirm) g_msg[0] = 0;
  if(g_msg[0]) tui_putn(msg_row, 1, g_confirm ? TC_WARN : TC_VALUE, g_msg, W - 2);
  {
    static const char *keys_long[][2]  = { {"H", "HANG UP"}, {"R", "RESET"}, {"A", "ALLOW"}, {"G", "GRAPHS"}, {"B", "LISTS"}, {"U", "UNBLOCK"}, {"P", "PALETTE"}, {"?", "HELP"}, {"Q", "QUIT"} };
    static const char *keys_mid[][2]   = { {"H", "HANG"}, {"R", "RST"}, {"A", "ALLOW"}, {"G", "GRAPH"}, {"B", "LIST"}, {"U", "UNBL"}, {"P", "PAL"}, {"?", "HELP"}, {"Q", "QUIT"} };
    static const char *keys_short[][2] = { {"H", "HANG"}, {"R", "RST"}, {"A", "ALLOW"}, {"G", "GRAPH"}, {"B", "LIST"}, {"U", "UNBL"}, {"?", "HELP"}, {"Q", "QUIT"} };
    const char *(*keys)[2] = W >= 88 ? keys_long : W >= 72 ? keys_mid : keys_short;   /* every key stays visible down to 60 columns (P is in the help there) */
    int nkeys = W >= 72 ? 9 : 8, gap = W >= 72 ? 2 : 1;
    const char *ph = tui_phosphor();
    int ph_col = W - 1 - (int)strlen(ph), c = 1;
    for(int i = 0; i < nkeys; i++) {
      int need = 2 + (int)strlen(keys[i][1]);
      if(c + need > ph_col - 2) { ph_col = -1; if(c + need > W - 1) break; }   /* keys win over the palette name */
      char k[8]; snprintf(k, sizeof(k), "%s=", keys[i][0]);
      int here = (g_page == PG_GRAPHS && keys[i][0][0] == 'G') || (g_page == PG_BLOCKS && keys[i][0][0] == 'B') || (g_page == PG_HELP && keys[i][0][0] == '?');
      tui_put(keys_row, c, TC_KEY, k); c += 2;
      tui_put(keys_row, c, here ? TC_REV_INFO : TC_LABEL, keys[i][1]); c += (int)strlen(keys[i][1]) + gap;
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

/* Keyboard — send commands through pipe protocol (thread-safe).
 * H and R act on ONE line: the one picked with the digit keys, or, until the
 * operator picks, the first line with a caller (else the first line). They
 * used to act on whichever line had registered last, so on a multi-line BBS
 * "hang up" could hit an idle line and "reset" could drop the wrong caller. */
static int target_line(void) {
  if(g_sel_line >= 0 && g_sel_line < DASH_MAX_LINES && g_dash.lines[g_sel_line].inited) return g_sel_line;
  for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited && g_dash.lines[i].connected) return i;
  for(int i = 0; i < DASH_MAX_LINES; i++) if(g_dash.lines[i].inited) return i;
  return -1;
}

static void line_msg(int line, char msg) {
  if(line < 0 || line >= DASH_MAX_LINES || !g_dash.lines[line].inited) return;
  modem_config *cfg = (modem_config *)g_dash.lines[line].cfg;
  if(!cfg) return;
  writePipe(cfg->mp[1][1], msg);
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
void dashboard_counters(unsigned long *connections, unsigned long *blocked, unsigned long *gate_pass, unsigned long *gate_fail) {
  pthread_mutex_lock(&dash_mutex);
  if(connections) *connections = g_dash.total_connections;
  if(blocked) *blocked = g_dash.blocked;
  if(gate_pass) *gate_pass = g_dash.gate_pass;
  if(gate_fail) *gate_fail = g_dash.gate_fail;
  pthread_mutex_unlock(&dash_mutex);
}

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
  int sx = (int)(now % 60);                                   /* per-second ring for the throughput graph */
  if(g_dash.sec_stamp[sx] != now) { g_dash.sec_stamp[sx] = now; g_dash.sec_in[sx] = 0; g_dash.sec_out[sx] = 0; }
  if(in) g_dash.sec_in[sx] += (unsigned long)len; else g_dash.sec_out[sx] += (unsigned long)len;
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

/* The lifetime counters count whether or not a dashboard is enabled: the
 * SIGUSR1 dump and -Y stats read them. Only the drawing is conditional. */
void dashboard_stat_connection(const char *ip) {
  pthread_mutex_lock(&dash_mutex);
  g_dash.total_connections++;
  g_dash.last_attempt = time(NULL);
  if(g_dash.enabled) { series_call(); render(); }
  pthread_mutex_unlock(&dash_mutex);
  (void)ip;
}

void dashboard_stat_blocked(const char *ip) {
  pthread_mutex_lock(&dash_mutex);
  g_dash.blocked++;
  g_dash.last_drop = g_dash.last_attempt = time(NULL);
  if(!g_dash.enabled) { pthread_mutex_unlock(&dash_mutex); return; }
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
  pthread_mutex_lock(&dash_mutex);
  if(passed) { g_dash.gate_pass++; g_dash.last_gate_pass = time(NULL); } else { g_dash.gate_fail++; g_dash.last_gate_fail = time(NULL); }
  if(!g_dash.enabled) { pthread_mutex_unlock(&dash_mutex); return; }
  if(!passed) {
    series_fail();
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
          case 'h': line_msg(g_confirm_line, MSG_HANGUP); set_msg("HANG UP REQUESTED ON LINE %d", g_confirm_line); break;
          case 'r': line_msg(g_confirm_line, MSG_RESET); set_msg("LINE %d: MODEM RESET SENT (ATZ)", g_confirm_line); break;
          case 'u': { int n = blocklist_unblock_all_timed(); set_msg("CLEARED %d TIMED BLOCK(S)", n); break; }
          case 'a': { int rc = allowlist_add(g_confirm_ip);
                      if(rc == 0) { set_msg("%s ALLOWED: SKIPS THE GATE FROM NOW ON", g_confirm_ip); dashboard_note("%s added to the allowlist", g_confirm_ip); }
                      else set_msg(rc == 1 ? "%s IS ALREADY ALLOWED" : rc == -3 ? "ALLOWLIST IS FULL" : "CANNOT WRITE THE ALLOWLIST FILE", g_confirm_ip);
                      break; }
          case 'x': { int rc = allowlist_remove(g_confirm_ip);
                      if(rc == 0) { set_msg("%s REMOVED FROM THE ALLOWLIST", g_confirm_ip); dashboard_note("%s removed from the allowlist", g_confirm_ip); }
                      else set_msg(rc == 1 ? "%s WAS NOT LISTED" : "CANNOT WRITE THE ALLOWLIST FILE", g_confirm_ip);
                      break; }
          case 'q': g_quit_requested = 1; return;
        }
      } else set_msg("CANCELLED");
      continue;
    }
    switch(ch) {
      case 'h': case 'H': {
        int t = target_line();
        char q[80];
        if(t >= 0 && g_dash.lines[t].connected) {
          snprintf(q, sizeof(q), "HANG UP LINE %d (%s)", t, g_dash.lines[t].peer_ip);
          g_confirm_line = t; ask_confirm('h', q);
        } else if(t >= 0 && g_sel_line >= 0) set_msg("NO CALLER ON LINE %d", t);
        else set_msg("NO CALLER ONLINE");
        break; }
      case 'r': case 'R': {
        int t = target_line();
        char q[80];
        if(t < 0) { set_msg("NO LINES CONFIGURED"); break; }
        snprintf(q, sizeof(q), "RESET LINE %d%s", t, g_dash.lines[t].connected ? " (DROPS ITS CALLER)" : "");
        g_confirm_line = t; ask_confirm('r', q);
        break; }
      case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int t = ch - '0';
        if(g_dash.lines[t].inited) { g_sel_line = (g_sel_line == t) ? -1 : t; if(g_sel_line >= 0) set_msg("LINE %d MARKED FOR H AND R", t); else set_msg("MARK CLEARED: H AND R FOLLOW THE CALLER"); }
        else set_msg("NO LINE %d", t);
        break; }
      case 'a': case 'A': {
        /* the caller on the marked / active line, else whoever called last */
        int t = target_line();
        const char *ip = NULL;
        if(t >= 0 && g_dash.lines[t].connected && g_dash.lines[t].peer_ip[0]) ip = g_dash.lines[t].peer_ip;
        else if(g_dash.history_count > 0) ip = g_dash.history[(g_dash.history_head - 1 + DASH_HISTORY_LINES) % DASH_HISTORY_LINES].ip;
        char q[96];
        if(!ip || !ip[0]) { set_msg("NO CALLER TO ALLOW YET"); break; }
        if(allowlist_has(ip)) { set_msg("%s IS ALREADY ALLOWED", ip); break; }
        snprintf(g_confirm_ip, sizeof(g_confirm_ip), "%.19s", ip);
        snprintf(q, sizeof(q), "ALLOW %s (SKIPS GATE AND BLOCKLIST)", g_confirm_ip);
        ask_confirm('a', q);
        break; }
      case 'x': case 'X': {
        char al[256][16]; int m = allowlist_list(al, 256);
        char q[96];
        if(g_page != PG_BLOCKS) { set_msg("X WORKS ON THE LISTS PAGE (B)"); break; }
        if(m == 0) { set_msg("THE ALLOWLIST IS EMPTY"); break; }
        snprintf(g_confirm_ip, sizeof(g_confirm_ip), "%s", al[m - 1]);
        snprintf(q, sizeof(q), "REMOVE %s FROM THE ALLOWLIST", g_confirm_ip);
        ask_confirm('x', q);
        break; }
      case 'b': case 'B': g_page = (g_page == PG_BLOCKS) ? PG_MAIN : PG_BLOCKS; tui_invalidate(); break;
      case 'g': case 'G': g_page = (g_page == PG_GRAPHS) ? PG_MAIN : PG_GRAPHS; tui_invalidate(); break;
      case '?': case '/': g_page = (g_page == PG_HELP) ? PG_MAIN : PG_HELP; tui_invalidate(); break;
      case 's': case 'S': case 27: g_page = PG_MAIN; g_msg[0] = 0; tui_invalidate(); break;
      case 'u': case 'U': ask_confirm('u', "CLEAR ALL TIMED BLOCKS"); break;
      case 'p': case 'P': set_msg("PALETTE: %s", tui_next_phosphor()); break;
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
  /* Written to a temporary file and renamed into place, so a crash or power
   * cut mid-save cannot leave a truncated stats file. The persisted history
   * includes caller IP addresses (PII): created owner-only from the start. */
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int sfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if(sfd < 0) return;
  (void)fchmod(sfd, S_IRUSR | S_IWUSR);
  f = fdopen(sfd, "w");
  if(!f) { close(sfd); return; }
  pthread_mutex_lock(&dash_mutex);
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
  if(fclose(f) == 0) (void)rename(tmp, path);
  else (void)unlink(tmp);
}

/* Byte counters of the call currently on a line (for the connection log). */
void dashboard_line_bytes(int idx, long *in, long *out) {
  *in = *out = 0;
  if(idx < 0 || idx >= DASH_MAX_LINES) return;
  pthread_mutex_lock(&dash_mutex);
  *in = (long)g_dash.lines[idx].bytes_in;
  *out = (long)g_dash.lines[idx].bytes_out;
  pthread_mutex_unlock(&dash_mutex);
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
