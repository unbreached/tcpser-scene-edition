/*
 * TCPSER Web Dashboard - Simple HTTP Server
 *
 * Security model:
 *   - Binds to 127.0.0.1 by default (override with TCPSER_DASH_BIND).
 *   - Optional shared-secret auth via TCPSER_DASH_TOKEN (?token=... on requests).
 *   - Per-connection read/write timeouts and a bounded handler-thread pool so a
 *     slow/idle client cannot hang or exhaust the server (slowloris).
 *   - Exposes read-only status plus a Prometheus /metrics endpoint.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#include "webserver.h"
#include "dashboard.h"
#include "version.h"
#include "debug.h"
#include "secure_http.h"
#include "util.h"

#define MAX_WEB_HANDLERS  16   /* max concurrent client handler threads */
#define WEB_IO_TIMEOUT_S  10   /* per-connection read/write timeout (seconds) */

static int g_web_port = 0;
static int g_web_socket = -1;
static pthread_t g_web_thread;
static volatile int g_web_running = 0;
static char g_web_bind[64] = "127.0.0.1";
static char g_web_token[128] = {0};
/* Live-traffic CONTENT preview is opt-in (TCPSER_DASH_TRAFFIC=1): the raw
 * bytes can include what callers type into the BBS (even passwords), so by
 * default the feed shows only direction + byte counts + timing, never content. */
static int g_web_traffic = 0;

/* Build a JSON-safe, printable ASCII preview of raw traffic bytes. High bit is
 * stripped (PETSCII), control/non-printable become '.', and '"'/'\\' escaped. */
