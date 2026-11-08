/*
 * TCPSER Web Dashboard - small HTTP server
 *
 * Security model:
 *   - Binds to 127.0.0.1 by default (override with TCPSER_DASH_BIND).
 *   - Optional shared secret (TCPSER_DASH_TOKEN). Visiting /?token=SECRET once
 *     sets an HttpOnly, SameSite=Strict cookie and redirects to /, so the token
 *     never stays in the address bar, history or proxy logs. API, SSE and
 *     /metrics accept either the cookie or ?token= (for curl / Prometheus).
 *   - Actions (hang up, clear timed blocks) are POSTs that additionally require
 *     the header X-Requested-With: tcpser, which a cross-site form cannot set.
 *   - Static files are embedded at build time (src/web/ -> web_assets.c); the
 *     CSP allows no inline script, no third-party origins, no framing.
 *   - Per-connection read/write timeouts and a bounded handler pool (with a
 *     separate budget for long-lived SSE streams) bound slow/idle clients.
 *   - Everything caller-derived (IPs, traffic bytes) is JSON-escaped here and
 *     rendered with textContent on the client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#include "webserver.h"
#include "web_assets.h"
#include "dashboard.h"
#include "modem_core.h"
#include "bridge.h"
#include "version.h"
#include "debug.h"
#include "secure_http.h"
#include "util.h"

#define MAX_WEB_HANDLERS   16   /* concurrent client handler threads (all kinds) */
#define MAX_SSE_STREAMS     8   /* of which at most this many long-lived SSE streams */
#define WEB_IO_TIMEOUT_S   10   /* per-connection read/write timeout (seconds) */
#define SSE_PERIOD_S        2   /* status push interval */
#define SSE_MAX_S        3600   /* a stream is closed after this; the client reconnects */
#define COOKIE_NAME  "tcpser_dash"

static int g_web_port = 0;
static int g_web_socket = -1;
static pthread_t g_web_thread;
static volatile int g_web_running = 0;
static char g_web_bind[64] = "127.0.0.1";
static char g_web_token[128] = {0};
static char g_web_name[64] = "TCPSER";
/* Live-traffic CONTENT is opt-in (TCPSER_DASH_TRAFFIC=1): the raw bytes can
 * include what callers type into the BBS (even passwords). Without it the feed
 * carries direction + byte counts only. */
static int g_web_traffic = 0;

static int g_web_handlers = 0;
static int g_web_sse = 0;
static pthread_mutex_t g_web_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------------
 * Growable output buffer (replaces the fixed 8 KB strncat builder, which
 * silently truncated into invalid JSON as soon as fields were added).
 * ------------------------------------------------------------------------- */
typedef struct { char *s; size_t len, cap; int err; } jbuf_t;

