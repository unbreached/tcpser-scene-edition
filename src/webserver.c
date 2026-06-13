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
"body { font: 14px monospace; background: #000; color: #0f0; padding: 10px; }\n"
".header { color: #ff0; margin-bottom: 20px; text-align: center; }\n"
".section { border: 1px solid #333; margin: 10px 0; padding: 10px; }\n"
".section h3 { color: #0ff; margin-bottom: 10px; }\n"
".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 15px; }\n"
".stats { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }\n"
".stat { padding: 5px; border: 1px solid #444; text-align: center; }\n"
".stat .label { color: #888; font-size: 12px; }\n"
".stat .value { color: #0f0; font-weight: bold; }\n"
".history { max-height: 200px; overflow-y: auto; }\n"
".history-item { padding: 3px 0; border-bottom: 1px solid #222; }\n"
".connected { color: #0f0; }\n"
".blocked { color: #f00; }\n"
".gate-fail { color: #fa0; }\n"
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
    struct tm *tmv = localtime(&g_dash.history[idx].connect_time);
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

  strncat(buffer, "\n  ]\n}", size - strlen(buffer) - 1);
}

/* Generate Prometheus/OpenMetrics text exposition */
static void get_metrics_text(char *buffer, size_t size) {
  extern dashboard_state_t g_dash;
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
  char response[4096];
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
        char sse_msg[4200];
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
  g_web_port = port;
  if(bind_env && bind_env[0]) safe_strncpy(g_web_bind, bind_env, sizeof(g_web_bind));
  if(token_env && token_env[0]) safe_strncpy(g_web_token, token_env, sizeof(g_web_token));
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
