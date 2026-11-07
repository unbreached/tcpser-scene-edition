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
#include "version.h"
#include "modem_core.h"
#include "bridge.h"
#include "util.h"

dashboard_state_t g_dash;
static pthread_mutex_t dash_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct termios g_orig_term;
static int g_dash_paused = 0;  /* pause auto-render while showing overlay */
static int g_term_raw = 0;
static char g_stats_path[512] = {0};
static time_t g_last_autosave = 0;
#define AUTOSAVE_INTERVAL 60  /* seconds */

/* ANSI */
#define E       "\033["
#define RST     E "0m"
#define BOLD    E "1m"
#define DIM     E "2m"

/* --- Cyberpunk neon palette (xterm-256) ---------------------------------
 * The dashboard names its colours semantically (C_LCYN = header, C_LMAG =
 * primary accent, C_LYEL = stats, C_LGRN = live/online, C_GRY = grid/borders).
 * We remap those names to a cohesive neon set — hot magenta, electric cyan,
 * acid green, neon amber — over a muted blue-violet grid. Layout is driven by
 * cursor positioning, so only the look changes, not the geometry.
 * Falls back gracefully: any 256-colour-capable terminal (all modern ones)
 * renders these; the SGR reset (RST) restores default. */
#define N(n)    E "38;5;" #n "m"
#define C_RED   N(197)   /* neon red/pink   */
#define C_GRN   N(41)    /* neon green      */
#define C_YEL   N(178)   /* amber           */
#define C_BLU   N(63)    /* electric blue   */
#define C_MAG   N(164)   /* magenta         */
#define C_CYN   N(44)    /* deep cyan       */
#define C_WHT   N(255)   /* near-white      */
#define C_GRY   N(60)    /* muted grid line */
#define C_LRED  N(203)   /* coral           */
#define C_LGRN  N(48)    /* acid green      */
#define C_LYEL  N(220)   /* neon gold       */
#define C_LBLU  N(75)    /* sky neon        */
#define C_LMAG  N(198)   /* hot neon pink   */
#define C_LCYN  N(51)    /* electric cyan   */
#define CLR     E "2J" E "H"
#define CHIDE   E "?25l"
#define CSHOW   E "?25h"

#define TL "\xe2\x95\xad"
#define TR "\xe2\x95\xae"
#define BL "\xe2\x95\xb0"
#define BR "\xe2\x95\xaf"
#define HZ "\xe2\x94\x80"
#define VT "\xe2\x94\x82"
#define BUL "\xe2\x97\x8f"  /* ● filled */
#define BUE "\xe2\x97\x8b"  /* ○ empty */
#define BLK "\xe2\x96\x88"  /* █ full block */

static int W = 80, H = 24;
static volatile int g_winch = 0;

static void handle_winch(int sig) { (void)sig; g_winch = 1; }

static void term_size(int *w, int *h) {
  struct winsize ws;
  if(ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) {
    if(ws.ws_col > 0) *w = ws.ws_col;
    if(ws.ws_row > 0) *h = ws.ws_row;
  }
}

static int g_quit_requested = 0;
static char g_confirm = 0;     /* pending destructive hotkey awaiting y/N */
static time_t g_confirm_at = 0;
#define CONFIRM_TIMEOUT 10     /* seconds before a pending y/N prompt auto-cancels */
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
}

void term_restore(void) {
  if(g_term_raw) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    g_term_raw = 0;
  }
}

/* Drawing helpers */
static void hr(FILE *f, int n) { for(int i=0;i<n;i++) fputs(HZ,f); }

/* Move cursor to column c (1-based) */
static void gotocol(FILE *f, int c) { fprintf(f, E "%dG", c); }

static void top_box(FILE *f, const char *title, const char *color, int w) {
  fputs(C_GRY TL, f); hr(f, 2);
  fprintf(f, " %s%s%s%s ", BOLD, color, title, RST C_GRY);
  gotocol(f, w);
  fputs(TR RST "\n", f);
}

