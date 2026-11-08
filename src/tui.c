/* Cell-buffer terminal renderer. See tui.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "tui.h"

#define MAXW 240
#define MAXH 100
#define CELLB 5                 /* up to 4 UTF-8 bytes + NUL */

typedef struct { char ch[CELLB]; unsigned char color; } cell_t;

static cell_t g_back[MAXH][MAXW];
static cell_t g_front[MAXH][MAXW];
static int g_w = 80, g_h = 24;
static int g_active = 0;
static int g_utf8 = 1;
static int g_tier = 256;
static int g_full = 1;           /* next flush repaints everything */
static int g_saved_flags = -1;
static char g_phos[8] = "green";

/* ---- output buffer (one write per frame) ---------------------------------- */
static char g_out[MAXW * MAXH * 16];
static size_t g_outlen = 0;
static void outs(const char *s) { size_t n = strlen(s); if(g_outlen + n < sizeof(g_out)) { memcpy(g_out + g_outlen, s, n); g_outlen += n; } }
static void outf(const char *fmt, int a, int b) { char t[32]; snprintf(t, sizeof(t), fmt, a, b); outs(t); }

/* ---- glyphs ------------------------------------------------------------ */
static const char *GL_UTF8[G_COUNT] = { "\xe2\x94\x80", "\xe2\x94\x82", "\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98",
                                        "\xe2\x94\x9c", "\xe2\x94\xa4", "\xe2\x97\x8f", "\xe2\x97\x8b", "\xe2\x86\x92", "\xe2\x86\x90" };
static const char *GL_ASCII[G_COUNT] = { "-", "|", "+", "+", "+", "+", "+", "+", "*", "o", ">", "<" };
const char *tui_glyph(int which) { if(which < 0 || which >= G_COUNT) return "?"; return g_utf8 ? GL_UTF8[which] : GL_ASCII[which]; }

/* ---- palettes ---------------------------------------------------------- */
/* 256-colour index per role, per phosphor */
static const int PAL256[3][TC_COUNT] = {
  /* green */ { 0, 28, 46, 40, 231, 28, 46, 220, 203, 51, 213, 46 },
  /* amber */ { 0, 130, 214, 214, 230, 94, 214, 226, 203, 222, 217, 214 },
  /* white */ { 0, 245, 252, 252, 231, 240, 120, 220, 203, 117, 218, 252 },
};
/* 16-colour SGR codes per role, per phosphor (30-37 normal, 90-97 bright) */
static const int PAL16[3][TC_COUNT] = {
  { 39, 32, 92, 92, 97, 32, 92, 93, 91, 96, 95, 92 },
  { 39, 33, 93, 93, 97, 33, 93, 93, 91, 93, 95, 93 },
  { 39, 37, 97, 97, 97, 90, 92, 93, 91, 96, 95, 97 },
};
static int phos_idx(void) { return strcmp(g_phos, "amber") == 0 ? 1 : strcmp(g_phos, "white") == 0 ? 2 : 0; }

void tui_set_phosphor(const char *name) {
  if(!name) return;
  if(strcmp(name, "amber") == 0 || strcmp(name, "white") == 0 || strcmp(name, "green") == 0) { snprintf(g_phos, sizeof(g_phos), "%s", name); g_full = 1; }
}
const char *tui_phosphor(void) { return g_phos; }
const char *tui_next_phosphor(void) { int i = (phos_idx() + 1) % 3; tui_set_phosphor(i == 0 ? "green" : i == 1 ? "amber" : "white"); return g_phos; }
int tui_is_utf8(void) { return g_utf8; }
int tui_color_tier(void) { return g_tier; }
int tui_active(void) { return g_active; }