static void jb_init(jbuf_t *b) { b->cap = 4096; b->len = 0; b->err = 0; b->s = malloc(b->cap); if(!b->s) { b->err = 1; b->cap = 0; } else b->s[0] = 0; }
static void jb_free(jbuf_t *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }
static int jb_reserve(jbuf_t *b, size_t extra) {
  if(b->err) return 0;
  if(b->len + extra + 1 <= b->cap) return 1;
  size_t ncap = b->cap ? b->cap : 4096;
  while(ncap < b->len + extra + 1) ncap *= 2;
  if(ncap > (16u << 20)) { b->err = 1; return 0; }   /* 16 MB sanity cap */
  char *ns = realloc(b->s, ncap);
  if(!ns) { b->err = 1; return 0; }
  b->s = ns; b->cap = ncap;
  return 1;
}
static void jb_raw(jbuf_t *b, const char *s) {
  size_t n = strlen(s);
  if(!jb_reserve(b, n)) return;
  memcpy(b->s + b->len, s, n + 1); b->len += n;
}
static void jb_printf(jbuf_t *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if(n < 0) return;
  if((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
  if(!jb_reserve(b, (size_t)n)) return;
  memcpy(b->s + b->len, tmp, (size_t)n); b->len += (size_t)n; b->s[b->len] = 0;
}
/* JSON string literal with escaping; NULL -> null */
static void jb_qstr(jbuf_t *b, const char *s) {
  if(!s) { jb_raw(b, "null"); return; }
  if(!jb_reserve(b, strlen(s) * 6 + 2)) return;
  b->s[b->len++] = '"';
  for(const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    if(c == '"' || c == '\\') { b->s[b->len++] = '\\'; b->s[b->len++] = (char)c; }
    else if(c < 0x20 || c == 0x7f || c >= 0x80) { b->len += (size_t)sprintf(b->s + b->len, "\\u%04x", c); }
    else b->s[b->len++] = (char)c;
  }
  b->s[b->len++] = '"'; b->s[b->len] = 0;
}
static void jb_b64(jbuf_t *b, const unsigned char *d, int n) {
  static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(!jb_reserve(b, (size_t)((n + 2) / 3) * 4 + 2)) return;
  b->s[b->len++] = '"';
  for(int i = 0; i < n; i += 3) {
    unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    b->s[b->len++] = tb[(v >> 18) & 63]; b->s[b->len++] = tb[(v >> 12) & 63];
    b->s[b->len++] = i + 1 < n ? tb[(v >> 6) & 63] : '=';
    b->s[b->len++] = i + 2 < n ? tb[v & 63] : '=';
  }
  b->s[b->len++] = '"'; b->s[b->len] = 0;
}
/* ISO-8601 with a colon in the offset so every JS Date() parses it. */
static void iso_time(time_t t, char *out, size_t sz) {
  struct tm tmb; char z[8];
  if(t <= 0 || !localtime_r(&t, &tmb)) { snprintf(out, sz, "1970-01-01T00:00:00Z"); return; }
  strftime(out, sz, "%Y-%m-%dT%H:%M:%S", &tmb);
  strftime(z, sizeof(z), "%z", &tmb);                 /* +0100 */
  size_t l = strlen(out);
  if(strlen(z) == 5 && l + 7 < sz) { out[l] = z[0]; out[l+1] = z[1]; out[l+2] = z[2]; out[l+3] = ':'; out[l+4] = z[3]; out[l+5] = z[4]; out[l+6] = 0; }
}

/* ---------------------------------------------------------------------------
 * JSON status document
 * ------------------------------------------------------------------------- */
static const char *result_name(int r) {
  switch(r) { case 0: return "BLOCKED"; case 1: return "GATE_FAIL"; case 2: return "GATE_PASS"; case 3: return "CONNECTED"; default: return "UNKNOWN"; }
}

static void build_status_json(jbuf_t *b) {
  extern dashboard_state_t g_dash;
  char ts[40];
  time_t now = time(NULL);
  block_info_t blocks[64];
  int nblocks = blocklist_list_ex(blocks, 64);     /* takes its own lock; do it before ours */

  dashboard_lock();
  iso_time(now, ts, sizeof(ts));
  jb_raw(b, "{\"version\":\"" VERSION "\",\"name\":"); jb_qstr(b, g_web_name);
  jb_printf(b, ",\"now\":\"%s\",\"uptime_s\":%ld,\"listen_port\":%d,", ts, (long)(now - g_dash.start_time), g_dash.port);

  /* lines */
  jb_raw(b, "\"lines\":[");
  int first = 1;
  for(int i = 0; i < DASH_MAX_LINES; i++) {
    dash_line_t *l = &g_dash.lines[i];
    if(!l->inited) continue;
    if(!first) jb_raw(b, ","); first = 0;
    jb_printf(b, "{\"id\":%d,\"device\":", i); jb_qstr(b, l->device);
    jb_printf(b, ",\"speed\":%d,\"state\":\"%s\",\"peer\":", l->speed, l->connected ? "connected" : "idle");
    jb_qstr(b, l->connected ? l->peer_ip : NULL);
    if(l->connected) { iso_time(l->session_start, ts, sizeof(ts)); jb_printf(b, ",\"since\":\"%s\",\"session_s\":%ld", ts, (long)(now - l->session_start)); }
    else jb_raw(b, ",\"since\":null,\"session_s\":0");
    jb_printf(b, ",\"bytes_in\":%lu,\"bytes_out\":%lu,\"dcd\":%s,\"dtr\":%s}",
              l->bytes_in, l->bytes_out, l->connected ? "true" : "false", l->dtr ? "true" : "false");
  }
  jb_raw(b, "],");

  /* gate (from line 0's config) */
  {
    modem_config *c = (modem_config *)(g_dash.lines[0].inited ? g_dash.lines[0].cfg : g_dash.modem_cfg);
    const char *mode = "off"; int to = 0, fails = 0;
    if(c) { mode = c->gate_mode == 1 ? "backspace" : c->gate_mode == 2 ? "password" : "off"; to = c->gate_timeout; fails = c->gate_max_fails; }
    jb_printf(b, "\"gate\":{\"mode\":\"%s\",\"timeout\":%d,\"fails\":%d},", mode, to, fails);
  }

  /* counters */
  jb_printf(b, "\"stats\":{\"total_connections\":%lu,\"dropped\":%lu,\"blocked\":%lu,\"gate_pass\":%lu,\"gate_fail\":%lu,\"bytes_in\":%lu,\"bytes_out\":%lu},",
            g_dash.total_connections, g_dash.blocked, g_dash.blocked, g_dash.gate_pass, g_dash.gate_fail, g_dash.bytes_in, g_dash.bytes_out);

  /* history, newest first */
  jb_raw(b, "\"history\":[");
  for(int i = 0; i < g_dash.history_count && i < DASH_HISTORY_LINES; i++) {
    int idx = (g_dash.history_head - 1 - i + DASH_HISTORY_LINES) % DASH_HISTORY_LINES;
    dash_history_entry_t *h = &g_dash.history[idx];
    iso_time(h->connect_time, ts, sizeof(ts));
    jb_printf(b, "%s{\"time\":\"%s\",\"ip\":", i ? "," : "", ts); jb_qstr(b, h->ip);
    jb_printf(b, ",\"result\":\"%s\",\"duration\":%d,\"bytes_in\":%d,\"bytes_out\":%d}", result_name(h->result), h->duration, h->bytes_in, h->bytes_out);
  }
  jb_raw(b, "],");

  /* traffic, oldest first so the client appends naturally */
  jb_printf(b, "\"traffic_content\":%s,\"traffic\":[", g_web_traffic ? "true" : "false");
  {
    int n = g_dash.traffic_count < DASH_TRAFFIC_LINES ? g_dash.traffic_count : DASH_TRAFFIC_LINES;
    for(int i = n - 1; i >= 0; i--) {
      int idx = (g_dash.traffic_head - 1 - i + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
      dash_traffic_line_t *t = &g_dash.traffic[idx];
      jb_printf(b, "%s{\"dir\":\"%s\",\"bytes\":%d,\"raw\":", i == n - 1 ? "" : ",", t->type == 16 ? "in" : "out", t->bytes);
      if(g_web_traffic) { int rl = t->rawlen; if(rl > (int)sizeof(t->raw)) rl = (int)sizeof(t->raw); jb_b64(b, t->raw, rl); }
      else jb_raw(b, "null");
      jb_raw(b, "}");
    }
  }
  jb_raw(b, "],");

  /* blocklist */
  jb_raw(b, "\"blocklist\":[");
  for(int i = 0; i < nblocks; i++) {
    jb_printf(b, "%s{\"ip\":", i ? "," : ""); jb_qstr(b, blocks[i].ip);
    if(blocks[i].permanent) jb_raw(b, ",\"kind\":\"permanent\",\"expires\":null}");
    else { iso_time(blocks[i].until, ts, sizeof(ts)); jb_printf(b, ",\"kind\":\"timed\",\"expires\":\"%s\"}", ts); }
  }
  jb_raw(b, "],");

  /* activity series: oldest -> newest, ending at the current bucket */
  {
    long hp = (long)(now / 3600), mp = (long)(now / 60);
    jb_raw(b, "\"series\":{\"hour_calls\":[");
    for(int i = 23; i >= 0; i--) { long p = hp - i; int h = (int)(p % 24); jb_printf(b, "%s%u", i == 23 ? "" : ",", g_dash.hour_stamp[h] == p ? g_dash.hour_calls[h] : 0u); }
    jb_raw(b, "],\"hour_fail\":[");
    for(int i = 23; i >= 0; i--) { long p = hp - i; int h = (int)(p % 24); jb_printf(b, "%s%u", i == 23 ? "" : ",", g_dash.hour_stamp[h] == p ? g_dash.hour_fail[h] : 0u); }
    jb_raw(b, "],\"min_in\":[");
    for(int i = 59; i >= 0; i--) { long p = mp - i; int m = (int)(p % 60); jb_printf(b, "%s%lu", i == 59 ? "" : ",", g_dash.min_stamp[m] == p ? g_dash.min_in[m] : 0ul); }
    jb_raw(b, "],\"min_out\":[");
    for(int i = 59; i >= 0; i--) { long p = mp - i; int m = (int)(p % 60); jb_printf(b, "%s%lu", i == 59 ? "" : ",", g_dash.min_stamp[m] == p ? g_dash.min_out[m] : 0ul); }
    jb_printf(b, "],\"hour_now\":%d,\"min_now\":%d},", (int)(hp % 24), (int)(mp % 60));
  }

  /* events, newest first */
  jb_raw(b, "\"events\":[");
  for(int i = 0; i < g_dash.events_count && i < DASH_EVENTS; i++) {
    int idx = (g_dash.events_head - 1 - i + DASH_EVENTS) % DASH_EVENTS;
    iso_time(g_dash.events[idx].when, ts, sizeof(ts));
    jb_printf(b, "%s{\"time\":\"%s\",\"text\":", i ? "," : "", ts); jb_qstr(b, g_dash.events[idx].text); jb_raw(b, "}");
  }
  jb_raw(b, "]}");
  dashboard_unlock();
}

/* Prometheus/OpenMetrics text exposition */
static void build_metrics(jbuf_t *b) {
  extern dashboard_state_t g_dash;
  dashboard_lock();
  time_t now = time(NULL);
  jb_printf(b,
    "# HELP tcpser_connections_total Total accepted connections.\n# TYPE tcpser_connections_total counter\ntcpser_connections_total %lu\n"
    "# HELP tcpser_dropped_total Connections dropped by blocklist or rate limit.\n# TYPE tcpser_dropped_total counter\ntcpser_dropped_total %lu\n"
    "# HELP tcpser_blocked_total Alias of tcpser_dropped_total (kept for older dashboards).\n# TYPE tcpser_blocked_total counter\ntcpser_blocked_total %lu\n"
    "# HELP tcpser_gate_pass_total Successful gate passes.\n# TYPE tcpser_gate_pass_total counter\ntcpser_gate_pass_total %lu\n"
    "# HELP tcpser_gate_fail_total Failed gate attempts.\n# TYPE tcpser_gate_fail_total counter\ntcpser_gate_fail_total %lu\n"
    "# HELP tcpser_bytes_in_total Bytes received from callers.\n# TYPE tcpser_bytes_in_total counter\ntcpser_bytes_in_total %lu\n"
    "# HELP tcpser_bytes_out_total Bytes sent to callers.\n# TYPE tcpser_bytes_out_total counter\ntcpser_bytes_out_total %lu\n"
    "# HELP tcpser_uptime_seconds Process uptime in seconds.\n# TYPE tcpser_uptime_seconds gauge\ntcpser_uptime_seconds %ld\n"
    "# HELP tcpser_connected Whether a caller is connected (any line).\n# TYPE tcpser_connected gauge\ntcpser_connected %d\n"
    "# HELP tcpser_line_connected Whether a caller is connected, per line.\n# TYPE tcpser_line_connected gauge\n",
    g_dash.total_connections, g_dash.blocked, g_dash.blocked, g_dash.gate_pass, g_dash.gate_fail,
    g_dash.bytes_in, g_dash.bytes_out, (long)(now - g_dash.start_time), g_dash.connected ? 1 : 0);
  for(int i = 0; i < DASH_MAX_LINES; i++)
    if(g_dash.lines[i].inited) jb_printf(b, "tcpser_line_connected{line=\"%d\"} %d\n", i, g_dash.lines[i].connected ? 1 : 0);
  dashboard_unlock();
}

/* ---------------------------------------------------------------------------
 * HTTP plumbing
 * ------------------------------------------------------------------------- */
static int write_all_fd(int fd, const char *p, size_t n) {
  while(n > 0) {
    ssize_t w = write(fd, p, n);
    if(w < 0) { if(errno == EINTR) continue; return -1; }
    p += w; n -= (size_t)w;
  }
  return 0;
}

static const char *status_text(int code) {
  switch(code) {
    case 200: return "OK"; case 204: return "No Content"; case 302: return "Found"; case 304: return "Not Modified";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 503: return "Service Unavailable"; default: return "Error";
  }
}

/* extra may be NULL or a string of complete "Header: value\r\n" lines */
static void send_response(int fd, int code, const char *ctype, const char *extra, const char *body, size_t blen) {
  char hdr[1024];
  int n = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'\r\n"
    "%s\r\n",
    code, status_text(code), ctype ? ctype : "text/plain; charset=utf-8", blen, extra ? extra : "");
  if(n < 0 || n >= (int)sizeof(hdr)) return;
  if(write_all_fd(fd, hdr, (size_t)n) == 0 && body && blen) write_all_fd(fd, body, blen);
}
static void send_text(int fd, int code, const char *msg) { send_response(fd, code, "text/plain; charset=utf-8", NULL, msg, strlen(msg)); }
static void send_json(int fd, int code, jbuf_t *b) {
  if(b->err) { send_text(fd, 503, "out of memory"); return; }
  send_response(fd, code, "application/json; charset=utf-8", "Cache-Control: no-store\r\n", b->s, b->len);
}

/* Find "Name: value" in the raw request (case-insensitive name); copies the
 * value (trimmed, up to CRLF) into out. */
static int header_get(const char *req, const char *name, char *out, size_t outsz) {
  size_t nl = strlen(name);
  const char *p = req;
  while((p = strchr(p, '\n')) != NULL) {
    p++;
    if(strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
      const char *v = p + nl + 1;
      while(*v == ' ' || *v == '\t') v++;
      size_t i = 0;
      while(v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < outsz) { out[i] = v[i]; i++; }
      out[i] = 0;
      return 1;
    }
  }
  return 0;
}
static int cookie_get(const char *cookies, const char *name, char *out, size_t outsz) {
  size_t nl = strlen(name);
  const char *p = cookies;
  while(p && *p) {
    while(*p == ' ' || *p == ';') p++;
    if(strncmp(p, name, nl) == 0 && p[nl] == '=') {
      const char *v = p + nl + 1; size_t i = 0;
      while(v[i] && v[i] != ';' && i + 1 < outsz) { out[i] = v[i]; i++; }
      out[i] = 0; return 1;
    }
    p = strchr(p, ';');
  }
  return 0;
}
static int query_get(const char *query, const char *key, char *out, size_t outsz) {
  size_t klen = strlen(key);
  const char *p = query;
  out[0] = 0;
  while(p && *p) {
    if(strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1; size_t i = 0;
      while(v[i] && v[i] != '&' && i + 1 < outsz) { out[i] = v[i]; i++; }
      out[i] = 0; return 1;
    }
    p = strchr(p, '&');
    if(p) p++;
  }
  return 0;
}

/* 1 if no token is configured, or the query/cookie token matches (constant time). */
static int request_authorized(const char *req, const char *query) {
  char supplied[160], cookies[1024];
  if(g_web_token[0] == 0) return 1;
  if(query && query_get(query, "token", supplied, sizeof(supplied)) && ct_streq(supplied, g_web_token)) return 1;
  if(header_get(req, "Cookie", cookies, sizeof(cookies)) && cookie_get(cookies, COOKIE_NAME, supplied, sizeof(supplied)) && ct_streq(supplied, g_web_token)) return 1;
  return 0;
}

static const web_asset_t *find_asset(const char *path) {
  if(strcmp(path, "/") == 0) path = "/index.html";
  for(int i = 0; i < web_assets_count; i++)
    if(strcmp(web_assets[i].path, path) == 0) return &web_assets[i];
  return NULL;
}

static int sse_slot_take(void) {
  int ok = 0;
  pthread_mutex_lock(&g_web_handlers_mutex);
  if(g_web_sse < MAX_SSE_STREAMS) { g_web_sse++; ok = 1; }
  pthread_mutex_unlock(&g_web_handlers_mutex);
  return ok;
}
static void sse_slot_release(void) { pthread_mutex_lock(&g_web_handlers_mutex); if(g_web_sse > 0) g_web_sse--; pthread_mutex_unlock(&g_web_handlers_mutex); }

static void handle_request(int fd) {
  char buffer[8192];
  http_request_t request;
  ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
  if(n <= 0) { close(fd); return; }
  buffer[n] = 0;

  if(parse_http_request_safe(buffer, (size_t)n, &request) != 0) { send_text(fd, 400, "Bad Request"); close(fd); return; }
  if(!request.headers_complete) { send_text(fd, 400, "Incomplete Request"); close(fd); return; }

  char *query = strchr(request.path, '?');
  if(query) { *query = 0; query++; }
  const char *path = request.path;
  int authed = request_authorized(buffer, query);
  int is_get = strcmp(request.method, "GET") == 0 || strcmp(request.method, "HEAD") == 0;
  int is_post = strcmp(request.method, "POST") == 0;
  LOG(LOG_DEBUG, "Web request: %s %s (%s)", request.method, path, authed ? "auth" : "anon");

  /* ---- sign-in: /?token=SECRET -> cookie + redirect to a clean URL ---- */
  if(is_get && strcmp(path, "/") == 0 && query && strstr(query, "token=") && g_web_token[0]) {
    char supplied[160];
    if(query_get(query, "token", supplied, sizeof(supplied)) && ct_streq(supplied, g_web_token)) {
      char extra[320];
      snprintf(extra, sizeof(extra), "Location: /\r\nSet-Cookie: " COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\nCache-Control: no-store\r\n", g_web_token);
      send_response(fd, 302, "text/plain; charset=utf-8", extra, "", 0);
      close(fd); return;
    }
    send_text(fd, 401, "Unauthorized"); close(fd); return;
  }

  /* ---- static assets (no data inside; the page shows a lock panel if the API says 401) ---- */
  const web_asset_t *a = is_get ? find_asset(path) : NULL;
  if(a) {
    char etag[64], inm[80], extra[200];
    snprintf(etag, sizeof(etag), "\"%s-%lu\"", VERSION, a->len);
    if(header_get(buffer, "If-None-Match", inm, sizeof(inm)) && strcmp(inm, etag) == 0) {
      snprintf(extra, sizeof(extra), "ETag: %s\r\n", etag);
      send_response(fd, 304, a->ctype, extra, "", 0); close(fd); return;
    }
    snprintf(extra, sizeof(extra), "ETag: %s\r\nCache-Control: %s\r\n", etag, strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 ? "no-cache" : "max-age=3600");
    send_response(fd, 200, a->ctype, extra, strcmp(request.method, "HEAD") == 0 ? "" : (const char *)a->data, strcmp(request.method, "HEAD") == 0 ? 0 : (size_t)a->len);
    close(fd); return;
  }

  /* ---- everything below needs auth ---- */
  if(!authed) { send_response(fd, 401, "text/plain; charset=utf-8", "Cache-Control: no-store\r\n", "Unauthorized: open /?token=YOUR_TOKEN once to sign in", 54); close(fd); return; }

  if(is_get && strcmp(path, "/api/status") == 0) {
    jbuf_t b; jb_init(&b); build_status_json(&b); send_json(fd, 200, &b); jb_free(&b); close(fd); return;
  }
  if(is_get && strcmp(path, "/metrics") == 0) {
    jbuf_t b; jb_init(&b); build_metrics(&b);
    if(!b.err) send_response(fd, 200, "text/plain; version=0.0.4; charset=utf-8", "Cache-Control: no-store\r\n", b.s, b.len); else send_text(fd, 503, "oom");
    jb_free(&b); close(fd); return;
  }
  if(is_get && strcmp(path, "/api/events") == 0) {
    if(!sse_slot_take()) { send_text(fd, 503, "Too many live streams"); close(fd); return; }
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nX-Content-Type-Options: nosniff\r\n\r\nretry: 3000\n\n";
    if(write_all_fd(fd, hdr, strlen(hdr)) == 0) {
      for(int t = 0; t < SSE_MAX_S && g_web_running; t += SSE_PERIOD_S) {
        jbuf_t b; jb_init(&b); jb_raw(&b, "event: status\ndata: "); build_status_json(&b); jb_raw(&b, "\n\n");
        int rc = b.err ? -1 : write_all_fd(fd, b.s, b.len);
        jb_free(&b);
        if(rc < 0) break;
        sleep(SSE_PERIOD_S);
      }
    }
    sse_slot_release(); close(fd); return;
  }

  /* ---- actions: POST + X-Requested-With (CSRF guard) ---- */
  if(is_post) {
    char xrw[32];
    if(!header_get(buffer, "X-Requested-With", xrw, sizeof(xrw)) || strcmp(xrw, "tcpser") != 0) { send_text(fd, 403, "Forbidden"); close(fd); return; }
    int line;
    if(sscanf(path, "/api/line/%d/hangup", &line) == 1 && strstr(path, "/hangup")) {
      extern dashboard_state_t g_dash;
      modem_config *c = NULL;
      dashboard_lock();
      if(line >= 0 && line < DASH_MAX_LINES && g_dash.lines[line].inited) c = (modem_config *)g_dash.lines[line].cfg;
      dashboard_unlock();
      if(!c) { send_text(fd, 404, "No such line"); close(fd); return; }
      writePipe(c->mp[1][1], MSG_HANGUP);
      dashboard_note("web: hang up requested on line %d", line);
      send_text(fd, 200, "{\"ok\":true}"); close(fd); return;
    }
    if(strcmp(path, "/api/unblock") == 0) {
      int cleared = blocklist_unblock_all_timed();
      dashboard_note("web: cleared %d timed block(s)", cleared);
      char msg[64]; snprintf(msg, sizeof(msg), "{\"ok\":true,\"cleared\":%d}", cleared);
      send_response(fd, 200, "application/json; charset=utf-8", NULL, msg, strlen(msg)); close(fd); return;
    }
    send_text(fd, 404, "Not Found"); close(fd); return;
  }

  send_text(fd, is_get ? 404 : 405, is_get ? "Not Found" : "Method Not Allowed");
  close(fd);
}

static void *handler_entry(void *arg) {
  int fd = (int)(intptr_t)arg;
  handle_request(fd);
  pthread_mutex_lock(&g_web_handlers_mutex);
  if(g_web_handlers > 0) g_web_handlers--;
  pthread_mutex_unlock(&g_web_handlers_mutex);
  return NULL;
}

static void *webserver_thread(void *arg) {
  (void)arg;
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  LOG(LOG_INFO, "Web dashboard started on %s:%d", g_web_bind, g_web_port);

  while(g_web_running) {
    fd_set readfds; struct timeval tv;
    FD_ZERO(&readfds); FD_SET(g_web_socket, &readfds);
    tv.tv_sec = 1; tv.tv_usec = 0;
    int activity = select(g_web_socket + 1, &readfds, NULL, NULL, &tv);
    if(activity <= 0 || !FD_ISSET(g_web_socket, &readfds)) continue;
    int client_fd = accept(g_web_socket, (struct sockaddr *)&client_addr, &client_len);
    if(client_fd < 0) continue;

    struct timeval to = { WEB_IO_TIMEOUT_S, 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

    pthread_mutex_lock(&g_web_handlers_mutex);
    int busy = (g_web_handlers >= MAX_WEB_HANDLERS);
    if(!busy) g_web_handlers++;
    pthread_mutex_unlock(&g_web_handlers_mutex);
    if(busy) { send_text(client_fd, 503, "Server Busy"); close(client_fd); continue; }

    pthread_t th; pthread_attr_t attr;
    pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th, &attr, handler_entry, (void *)(intptr_t)client_fd) != 0) {
      pthread_mutex_lock(&g_web_handlers_mutex); g_web_handlers--; pthread_mutex_unlock(&g_web_handlers_mutex);
      close(client_fd);
    }
    pthread_attr_destroy(&attr);
  }
  return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void webserver_init(int port) {
  g_web_port = port;
}

/* Called once option parsing is complete, so --web-bind/--web-token given
 * after -H (or in a config file) are seen. */
void webserver_read_env(void) {
  const char *bind_env = getenv("TCPSER_DASH_BIND");
  const char *token_env = getenv("TCPSER_DASH_TOKEN");
  const char *traffic_env = getenv("TCPSER_DASH_TRAFFIC");
  const char *name_env = getenv("TCPSER_DASH_NAME");
  if(bind_env && bind_env[0]) safe_strncpy(g_web_bind, bind_env, sizeof(g_web_bind));
  if(token_env && token_env[0]) {
    /* the token travels in a URL and a cookie: keep it to URL-safe characters */
    for(const char *p = token_env; *p; p++) {
      if(!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
        fprintf(stderr, "  ERROR: TCPSER_DASH_TOKEN may only contain letters, digits, '-', '_' and '.'\n");
        exit(2);
      }
    }
    safe_strncpy(g_web_token, token_env, sizeof(g_web_token));
  }
  if(traffic_env && (traffic_env[0] == '1' || traffic_env[0] == 'y' || traffic_env[0] == 'Y' || traffic_env[0] == 't' || traffic_env[0] == 'T')) g_web_traffic = 1;
  if(name_env && name_env[0]) safe_strncpy(g_web_name, name_env, sizeof(g_web_name));
}

int webserver_start(void) {
  struct sockaddr_in server_addr;
  if(g_web_port <= 0) return -1;

  g_web_socket = socket(AF_INET, SOCK_STREAM, 0);
  if(g_web_socket < 0) { LOG(LOG_ERROR, "Failed to create web server socket"); return -1; }
  int opt = 1;
  setsockopt(g_web_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons((uint16_t)g_web_port);
  if(inet_pton(AF_INET, g_web_bind, &server_addr.sin_addr) != 1) {
    LOG(LOG_WARN, "Invalid dashboard bind address '%s', falling back to 127.0.0.1", g_web_bind);
    safe_strncpy(g_web_bind, "127.0.0.1", sizeof(g_web_bind));
    inet_pton(AF_INET, g_web_bind, &server_addr.sin_addr);
  }
  if(bind(g_web_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    LOG(LOG_ERROR, "Failed to bind web server to %s:%d: %s", g_web_bind, g_web_port, strerror(errno));
    fprintf(stderr, "  ERROR: web dashboard could not bind %s:%d: %s\n", g_web_bind, g_web_port, strerror(errno));
    close(g_web_socket); g_web_socket = -1; return -1;
  }
  if(listen(g_web_socket, 8) < 0) {
    LOG(LOG_ERROR, "Failed to listen on web server port %d", g_web_port);
    fprintf(stderr, "  ERROR: web dashboard could not listen on port %d: %s\n", g_web_port, strerror(errno));
    close(g_web_socket); g_web_socket = -1; return -1;
  }

  int loopback = (strcmp(g_web_bind, "127.0.0.1") == 0 || strcmp(g_web_bind, "localhost") == 0);
  if(!loopback && g_web_token[0] == 0)
    LOG(LOG_WARN, "Web dashboard bound to %s with NO token - caller IPs and stats are exposed to anyone who can reach port %d. Set TCPSER_DASH_TOKEN.", g_web_bind, g_web_port);
  if(g_web_traffic && (!loopback || g_web_token[0] == 0))
    LOG(LOG_WARN, "TCPSER_DASH_TRAFFIC is ON - the web dashboard streams caller session CONTENT. Only expose this behind localhost + a token.");

  g_web_running = 1;
  if(pthread_create(&g_web_thread, NULL, webserver_thread, NULL) != 0) {
    LOG(LOG_ERROR, "Failed to start web server thread");
    close(g_web_socket); g_web_socket = -1; g_web_running = 0; return -1;
  }
  dashboard_note("web dashboard listening on %s:%d%s", g_web_bind, g_web_port, g_web_token[0] ? " (token)" : "");
  return 0;
}

void webserver_stop(void) {
  if(g_web_running) {
    g_web_running = 0;
    pthread_join(g_web_thread, NULL);
    close(g_web_socket);
    g_web_socket = -1;
  }
}

int webserver_is_running(void) { return g_web_running; }
int webserver_get_port(void) { return g_web_port; }
const char *webserver_get_bind(void) { return g_web_bind; }
int webserver_has_token(void) { return g_web_token[0] != 0; }