static void bot_box(FILE *f, int w) {
  fputs(C_GRY BL, f); hr(f, w-2); fputs(BR RST "\n", f);
}

/* Print right border at exact column W */
static void rborder(FILE *f, int w) {
  gotocol(f, w);
  fprintf(f, C_GRY VT RST "\n");
}

static void row_empty(FILE *f, int w) {
  fprintf(f, C_GRY VT RST);
  gotocol(f, w);
  fprintf(f, C_GRY VT RST "\n");
}

/* Format helpers */
static const char *fmt_bytes(unsigned long b, char *buf, int sz) {
  if(b < 1024)         snprintf(buf, sz, "%lu B", b);
  else if(b < 1048576) snprintf(buf, sz, "%.1f KB", (double)b / 1024.0);
  else                 snprintf(buf, sz, "%.1f MB", (double)b / 1048576.0);
  return buf;
}

static const char *fmt_uptime(time_t start, char *buf, int sz) {
  long s = (long)(time(NULL) - start);
  long d=s/86400, h=(s%86400)/3600, m=(s%3600)/60;
  if(d > 0)      snprintf(buf, sz, "%ldd %ldh %ldm", d, h, m);
  else if(h > 0) snprintf(buf, sz, "%ldh %ldm", h, m);
  else           snprintf(buf, sz, "%ldm %lds", m, s % 60);
  return buf;
}

static const char *fmt_dur(int secs, char *buf, int sz) {
  if(secs < 60)        snprintf(buf, sz, "%ds", secs);
  else if(secs < 3600) snprintf(buf, sz, "%dm %ds", secs/60, secs%60);
  else                 snprintf(buf, sz, "%dh %dm", secs/3600, (secs%3600)/60);
  return buf;
}

static const char *result_str(int r) {
  switch(r) { case 0: return "BLOCKED"; case 1: return "GATE FAIL";
              case 2: return "GATE PASS"; case 3: return "CONNECTED"; default: return "?"; }
}
/* LED indicator: lit if activity within last 2 seconds */
static const char *led(time_t last) {
  if(last == 0) return C_GRY BUE RST;
  return (time(NULL) - last < 2) ? BOLD C_LGRN BUL RST : C_GRY BUE RST;
}

/* Traffic direction color */
static const char *traffic_color(int type) {
  return (type == 16) ? BOLD C_CYN : BOLD C_YEL;
}

/* ---- Main render ---- */