static void json_ascii_preview(const unsigned char *raw, int rawlen, char *out, size_t outsz) {
  size_t o = 0;
  for(int i = 0; i < rawlen && o + 2 < outsz; i++) {
    unsigned char c = (unsigned char)(raw[i] & 0x7f);
    if(c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if(c >= 0x20 && c < 0x7f) out[o++] = (char)c;
    else out[o++] = '.';
  }
  out[o] = 0;
}

static int g_web_handlers = 0;
static pthread_mutex_t g_web_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;

static void send_sse_header(int fd) {
  const char *header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";
  ssize_t w = write(fd, header, strlen(header));
  (void)w;
}

/* Dashboard HTML page. The client reads an optional ?token=... from its own URL
 * and forwards it to the API/SSE endpoints, so a token-protected dashboard is
 * reached simply by visiting http://host:port/?token=SECRET */
static const char *dashboard_html =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
"<title>TCPSER Dashboard</title>\n"
"<style>\n"
"* { margin: 0; padding: 0; box-sizing: border-box; }\n"
":root { --cy: #29f6ff; --mg: #ff2bd6; --gn: #26ff8f; }\n"
"body { font: 14px 'Courier New', monospace; color: #8be9fd;\n"
"  background: #05060a; padding: 14px; min-height: 100vh;\n"
"  background-image:\n"
"    linear-gradient(rgba(41,246,255,0.04) 1px, transparent 1px),\n"
"    linear-gradient(90deg, rgba(255,43,214,0.04) 1px, transparent 1px);\n"
"  background-size: 100% 3px, 42px 100%; }\n"
".header { text-align: center; margin-bottom: 22px; }\n"
".header h1 { color: var(--cy); font-size: 19px; letter-spacing: 2px;\n"
"  text-shadow: 0 0 6px var(--cy), 0 0 16px var(--cy), 0 0 2px #fff; }\n"
"#status { color: var(--mg); text-shadow: 0 0 6px var(--mg); margin-top: 6px; }\n"
".section { border: 1px solid #103a3f; border-radius: 4px; margin: 12px 0;\n"
"  padding: 12px; background: rgba(41,246,255,0.02);\n"
"  box-shadow: 0 0 10px rgba(41,246,255,0.12), inset 0 0 14px rgba(41,246,255,0.03); }\n"
".section h3 { color: var(--mg); margin-bottom: 12px; font-size: 13px;\n"
"  text-transform: uppercase; letter-spacing: 2px; text-shadow: 0 0 6px var(--mg);\n"
"  border-left: 3px solid var(--mg); padding-left: 8px; }\n"
".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 15px; }\n"
".stats { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }\n"
".stat { padding: 10px 5px; border: 1px solid #0c3a3a; border-radius: 3px;\n"
"  text-align: center; background: rgba(0,0,0,0.4); }\n"
".stat .label { color: #4fb3a3; font-size: 11px; text-transform: uppercase; letter-spacing: 1px; }\n"
".stat .value { color: var(--cy); font-weight: bold; font-size: 18px; text-shadow: 0 0 6px var(--cy); }\n"
".history { max-height: 220px; overflow-y: auto; }\n"
".history-item { padding: 4px 2px; border-bottom: 1px solid rgba(41,246,255,0.08); font-size: 13px; }\n"
".connected { color: var(--gn); text-shadow: 0 0 5px var(--gn); }\n"
".blocked { color: #ff2d5e; text-shadow: 0 0 6px #ff2d5e; }\n"
".gate-fail { color: #ffb020; text-shadow: 0 0 6px #ffb020; }\n"
".feed { max-height: 320px; overflow-y: auto; font-size: 12px; line-height: 1.6; }\n"
".feed-row { display: flex; gap: 10px; align-items: baseline; padding: 3px 0;\n"
"  border-bottom: 1px solid rgba(41,246,255,0.06); white-space: nowrap; }\n"
".chip { flex: 0 0 auto; padding: 0 6px; border-radius: 2px; font-size: 10px; letter-spacing: 1px; }\n"
".chip.in { color: var(--cy); border: 1px solid rgba(41,246,255,0.5); text-shadow: 0 0 5px var(--cy); }\n"
".chip.out { color: var(--mg); border: 1px solid rgba(255,43,214,0.5); text-shadow: 0 0 5px var(--mg); }\n"
".bytes { flex: 0 0 auto; color: #4fb3a3; font-variant-numeric: tabular-nums; }\n"
".txt { flex: 1 1 auto; overflow: hidden; text-overflow: ellipsis; color: #8be9fd; opacity: 0.85; }\n"
".muted { color: #4a6b78; font-size: 11px; padding: 6px 2px; }\n"
"::-webkit-scrollbar { width: 8px; }\n"
"::-webkit-scrollbar-thumb { background: rgba(41,246,255,0.25); border-radius: 4px; }\n"
"@media (max-width: 768px) {\n"
"  .grid { grid-template-columns: 1fr; }\n"
"  .stats { grid-template-columns: 1fr; }\n"
"}\n"
"</style>\n"
"</head><body>\n"
"<div class='header'>\n"
"  <h1>TCPSER " VERSION " - " EDITION "</h1>\n"
"  <div id='status'>Loading...</div>\n"
"</div>\n"
"<div class='grid'>\n"
"  <div class='section'>\n"
"    <h3>Statistics</h3>\n"
"    <div class='stats' id='stats'>\n"
"      <div class='stat'><div class='label'>Connections</div><div class='value' id='conn'>-</div></div>\n"
"      <div class='stat'><div class='label'>Blocked</div><div class='value' id='blocked'>-</div></div>\n"
"      <div class='stat'><div class='label'>Gate Pass</div><div class='value' id='pass'>-</div></div>\n"
"      <div class='stat'><div class='label'>Gate Fail</div><div class='value' id='fail'>-</div></div>\n"
"      <div class='stat'><div class='label'>Bytes In</div><div class='value' id='bytes_in'>-</div></div>\n"
"      <div class='stat'><div class='label'>Bytes Out</div><div class='value' id='bytes_out'>-</div></div>\n"
"    </div>\n"
"  </div>\n"
"  <div class='section'>\n"
"    <h3>Connection History</h3>\n"
"    <div class='history' id='history'>Loading...</div>\n"
"  </div>\n"
"</div>\n"
"<div class='section'>\n"
"  <h3>Live Traffic</h3>\n"
"  <div class='feed' id='feed'>Waiting for traffic...</div>\n"
"</div>\n"
"<script>\n"
"const TOKEN = new URLSearchParams(location.search).get('token') || '';\n"
"const Q = TOKEN ? ('?token=' + encodeURIComponent(TOKEN)) : '';\n"
"let eventSource = new EventSource('/api/events' + Q);\n"
"eventSource.onmessage = function(event) {\n"
"  const data = JSON.parse(event.data);\n"
"  updateDashboard(data);\n"
"};\n"
"function updateDashboard(data) {\n"
"  document.getElementById('status').textContent = 'Connected - ' + data.uptime;\n"
"  document.getElementById('conn').textContent = data.stats.total_connections;\n"
"  document.getElementById('blocked').textContent = data.stats.blocked;\n"
"  document.getElementById('pass').textContent = data.stats.gate_pass;\n"
"  document.getElementById('fail').textContent = data.stats.gate_fail;\n"
"  document.getElementById('bytes_in').textContent = formatBytes(data.stats.bytes_in);\n"
"  document.getElementById('bytes_out').textContent = formatBytes(data.stats.bytes_out);\n"
"  const history = document.getElementById('history');\n"
"  history.innerHTML = '';\n"
"  data.history.forEach(item => {\n"
"    const div = document.createElement('div');\n"
"    div.className = 'history-item ' + getResultClass(item.result);\n"
"    div.textContent = item.time + ' ' + item.ip + ' ' + getResultText(item.result) + ' (' + item.duration + 's)';\n"
"    history.appendChild(div);\n"
"  });\n"
"  renderFeed(data);\n"
"}\n"
"function renderFeed(data) {\n"
"  const feed = document.getElementById('feed');\n"
"  if (!data.traffic || data.traffic.length === 0) { feed.innerHTML = '<div class=\"muted\">No traffic yet.</div>'; return; }\n"
"  feed.innerHTML = '';\n"
"  if (!data.traffic_content) {\n"
"    const m = document.createElement('div'); m.className = 'muted';\n"
"    m.textContent = 'Content hidden — start tcpser with TCPSER_DASH_TRAFFIC=1 to show payloads (localhost + token only).';\n"
"    feed.appendChild(m);\n"
"  }\n"
"  data.traffic.forEach(t => {\n"
"    const row = document.createElement('div'); row.className = 'feed-row';\n"
"    const chip = document.createElement('span'); chip.className = 'chip ' + t.dir;\n"
"    chip.textContent = (t.dir === 'in') ? 'CALLER->BBS' : 'BBS->CALLER';\n"
"    const b = document.createElement('span'); b.className = 'bytes'; b.textContent = t.bytes + 'B';\n"
"    const x = document.createElement('span'); x.className = 'txt'; x.textContent = t.text || '';\n"
"    row.appendChild(chip); row.appendChild(b); row.appendChild(x); feed.appendChild(row);\n"
"  });\n"
"}\n"
"function formatBytes(bytes) {\n"
"  if (bytes < 1024) return bytes + 'B';\n"
"  if (bytes < 1048576) return Math.round(bytes/1024) + 'KB';\n"
"  return Math.round(bytes/1048576) + 'MB';\n"
"}\n"
"function getResultClass(result) {\n"
"  if (result == 3) return 'connected';\n"
"  if (result == 0) return 'blocked';\n"
"  return 'gate-fail';\n"
"}\n"
"function getResultText(result) {\n"
"  const texts = ['BLOCKED', 'GATE-FAIL', 'GATE-PASS', 'CONNECTED'];\n"
"  return texts[result] || 'UNKNOWN';\n"
"}\n"
"eventSource.onerror = function() {\n"
"  document.getElementById('status').textContent = 'Connection lost';\n"
"};\n"
"</script>\n"
"</body></html>";

/* Generate JSON status */
static void get_status_json(char *buffer, size_t size) {
  extern dashboard_state_t g_dash;
  dashboard_lock();
  time_t now = time(NULL);
  long uptime = (long)(now - g_dash.start_time);

  snprintf(buffer, size,
    "{\n"
    "  \"uptime\": \"%ldd %ldh %ldm\",\n"
    "  \"stats\": {\n"
    "    \"total_connections\": %lu,\n"
    "    \"blocked\": %lu,\n"
    "    \"gate_pass\": %lu,\n"
    "    \"gate_fail\": %lu,\n"
    "    \"bytes_in\": %lu,\n"
    "    \"bytes_out\": %lu\n"
    "  },\n"
    "  \"history\": [\n",
    uptime / 86400, (uptime % 86400) / 3600, (uptime % 3600) / 60,
    g_dash.total_connections, g_dash.blocked, g_dash.gate_pass,
    g_dash.gate_fail, g_dash.bytes_in, g_dash.bytes_out
  );

  /* Add history entries */
  for(int i = 0; i < g_dash.history_count && i < 10; i++) {
    int idx = (g_dash.history_head - 1 - i + 12) % 12;
    char time_str[32];
    struct tm tmbuf;
    struct tm *tmv = localtime_r(&g_dash.history[idx].connect_time, &tmbuf);
    if(tmv) strftime(time_str, sizeof(time_str), "%H:%M:%S", tmv);
    else    snprintf(time_str, sizeof(time_str), "??:??:??");

    char entry[256];
    snprintf(entry, sizeof(entry),
      "%s    {\"time\": \"%s\", \"ip\": \"%s\", \"result\": %d, \"duration\": %d}",
      i > 0 ? ",\n" : "",
      time_str, g_dash.history[idx].ip, g_dash.history[idx].result, g_dash.history[idx].duration
    );
    strncat(buffer, entry, size - strlen(buffer) - 1);
  }

  /* Live traffic feed: newest CALLER<->BBS lines. Direction + byte count always;
   * ASCII content preview only when explicitly opted in (TCPSER_DASH_TRAFFIC). */
  strncat(buffer, "\n  ],\n  \"traffic_content\": ", size - strlen(buffer) - 1);
  strncat(buffer, g_web_traffic ? "true" : "false", size - strlen(buffer) - 1);
  strncat(buffer, ",\n  \"traffic\": [\n", size - strlen(buffer) - 1);
  {
    int shown = 0;
    for(int i = 0; i < g_dash.traffic_count && i < 12; i++) {
      int idx = (g_dash.traffic_head - 1 - i + DASH_TRAFFIC_LINES) % DASH_TRAFFIC_LINES;
      dash_traffic_line_t *t = &g_dash.traffic[idx];
      char prev[160]; prev[0] = 0;
      if(g_web_traffic) {
        int rl = t->rawlen;
        if(rl > (int)sizeof(t->raw)) rl = (int)sizeof(t->raw);
        if(rl > 64) rl = 64;   /* cap preview length to bound response size */
        json_ascii_preview(t->raw, rl, prev, sizeof(prev));
      }
      char entry[260];
      snprintf(entry, sizeof(entry),
        "%s    {\"dir\": \"%s\", \"bytes\": %d, \"text\": \"%s\"}",
        shown > 0 ? ",\n" : "",
        t->type == 16 ? "in" : "out", t->bytes, prev);
      strncat(buffer, entry, size - strlen(buffer) - 1);
      shown++;
    }
  }
  strncat(buffer, "\n  ]\n}", size - strlen(buffer) - 1);
  dashboard_unlock();
}

/* Generate Prometheus/OpenMetrics text exposition */
static void get_metrics_text(char *buffer, size_t size) {
  extern dashboard_state_t g_dash;
  dashboard_lock();
  time_t now = time(NULL);
  long uptime = (long)(now - g_dash.start_time);

  snprintf(buffer, size,
    "# HELP tcpser_connections_total Total accepted connections.\n"
    "# TYPE tcpser_connections_total counter\n"
    "tcpser_connections_total %lu\n"
    "# HELP tcpser_blocked_total Connections dropped by blocklist/rate limit.\n"
    "# TYPE tcpser_blocked_total counter\n"
    "tcpser_blocked_total %lu\n"
    "# HELP tcpser_gate_pass_total Successful gate passes.\n"
    "# TYPE tcpser_gate_pass_total counter\n"
    "tcpser_gate_pass_total %lu\n"
    "# HELP tcpser_gate_fail_total Failed gate attempts.\n"
    "# TYPE tcpser_gate_fail_total counter\n"
    "tcpser_gate_fail_total %lu\n"
    "# HELP tcpser_bytes_in_total Bytes received from callers.\n"
    "# TYPE tcpser_bytes_in_total counter\n"
    "tcpser_bytes_in_total %lu\n"
    "# HELP tcpser_bytes_out_total Bytes sent to callers.\n"
    "# TYPE tcpser_bytes_out_total counter\n"
    "tcpser_bytes_out_total %lu\n"
    "# HELP tcpser_uptime_seconds Process uptime in seconds.\n"
    "# TYPE tcpser_uptime_seconds gauge\n"
    "tcpser_uptime_seconds %ld\n"
    "# HELP tcpser_connected Whether a caller is currently connected.\n"
    "# TYPE tcpser_connected gauge\n"
    "tcpser_connected %d\n",
    g_dash.total_connections, g_dash.blocked, g_dash.gate_pass,
    g_dash.gate_fail, g_dash.bytes_in, g_dash.bytes_out,
    uptime, g_dash.connected ? 1 : 0
  );
  dashboard_unlock();
}

/* Extract a query-string parameter value (e.g. token) into out. */
static int query_get(const char *query, const char *key, char *out, size_t outsz) {
  size_t klen = strlen(key);
  const char *p = query;
  out[0] = 0;
  while(p && *p) {
    if(strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      size_t i = 0;
      while(v[i] && v[i] != '&' && i + 1 < outsz) { out[i] = v[i]; i++; }
      out[i] = 0;
      return 1;
    }
    p = strchr(p, '&');
    if(p) p++;
  }
  return 0;
}

/* Returns 1 if the request is authorized (no token configured, or token matches). */
static int request_authorized(const char *query) {
  char supplied[128];
  if(g_web_token[0] == 0) return 1;            /* auth disabled */
  if(!query) return 0;
  if(!query_get(query, "token", supplied, sizeof(supplied))) return 0;
  return ct_streq(supplied, g_web_token);
}

/* Handle HTTP request securely */
static void handle_request(int fd) {
  char buffer[8192];
  char response[8192];   /* status JSON now includes the live-traffic feed */
  http_request_t request;
  ssize_t n;

  n = read(fd, buffer, sizeof(buffer) - 1);
  if(n <= 0) {
    close(fd);
    return;
  }
  buffer[n] = 0;

  if(parse_http_request_safe(buffer, n, &request) != 0) {
    send_http_response_safe(fd, HTTP_BAD_REQUEST, "text/plain", "Bad Request", 11);
    close(fd);
    return;
  }
  if(!request.headers_complete) {
    send_http_response_safe(fd, HTTP_BAD_REQUEST, "text/plain", "Incomplete Request", 18);
    close(fd);
    return;
  }

  /* Split path and query string */
  char *query = strchr(request.path, '?');
  if(query) { *query = 0; query++; }

  LOG(LOG_DEBUG, "Web request: %s %s", request.method, request.path);

  /* Authentication gate (constant-time token check) */
  if(!request_authorized(query)) {
    send_http_response_safe(fd, 401, "text/plain", "Unauthorized", 12);
    close(fd);
    return;
  }

  if(strcmp(request.method, "GET") == 0) {
    if(strcmp(request.path, "/") == 0) {
      send_http_response_safe(fd, HTTP_OK, "text/html", dashboard_html, strlen(dashboard_html));
    }
    else if(strcmp(request.path, "/api/status") == 0) {
      get_status_json(response, sizeof(response));
      send_http_response_safe(fd, HTTP_OK, "application/json", response, strlen(response));
    }
    else if(strcmp(request.path, "/metrics") == 0) {
      get_metrics_text(response, sizeof(response));
      send_http_response_safe(fd, HTTP_OK, "text/plain; version=0.0.4", response, strlen(response));
    }
    else if(strcmp(request.path, "/api/events") == 0) {
      /* Server-Sent Events. The per-connection SO_SNDTIMEO bounds how long a
       * dead client can pin this thread; the handler-thread cap bounds the
       * total number of pinned threads. */
      send_sse_header(fd);
      for(int i = 0; i < 60 && g_web_running; i++) {  /* up to ~5 min */
        get_status_json(response, sizeof(response));
        char sse_msg[8300];
        int sse_len = snprintf(sse_msg, sizeof(sse_msg), "data: %s\n\n", response);
        if(sse_len >= (int)sizeof(sse_msg)) sse_len = sizeof(sse_msg) - 1;
        if(write(fd, sse_msg, sse_len) < 0) break;
        sleep(5);
      }
    }
    else {
      send_http_response_safe(fd, HTTP_NOT_FOUND, "text/plain", "Not Found", 9);
    }
  }
  else if(strcmp(request.method, "HEAD") == 0) {
    if(strcmp(request.path, "/") == 0 || strcmp(request.path, "/api/status") == 0) {
      send_http_response_safe(fd, HTTP_OK, "text/html", "", 0);
    } else {
      send_http_response_safe(fd, HTTP_NOT_FOUND, "text/plain", "", 0);
    }
  }
  else {
    send_http_response_safe(fd, HTTP_METHOD_NOT_ALLOWED, "text/plain", "Method Not Allowed", 18);
  }

  close(fd);
}

/* Thread entry: proper void *(void *) signature (no function-pointer-cast UB),
 * runs one request, then releases its slot in the handler pool. */
static void *handler_entry(void *arg) {
  int fd = (int)(intptr_t)arg;
  handle_request(fd);
  pthread_mutex_lock(&g_web_handlers_mutex);
  if(g_web_handlers > 0) g_web_handlers--;
  pthread_mutex_unlock(&g_web_handlers_mutex);
  return NULL;
}

/* Web server thread */
static void *webserver_thread(void *arg) {
  (void)arg;
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  LOG(LOG_INFO, "Web dashboard started on %s:%d", g_web_bind, g_web_port);

  while(g_web_running) {
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(g_web_socket, &readfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int activity = select(g_web_socket + 1, &readfds, NULL, NULL, &tv);
    if(activity > 0 && FD_ISSET(g_web_socket, &readfds)) {
      int client_fd = accept(g_web_socket, (struct sockaddr *)&client_addr, &client_len);
      if(client_fd < 0) continue;

      /* Per-connection read/write timeouts (defeats slowloris-style holds) */
      struct timeval io_tv;
      io_tv.tv_sec = WEB_IO_TIMEOUT_S;
      io_tv.tv_usec = 0;
      setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
      setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));

      /* Bound the number of concurrent handler threads */
      pthread_mutex_lock(&g_web_handlers_mutex);
      int at_cap = (g_web_handlers >= MAX_WEB_HANDLERS);
      if(!at_cap) g_web_handlers++;
      pthread_mutex_unlock(&g_web_handlers_mutex);

      if(at_cap) {
        LOG(LOG_WARN, "Web dashboard handler pool full (%d), rejecting client", MAX_WEB_HANDLERS);
        send_http_response_safe(client_fd, 503, "text/plain", "Server Busy", 11);
        close(client_fd);
        continue;
      }

      pthread_t handler_thread;
      if(pthread_create(&handler_thread, NULL, handler_entry, (void *)(intptr_t)client_fd) == 0) {
        pthread_detach(handler_thread);
      } else {
        pthread_mutex_lock(&g_web_handlers_mutex);
        if(g_web_handlers > 0) g_web_handlers--;
        pthread_mutex_unlock(&g_web_handlers_mutex);
        close(client_fd);
      }
    }
  }

  LOG(LOG_INFO, "Web dashboard stopped");
  return NULL;
}