static void sgr_for(int color) {
  /* reset first so attributes never leak between cells */
  outs("\033[0m");
  if(color == TC_DEFAULT) return;
  int p = phos_idx();
  if(g_tier == 256) {
    if(color == TC_REV) { outf("\033[7;38;5;%dm", PAL256[p][TC_TITLE], 0); return; }
    outf("\033[38;5;%dm", PAL256[p][color], 0);
    if(color == TC_VALUE || color == TC_TITLE) outs("\033[1m");
  } else if(g_tier == 16) {
    if(color == TC_REV) { outf("\033[7;%dm", PAL16[p][TC_TITLE], 0); return; }
    outf("\033[%dm", PAL16[p][color], 0);
    if(color == TC_VALUE || color == TC_TITLE) outs("\033[1m");
  } else {
    switch(color) {
      case TC_REV: outs("\033[7m"); break;
      case TC_VALUE: case TC_TITLE: case TC_OK: case TC_ACCENT: outs("\033[1m"); break;
      case TC_DIM: case TC_FRAME: outs("\033[2m"); break;
      case TC_BAD: case TC_WARN: outs("\033[1;4m"); break;
      default: break;
    }
  }
}

/* ---- lifecycle --------------------------------------------------------- */
int tui_init(void) {
  const char *term = getenv("TERM");
  const char *cterm = getenv("COLORTERM");
  const char *lang = getenv("LC_ALL"); if(!lang || !*lang) lang = getenv("LC_CTYPE"); if(!lang || !*lang) lang = getenv("LANG");
  const char *phos = getenv("TCPSER_TUI_PHOSPHOR");

  g_utf8 = lang && (strstr(lang, "UTF-8") || strstr(lang, "utf8") || strstr(lang, "UTF8") || strstr(lang, "utf-8"));
  if(getenv("TCPSER_TUI_ASCII")) g_utf8 = 0;
  if(getenv("NO_COLOR") || (term && strcmp(term, "dumb") == 0)) g_tier = 0;
  else if((term && (strstr(term, "256color") || strstr(term, "truecolor") || strstr(term, "direct"))) || (cterm && (strstr(cterm, "truecolor") || strstr(cterm, "24bit")))) g_tier = 256;
  else g_tier = 16;
  if(phos) tui_set_phosphor(phos);

  if(g_saved_flags < 0) {
    g_saved_flags = fcntl(STDERR_FILENO, F_GETFL, 0);
    if(g_saved_flags >= 0) fcntl(STDERR_FILENO, F_SETFL, g_saved_flags | O_NONBLOCK);
  }
  memset(g_front, 0, sizeof(g_front));
  g_full = 1;
  g_active = 1;
  return 0;
}

void tui_shutdown(void) {
  if(!g_active) return;
  g_active = 0;
  const char *s = "\033[0m\033[?25h\033[999;1H\n";
  if(g_saved_flags >= 0) fcntl(STDERR_FILENO, F_SETFL, g_saved_flags);
  if(write(STDERR_FILENO, s, strlen(s)) < 0) { /* best effort */ }
}

void tui_size(int *w, int *h) {
  struct winsize ws;
  int cw = 80, ch = 24;
  if(ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) { if(ws.ws_col > 0) cw = ws.ws_col; if(ws.ws_row > 0) ch = ws.ws_row; }
  if(cw > MAXW) cw = MAXW; if(ch > MAXH) ch = MAXH;
  if(cw < 20) cw = 20; if(ch < 5) ch = 5;
  if(cw != g_w || ch != g_h) g_full = 1;
  g_w = cw; g_h = ch;
  *w = cw; *h = ch;
}

void tui_invalidate(void) { g_full = 1; }

/* ---- drawing ----------------------------------------------------------- */
void tui_begin(void) {
  for(int r = 0; r < g_h; r++) for(int c = 0; c < g_w; c++) { g_back[r][c].ch[0] = ' '; g_back[r][c].ch[1] = 0; g_back[r][c].color = TC_DEFAULT; }
}

static int utf8_len(unsigned char b) { return b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1; }

void tui_putn(int row, int col, int color, const char *s, int maxcols) {
  if(row < 0 || row >= g_h || !s) return;
  int c = col, n = 0;
  while(*s && n < maxcols) {
    int l = utf8_len((unsigned char)*s);
    if(c >= 0 && c < g_w) {
      cell_t *cell = &g_back[row][c];
      if(l > 4) l = 1;
      memcpy(cell->ch, s, (size_t)l); cell->ch[l] = 0;
      if(!g_utf8 && l > 1) { cell->ch[0] = '?'; cell->ch[1] = 0; }   /* never emit UTF-8 on a non-UTF-8 terminal */
      cell->color = (unsigned char)color;
    }
    s += l; c++; n++;
  }
}
void tui_put(int row, int col, int color, const char *s) { tui_putn(row, col, color, s, MAXW); }