static void render(void) {
  /* Only draw when the interactive terminal dashboard (-M) owns the tty. In
   * headless mode (-H web dashboard without -M) g_term_raw is 0, so the many
   * internal render() calls from the stat/event updaters become no-ops and we
   * never spew ANSI to stderr — we just keep collecting data in g_dash. */
  if(!g_term_raw) return;
  FILE *f = stderr;
  W = 80; H = 24;
  term_size(&W, &H);
  if(W < 60) W = 60;
  if(H < 20) H = 20;

  int remaining = H - 20;
  if(remaining < 4) remaining = 4;
  int traf_rows = remaining * 3 / 5;
  int hist_rows = remaining - traf_rows;
  if(traf_rows < 2) traf_rows = 2;
  if(hist_rows < 2) hist_rows = 2;

  char b1[64], b2[64];
  int lw = W / 2, rw = W - lw;
  int inner = W - 4;

  fprintf(f, CLR CHIDE);

  /* ═══════ HEADER ═══════ */
  top_box(f, "\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91 TCPSER " VERSION " // " EDITION " \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93", C_LCYN, W);
  {
    fmt_uptime(g_dash.start_time, b1, sizeof(b1));
    fprintf(f, C_GRY VT RST " " C_LGRN "Uptime:" RST " " C_WHT "%-12s" RST
            "  " C_LGRN "Port:" RST " " C_WHT ":%-5d" RST
            "  " C_LGRN "Modem:" RST " " C_WHT "%s" RST " " C_GRY "@" RST " " C_LYEL "%d" RST,
            b1, g_dash.port, g_dash.tty, g_dash.speed);
    rborder(f, W);
  }
  bot_box(f, W);

  /* ═══════ LINK STATUS + STATISTICS (side by side) ═══════ */
  fputs(C_GRY TL, f); hr(f, 2);
  fprintf(f, " " BOLD C_LMAG "Link Status" RST C_GRY " ");
  gotocol(f, lw); fputs(TR, f);
  fputs(TL, f); hr(f, 2);
  fprintf(f, " " BOLD C_LYEL "Statistics" RST C_GRY " ");
  gotocol(f, W); fputs(TR RST "\n", f);

  /* Second column position in right panel */
  int rcol2 = lw + rw / 2;

  /* Row 1: Connection state + Connections */
  {
    fprintf(f, C_GRY VT RST " " C_CYN "State:" RST "  ");
    if(g_dash.connected) fprintf(f, BOLD C_LGRN BUL " CARRIER LOCKED" RST);
    else fprintf(f, C_LMAG BUE " AWAITING CARRIER" RST);
    gotocol(f, lw); fprintf(f, C_GRY VT RST);

    fprintf(f, C_GRY VT RST " " C_LYEL "Connections:" RST " " BOLD C_WHT "%lu" RST, g_dash.total_connections);
    gotocol(f, rcol2);
    fprintf(f, C_LYEL "Pass:" RST " " BOLD C_GRN "%lu" RST, g_dash.gate_pass);
    rborder(f, W);
  }

  /* Row 2: LED header + Blocked/Fail */
  {
    /* Left: LED column headers aligned with dots below */
    fprintf(f, C_GRY VT RST " " C_GRY "          RX TX" RST);
    gotocol(f, lw); fprintf(f, C_GRY VT RST);

    /* Right */
    int nblocked = blocklist_count();
    fprintf(f, C_GRY VT RST " " C_LYEL "Blocked:" RST " ");
    if(nblocked > 0) fprintf(f, BOLD C_RED "%d" RST, nblocked);
    else fprintf(f, C_WHT "0" RST);
    gotocol(f, rcol2);
    fprintf(f, C_LYEL "Fail:" RST " " BOLD C_RED "%lu" RST, g_dash.gate_fail);
    rborder(f, W);
  }

  /* Row 3: CALLER LEDs + Bytes */
  {
    fprintf(f, C_GRY VT RST " " C_WHT "CALLER" RST C_GRY "..:" RST " %s  %s",
            led(g_dash.last_caller_rx), led(g_dash.last_caller_tx));
    gotocol(f, lw); fprintf(f, C_GRY VT RST);

    fmt_bytes(g_dash.bytes_in, b1, sizeof(b1));
    fmt_bytes(g_dash.bytes_out, b2, sizeof(b2));
    fprintf(f, C_GRY VT RST " " C_LYEL "In:" RST " " C_LCYN "%s" RST, b1);
    gotocol(f, rcol2);
    fprintf(f, C_LYEL "Out:" RST " " C_LCYN "%s" RST, b2);
    rborder(f, W);
  }

  /* Row 4: BBS LEDs + DCD/DSR */
  {
    fprintf(f, C_GRY VT RST " " C_WHT "BBS" RST C_GRY ".....:" RST " %s  %s",
            led(g_dash.last_bbs_rx), led(g_dash.last_bbs_tx));
    gotocol(f, lw); fprintf(f, C_GRY VT RST);

    fprintf(f, C_GRY VT RST " " C_CYN "DCD:" RST " %s" BOLD "%-3s" RST,
            g_dash.dcd_on ? C_LGRN : C_LRED, g_dash.dcd_on ? "ON" : "OFF");
    gotocol(f, rcol2);
    fprintf(f, C_CYN "DSR:" RST " %s" BOLD "%-3s" RST,
            g_dash.dsr_on ? C_LGRN : C_LRED, g_dash.dsr_on ? "ON" : "OFF");
    rborder(f, W);
  }

  /* Row 5: MODEM LEDs + Peer */
  {
    fprintf(f, C_GRY VT RST " " C_WHT "MODEM" RST C_GRY "...:" RST " %s  %s",
            led(g_dash.last_bbs_rx > g_dash.last_caller_rx ? g_dash.last_bbs_rx : g_dash.last_caller_rx),
            led(g_dash.last_bbs_tx > g_dash.last_caller_tx ? g_dash.last_bbs_tx : g_dash.last_caller_tx));
    gotocol(f, lw); fprintf(f, C_GRY VT RST);

    fprintf(f, C_GRY VT RST " " C_CYN "Peer:" RST " ");
    if(g_dash.connected) {
      int dur = (int)(time(NULL) - g_dash.session_start);
      fmt_dur(dur, b1, sizeof(b1));
      fprintf(f, BOLD C_WHT "%s" RST " " C_GRY "%s" RST, g_dash.peer_ip, b1);
    } else {
      fprintf(f, C_GRY "---" RST);
    }
    rborder(f, W);
  }

  fputs(C_GRY BL, f); hr(f, lw-2);
  gotocol(f, lw); fputs(BR, f);
  fputs(BL, f); hr(f, rw-2);
  gotocol(f, W); fputs(BR RST "\n", f);

  /* ═══════ TRAFFIC (hex + ASCII) ═══════ */
  top_box(f, "Traffic", C_LCYN, W);

  int size_w = 6;
  /* Each byte = 3 chars hex + 1 char ascii (ratio 3:1).
   * Available = W - borders(2) - dir(12) - size(6) - separators(dir│hex 3 + hex│ascii 3) = W - 26
   * Per byte = 4 chars (3 hex + 1 ascii), so hex_bytes = available / 4 */
  int hex_bytes = (W - 26) / 4;
  if(hex_bytes > 32) hex_bytes = 32;
  if(hex_bytes < 4) hex_bytes = 4;
  int asc_w = hex_bytes;
  /* Position ASCII and Size from right edge using gotocol */
  int size_col = W - size_w - 1;
  int asc_col = size_col - asc_w - 1;

  /* Column headers */
  fprintf(f, C_GRY VT RST " " BOLD C_LYEL "%-12s" RST " " C_GRY VT RST " "
          BOLD C_LYEL "Hex Data" RST,
          "Direction");
  gotocol(f, asc_col - 2);
  fprintf(f, C_GRY VT RST " " BOLD C_LYEL "ASCII" RST);
  gotocol(f, size_col);
  fprintf(f, BOLD C_LYEL "%*s" RST, size_w, "Size");
  rborder(f, W);
  fprintf(f, C_GRY VT RST " " C_GRY);
  for(int i=0;i<inner;i++) fputs(HZ, f);
  fprintf(f, RST);
  rborder(f, W);

  int show = g_dash.traffic_count < DASH_TRAFFIC_LINES ? g_dash.traffic_count : DASH_TRAFFIC_LINES;

  for(int r = 0; r < traf_rows; r++) {
    if(r < show) {
      int idx = (g_dash.traffic_head - show + r + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
      dash_traffic_line_t *t = &g_dash.traffic[idx];
      const char *clr = traffic_color(t->type);

      int nb = t->rawlen;
      if(nb > hex_bytes) nb = hex_bytes;

      /* Direction */
      fprintf(f, C_GRY VT RST " %s%-12s" RST " " C_GRY VT RST " ", clr, t->dir);

      /* Hex bytes in blue */
      fprintf(f, C_LBLU);
      for(int i = 0; i < nb; i++) {
        if(i > 0) fputc(' ', f);
        fprintf(f, "%02X", t->raw[i]);
      }
      fprintf(f, RST);

      /* ASCII positioned from right side */
      gotocol(f, asc_col - 2);
      fprintf(f, C_GRY VT RST " " C_LGRN);
      for(int i = 0; i < nb; i++) {
        unsigned char c = t->raw[i];
        fputc((c >= 0x20 && c < 0x7f) ? c : '.', f);
      }
      fprintf(f, RST);

      /* Size at far right */
      gotocol(f, size_col);
      fprintf(f, DIM "%*d" RST, size_w, t->bytes);
      rborder(f, W);
    } else {
      row_empty(f, W);
    }
  }
  bot_box(f, W);

  /* ═══════ CONNECTION HISTORY ═══════ */
  top_box(f, "Connection History", C_LMAG, W);

  int c_time=10, c_ip=18;
  int c_result = inner - c_time - c_ip - 4;
  if(c_result < 10) c_result = 10;

  fprintf(f, C_GRY VT RST " " BOLD C_LYEL "%-*s  %-*s  %-*s" RST,
          c_time, "Time", c_ip, "IP Address", c_result, "Result");
  rborder(f, W);
  fprintf(f, C_GRY VT RST " " C_GRY);
  for(int i=0;i<inner;i++) fputs(HZ, f);
  fprintf(f, RST);
  rborder(f, W);

  int hshow = g_dash.history_count < DASH_HISTORY_LINES ? g_dash.history_count : DASH_HISTORY_LINES;
  for(int r = 0; r < hist_rows; r++) {
    if(r < hshow) {
      int idx = (g_dash.history_head - hshow + r + DASH_HISTORY_LINES) % DASH_HISTORY_LINES;
      dash_history_entry_t *h = &g_dash.history[idx];

      char timebuf[16];
      struct tm tmbuf;
      struct tm *tm = localtime_r(&h->connect_time, &tmbuf);
      if(tm) strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);
      else snprintf(timebuf, sizeof(timebuf), "--:--:--");

      fprintf(f, C_GRY VT RST " " DIM "%-*s" RST "  " C_WHT "%-*s" RST "  ",
              c_time, timebuf, c_ip, h->ip);

      if(h->result == 0) {
        /* Blocked */
        fprintf(f, BOLD C_RED "BLOCKED" RST);
      } else if(h->result == 1) {
        /* Gate fail */
        fprintf(f, BOLD C_YEL "GATE FAIL" RST);
      } else {
        /* Connected / Gate pass — show duration + data */
        fmt_dur(h->duration, b1, sizeof(b1));
        fmt_bytes((unsigned long)(h->bytes_in + h->bytes_out), b2, sizeof(b2));
        fprintf(f, BOLD C_GRN "%s" RST " " C_GRY "%s" RST " " C_WHT "%s" RST,
                result_str(h->result), b1, b2);
      }
      rborder(f, W);
    } else {
      row_empty(f, W);
    }
  }
  bot_box(f, W);

  /* Hotkey bar */
  fprintf(f, " " C_GRY "[" RST BOLD C_LGRN "H" RST C_GRY "]" RST " " C_WHT "Hangup" RST
          "  " C_GRY "[" RST BOLD C_LYEL "R" RST C_GRY "]" RST " " C_WHT "Reset" RST
          "  " C_GRY "[" RST BOLD C_LCYN "S" RST C_GRY "]" RST " " C_WHT "Refresh" RST
          "  " C_GRY "[" RST BOLD C_LMAG "B" RST C_GRY "]" RST " " C_WHT "Blocklist" RST
          "  " C_GRY "[" RST BOLD C_LYEL "U" RST C_GRY "]" RST " " C_WHT "Unblock" RST
          "  " C_GRY "[" RST BOLD C_LRED "Q" RST C_GRY "]" RST " " C_WHT "Quit" RST "\n");
  fflush(f);
}

