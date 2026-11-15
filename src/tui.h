#ifndef TUI_H
#define TUI_H

/*
 * Minimal cell-buffer terminal renderer for the -M dashboard.
 *
 * - Draw a whole frame into a back buffer (tui_begin / tui_put / tui_box ...)
 *   and tui_flush() emits only the cells that changed since the last frame:
 *   no ESC[2J per refresh, no flicker, tiny output on a quiet screen.
 * - Capability tiers: 256 colours, 16 colours, or mono (bold/dim/reverse),
 *   chosen from TERM / COLORTERM / NO_COLOR; UTF-8 box glyphs or ASCII when
 *   the locale is not UTF-8.
 * - Output goes to stderr with O_NONBLOCK: a terminal that stops draining
 *   (frozen SSH) costs us one dropped frame, not a stalled accept loop.
 * - Four palettes like the web console: colour (the 5250 colour display,
 *   default) and the green / amber / white phosphors.
 */

enum {
  TC_DEFAULT = 0,
  TC_FRAME,     /* box lines */
  TC_TITLE,     /* panel title (drawn reverse) */
  TC_LABEL,     /* field labels */
  TC_VALUE,     /* values, white-ish */
  TC_DIM,       /* secondary text */
  TC_OK,
  TC_WARN,
  TC_BAD,
  TC_ACCENT,    /* "turquoise": carrier, caller->BBS */
  TC_ACCENT2,   /* "pink": BBS->caller */
  TC_REV,       /* reverse video in the title colour (panel titles, prompts) */
  TC_HEAD,      /* column headings */
  TC_KEY,       /* hotkey letters, the clock */
  TC_INFO,      /* "blue": secondary values such as timestamps */
  TC_COUNT,
  /* reverse-video panel titles in another role's colour; on the one-colour
   * phosphors they all fall back to TC_REV */
  TC_REV_ACCENT = TC_COUNT, TC_REV_ACCENT2, TC_REV_WARN, TC_REV_BAD, TC_REV_INFO
};

enum { G_HL, G_VL, G_TL, G_TR, G_BL, G_BR, G_LT, G_RT, G_DOT_ON, G_DOT_OFF, G_ARROW_R, G_ARROW_L, G_COUNT };

int  tui_init(void);                      /* detect caps, take stderr non-blocking, hide cursor */
void tui_shutdown(void);                  /* restore stderr flags, show cursor, reset attributes */
int  tui_active(void);
void tui_set_phosphor(const char *name);  /* "color" | "green" | "amber" | "white" */
const char *tui_phosphor(void);
const char *tui_next_phosphor(void);
int  tui_is_utf8(void);
int  tui_color_tier(void);                /* 256, 16 or 0 */
const char *tui_glyph(int which);

void tui_size(int *w, int *h);            /* terminal size, clamped to the buffer */
void tui_begin(void);                     /* start a frame */
void tui_put(int row, int col, int color, const char *utf8);              /* clipped at the right edge */
void tui_putn(int row, int col, int color, const char *utf8, int maxcols); /* at most maxcols cells */
void tui_fill(int row, int col, int n, int color, const char *glyph);
void tui_hrule(int row, int col, int n);
void tui_box(int top, int left, int h, int w, const char *title);          /* frame with " TITLE " inlaid reverse */
void tui_box_c(int top, int left, int h, int w, const char *title, int title_color);  /* same, title in TC_REV_* */
int  tui_flush(void);                     /* 0 = frame written, -1 = dropped (terminal not draining) */
void tui_invalidate(void);                /* force a full repaint on the next flush */

#endif
