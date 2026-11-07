#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

/* Static files for the web dashboard, embedded at build time by
 * tools/embed.sh from src/web/ into the generated src/web_assets.c. */
typedef struct {
  const char *path;            /* "/index.html", "/app.js", ... */
  const char *ctype;           /* Content-Type header value */
  const unsigned char *data;   /* file bytes (NUL-terminated as well) */
  unsigned long len;           /* file length in bytes */
} web_asset_t;

extern const web_asset_t web_assets[];
extern const int web_assets_count;

#endif