/* Partial redraw: only traffic rows + LEDs + byte counter */
static void render_traffic_only(void) {
  /* Terminal-only: never draw in headless (web-only) mode — see render(). */
  if(!g_term_raw) return;
  FILE *f = stderr;
  int tw = 80, th = 24;
  term_size(&tw, &th);
  if(tw < 60) tw = 60;

  int remaining = th - 20;
  if(remaining < 4) remaining = 4;
  int traf_rows = remaining * 3 / 5;
  if(traf_rows < 2) traf_rows = 2;

  int size_w = 6;
  int hex_bytes = (tw - 26) / 4;
  if(hex_bytes > 32) hex_bytes = 32;
  if(hex_bytes < 4) hex_bytes = 4;
  int asc_w = hex_bytes;
  int size_col = tw - size_w - 1;
  int asc_col = size_col - asc_w - 1;

  int show = g_dash.traffic_count < DASH_TRAFFIC_LINES ? g_dash.traffic_count : DASH_TRAFFIC_LINES;

  /* Layout: header=3 rows, side-by-side=7 rows (1 top + 5 content + 1 bottom),
   * traffic top border=1 row, traffic column headers=2 rows. Content starts at row 14. */
  int start_row = 14;

  for(int r = 0; r < traf_rows; r++) {
    fprintf(f, E "%d;1H", start_row + r);
    if(r < show) {
      int idx = (g_dash.traffic_head - show + r + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
      dash_traffic_line_t *t = &g_dash.traffic[idx];
      const char *clr = traffic_color(t->type);
      int nb = t->rawlen;
      if(nb > hex_bytes) nb = hex_bytes;

      fprintf(f, C_GRY VT RST " %s%-12s" RST " " C_GRY VT RST " ", clr, t->dir);
      fprintf(f, C_LBLU);
      for(int i = 0; i < nb; i++) {
        if(i > 0) fputc(' ', f);
        fprintf(f, "%02X", t->raw[i]);
      }
      fprintf(f, RST);
      gotocol(f, asc_col - 2);
      fprintf(f, C_GRY VT RST " " C_LGRN);
      for(int i = 0; i < nb; i++) {
        unsigned char c = t->raw[i];
        fputc((c >= 0x20 && c < 0x7f) ? c : '.', f);
      }
      fprintf(f, RST);
      gotocol(f, size_col);
      fprintf(f, DIM "%*d" RST, size_w, t->bytes);
      gotocol(f, tw);
      fprintf(f, C_GRY VT RST);
    } else {
      fprintf(f, C_GRY VT RST);
      gotocol(f, tw);
      fprintf(f, C_GRY VT RST);
    }
  }

  /* Update LEDs (rows 7-8: header=3, panel top=1, state=1, rx/tx hdr=1, then CALLER=row 7) */
  int lw = tw / 2;
  int rw = tw - lw;
  int rcol2 = lw + rw / 2;

  fprintf(f, E "7;1H");
  fprintf(f, C_GRY VT RST " " C_WHT "CALLER" RST C_GRY "..:" RST " %s  %s",
          led(g_dash.last_caller_rx), led(g_dash.last_caller_tx));
  gotocol(f, lw);
  fprintf(f, C_GRY VT RST);

  fprintf(f, E "8;1H");
  fprintf(f, C_GRY VT RST " " C_WHT "BBS" RST C_GRY ".....:" RST " %s  %s",
          led(g_dash.last_bbs_rx), led(g_dash.last_bbs_tx));
  gotocol(f, lw);
  fprintf(f, C_GRY VT RST);

  /* Update byte counters (right panel, row 7 = In/Out) */
  {
    char b1[64], b2[64];
    fmt_bytes(g_dash.bytes_in, b1, sizeof(b1));
    fmt_bytes(g_dash.bytes_out, b2, sizeof(b2));
    fprintf(f, E "7;%dH", lw + 1);
    fprintf(f, C_GRY VT RST " " C_LYEL "In:" RST " " C_LCYN "%s" RST, b1);
    gotocol(f, rcol2);
    fprintf(f, C_LYEL "Out:" RST " " C_LCYN "%s" RST, b2);
    gotocol(f, tw);
    fprintf(f, C_GRY VT RST);
  }

  fflush(f);
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

  render_traffic_only();
  pthread_mutex_unlock(&dash_mutex);
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
  render();
  pthread_mutex_unlock(&dash_mutex);
  (void)ip;
}

void dashboard_stat_blocked(const char *ip) {
  if(!g_dash.enabled) return;
  pthread_mutex_lock(&dash_mutex);
  g_dash.blocked++;
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
  if(passed) g_dash.gate_pass++; else g_dash.gate_fail++;
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
  if(!g_dash.enabled || g_dash_paused) return;
  if(g_confirm) {
    /* Keep the y/N prompt on screen instead of repainting over it; give up
     * waiting after CONFIRM_TIMEOUT so an unanswered prompt cannot freeze
     * the display. */
    if(time(NULL) - g_confirm_at < CONFIRM_TIMEOUT) return;
    g_confirm = 0;
  }
  pthread_mutex_lock(&dash_mutex); render(); pthread_mutex_unlock(&dash_mutex);
}

int dashboard_check_winch(void) {
  if(g_winch) { g_winch = 0; dashboard_render(); return 1; }
  return 0;
}

static void do_show_blocklist(void) {
  char ips[32][16];
  int count = blocklist_list(ips, 32);
  int i;
  FILE *f = stderr;

  g_dash_paused = 1;  /* pause auto-render */
  fprintf(f, "\033[2J\033[H");  /* clear screen, cursor home */
  fprintf(f, "\n" BOLD C_LMAG "  Blocklist (%d entries):" RST "\n\n", count);
  if(count == 0) {
    fprintf(f, "  " C_GRY "(no blocked IPs)" RST "\n");
  } else {
    for(i = 0; i < count; i++) {
      fprintf(f, "    " C_LRED "%-15s" RST "\n", ips[i]);
    }
  }
  fprintf(f, "\n  " C_GRY "[U] Clear timed blocks   [S] Return to dashboard" RST "\n");
  fflush(f);
}

static void do_unblock(void) {
  int cleared = blocklist_unblock_all_timed();
  FILE *f = stderr;
  fprintf(f, "\n  " C_LYEL "Cleared %d timed block(s)" RST "\n", cleared);
  fprintf(f, "  " C_GRY "[S] Return to dashboard" RST "\n");
  fflush(f);
}

int dashboard_quit_requested(void) { return g_quit_requested; }

static void ask_confirm(char key, const char *what) {
  g_confirm = key;
  g_confirm_at = time(NULL);
  /* Overwrite the line under the hotkey bar with the prompt. */
  fprintf(stderr, "\r" E "2K" " " BOLD C_LYEL "%s? [y/N]" RST " ", what);
  fflush(stderr);
}

void dashboard_check_keyboard(void) {
  /* Interactive keys only make sense when the terminal dashboard owns the tty
   * in raw mode. In headless (web-only) mode, never read stdin — a blocking
   * read would stall the main loop. */
  if(!g_dash.enabled || !g_term_raw) return;
  char ch;
  while(read(STDIN_FILENO, &ch, 1) == 1) {
    if(g_confirm) {
      /* Destructive keys (hang up a live caller, clear blocks, quit) sit on
       * adjacent single keys and used to fire instantly; 'q' even exit(0)'d
       * past the graceful shutdown (no NO CARRIER, stats not saved). */
      char pending = g_confirm;
      g_confirm = 0;
      if(ch == 'y' || ch == 'Y') {
        switch(pending) {
          case 'h': do_hangup(); break;
          case 'u': do_unblock(); continue;   /* leaves its own overlay text */
          case 'q': g_quit_requested = 1; return;
        }
      }
      g_dash_paused = 0;
      dashboard_render();
      continue;
    }
    switch(ch) {
      case 'h': case 'H': ask_confirm('h', "Hang up the current caller"); break;
      case 'r': case 'R': do_reset(); break;
      case 's': case 'S': g_dash_paused = 0; dashboard_render(); break;
      case 'b': case 'B': do_show_blocklist(); break;
      case 'u': case 'U': ask_confirm('u', "Clear all timed blocks"); break;
      case 'q': case 'Q': ask_confirm('q', "Quit tcpser (hangs up callers, saves stats)"); break;
    }
  }
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