void tui_fill(int row, int col, int n, int color, const char *glyph) {
  for(int i = 0; i < n; i++) tui_putn(row, col + i, color, glyph, 1);
}
void tui_hrule(int row, int col, int n) { tui_fill(row, col, n, TC_FRAME, tui_glyph(G_HL)); }

void tui_box(int top, int left, int h, int w, const char *title) {
  if(h < 2 || w < 2) return;
  tui_putn(top, left, TC_FRAME, tui_glyph(G_TL), 1);
  tui_hrule(top, left + 1, w - 2);
  tui_putn(top, left + w - 1, TC_FRAME, tui_glyph(G_TR), 1);
  for(int r = 1; r < h - 1; r++) { tui_putn(top + r, left, TC_FRAME, tui_glyph(G_VL), 1); tui_putn(top + r, left + w - 1, TC_FRAME, tui_glyph(G_VL), 1); }
  tui_putn(top + h - 1, left, TC_FRAME, tui_glyph(G_BL), 1);
  tui_hrule(top + h - 1, left + 1, w - 2);
  tui_putn(top + h - 1, left + w - 1, TC_FRAME, tui_glyph(G_BR), 1);
  if(title && *title) {
    char t[80]; snprintf(t, sizeof(t), " %s ", title);
    tui_putn(top, left + 2, TC_REV, t, w - 4);
  }
}

/* ---- flush: diff against the front buffer ----------------------------- */
/* Output that the terminal did not accept yet is kept and sent first next
 * time, so a slow link delays the picture instead of corrupting it. Only when
 * the backlog exceeds PEND_MAX (a terminal that stopped draining for good) is
 * it discarded and a full repaint scheduled. The main loop never blocks. */
#define PEND_MAX (512 * 1024)
static char g_pend[PEND_MAX];
static size_t g_pendlen = 0;

static int write_frame(void) {
  /* queue the new frame behind whatever is still pending */
  if(g_pendlen + g_outlen > PEND_MAX) { g_pendlen = 0; return -1; }
  memcpy(g_pend + g_pendlen, g_out, g_outlen); g_pendlen += g_outlen;
  size_t off = 0; int polls = 0;
  while(off < g_pendlen) {
    ssize_t n = write(STDERR_FILENO, g_pend + off, g_pendlen - off);
    if(n > 0) { off += (size_t)n; continue; }
    if(n < 0 && errno == EINTR) continue;
    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if(polls++ >= 2) break;                       /* ~50 ms budget, then leave the rest pending */
      struct pollfd p = { STDERR_FILENO, POLLOUT, 0 };
      poll(&p, 1, 25);
      continue;
    }
    g_pendlen = 0; return -1;                       /* real error: drop everything */
  }
  if(off < g_pendlen) memmove(g_pend, g_pend + off, g_pendlen - off);
  g_pendlen -= off;
  return 0;
}

int tui_flush(void) {
  if(!g_active) return -1;
  g_outlen = 0;
  int cur_color = -1, cur_r = -1, cur_c = -1;
  if(g_full) { outs("\033[0m\033[2J\033[H\033[?25l"); }
  for(int r = 0; r < g_h; r++) {
    for(int c = 0; c < g_w; c++) {
      cell_t *b = &g_back[r][c], *f = &g_front[r][c];
      if(!g_full && b->color == f->color && strcmp(b->ch, f->ch) == 0) continue;
      if(cur_r != r || cur_c != c) { outf("\033[%d;%dH", r + 1, c + 1); cur_r = r; cur_c = c; }
      if(cur_color != b->color) { sgr_for(b->color); cur_color = b->color; }
      outs(b->ch);
      cur_c++;
    }
  }
  outs("\033[0m");
  outf("\033[%d;%dH", g_h, g_w);   /* park the cursor out of the way */
  int rc = write_frame();
  if(rc == 0) {
    memcpy(g_front, g_back, sizeof(g_front));   /* queued output WILL reach the screen in order */
    g_full = 0;
  } else {
    g_full = 1;                           /* backlog discarded: we don't know what is on screen */
  }
  return rc;
}