void webserver_init(int port) {
  const char *bind_env = getenv("TCPSER_DASH_BIND");
  const char *token_env = getenv("TCPSER_DASH_TOKEN");
  const char *traffic_env = getenv("TCPSER_DASH_TRAFFIC");
  g_web_port = port;
  if(bind_env && bind_env[0]) safe_strncpy(g_web_bind, bind_env, sizeof(g_web_bind));
  if(token_env && token_env[0]) safe_strncpy(g_web_token, token_env, sizeof(g_web_token));
  if(traffic_env && (traffic_env[0] == '1' || traffic_env[0] == 'y' || traffic_env[0] == 'Y'))
    g_web_traffic = 1;
}

void webserver_start(void) {
  struct sockaddr_in server_addr;

  if(g_web_port <= 0) return;

  g_web_socket = socket(AF_INET, SOCK_STREAM, 0);
  if(g_web_socket < 0) {
    LOG(LOG_ERROR, "Failed to create web server socket");
    return;
  }

  int opt = 1;
  setsockopt(g_web_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(g_web_port);
  if(inet_pton(AF_INET, g_web_bind, &server_addr.sin_addr) != 1) {
    LOG(LOG_WARN, "Invalid dashboard bind address '%s', falling back to 127.0.0.1", g_web_bind);
    safe_strncpy(g_web_bind, "127.0.0.1", sizeof(g_web_bind));
    inet_pton(AF_INET, g_web_bind, &server_addr.sin_addr);
  }

  if(bind(g_web_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    LOG(LOG_ERROR, "Failed to bind web server to %s:%d", g_web_bind, g_web_port);
    close(g_web_socket);
    g_web_socket = -1;
    return;
  }

  if(listen(g_web_socket, 5) < 0) {
    LOG(LOG_ERROR, "Failed to listen on web server port %d", g_web_port);
    close(g_web_socket);
    g_web_socket = -1;
    return;
  }

  /* Warn loudly if exposed beyond localhost without authentication. */
  int loopback = (strcmp(g_web_bind, "127.0.0.1") == 0 || strcmp(g_web_bind, "localhost") == 0);
  if(!loopback && g_web_token[0] == 0) {
    LOG(LOG_WARN, "Web dashboard bound to %s with NO token — caller IPs and stats "
                  "are exposed to anyone who can reach port %d. Set TCPSER_DASH_TOKEN.",
        g_web_bind, g_web_port);
  }
  /* Live-traffic CONTENT preview can reveal what callers type (incl. passwords).
   * Warn hard if it is exposed beyond localhost. */
  if(g_web_traffic && (!loopback || g_web_token[0] == 0)) {
    LOG(LOG_WARN, "TCPSER_DASH_TRAFFIC is ON — the web dashboard streams caller "
                  "session CONTENT. Only expose this behind localhost + a token.");
  }

  g_web_running = 1;
  if(pthread_create(&g_web_thread, NULL, webserver_thread, NULL) != 0) {
    LOG(LOG_ERROR, "Failed to start web server thread");
    close(g_web_socket);
    g_web_socket = -1;
    g_web_running = 0;
    return;
  }
}

void webserver_stop(void) {
  if(g_web_running) {
    g_web_running = 0;
    pthread_join(g_web_thread, NULL);
    close(g_web_socket);
    g_web_socket = -1;
  }
}

int webserver_is_running(void) {
  return g_web_running;
}

int webserver_get_port(void) {
  return g_web_port;
}

const char *webserver_get_bind(void) {
  return g_web_bind;
}

int webserver_has_token(void) {
  return g_web_token[0] != 0;
}
