/*
 * Webhook notifications — fire-and-forget HTTP POST via posix_spawn+curl.
 * No library dependencies (uses system curl).
 *
 * Multiple targets are supported (repeat -e). The webhook URL often embeds a
 * secret token, so it is NOT passed on curl's command line (which would expose
 * it in `ps`); instead it is handed to curl through a `-K -` config on stdin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <spawn.h>
#include <errno.h>

#include "webhook.h"
#include "debug.h"
#include "util.h"

#define MAX_WEBHOOKS 8

static char g_webhook_urls[MAX_WEBHOOKS][512];
static int  g_webhook_count = 0;

/* Mask the middle of a URL so logs don't leak an embedded token. */
static void mask_url(const char *url, char *out, size_t outsz) {
  size_t n = strlen(url);
  if(n <= 24) {
    snprintf(out, outsz, "%.8s...", url);
  } else {
    snprintf(out, outsz, "%.16s...%.6s", url, url + n - 6);
  }
}

int webhook_count(void) { return g_webhook_count; }

void webhook_init(const char *url) {
  char masked[64];
  if(!url || !url[0]) return;
  if(g_webhook_count >= MAX_WEBHOOKS) {
    LOG(LOG_WARN, "Too many webhook targets (max %d), ignoring extra", MAX_WEBHOOKS);
    return;
  }
  safe_strncpy(g_webhook_urls[g_webhook_count], url, sizeof(g_webhook_urls[0]));
  g_webhook_count++;
  mask_url(url, masked, sizeof(masked));
  LOG(LOG_INFO, "Webhook target #%d configured: %s", g_webhook_count, masked);
}

/* Escape a string for use inside a curl config double-quoted value. */
static void cfg_escape(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  for(size_t i = 0; in[i] && o + 2 < outsz; i++) {
    if(in[i] == '"' || in[i] == '\\') out[o++] = '\\';
    out[o++] = in[i];
  }
  out[o] = 0;
}

/* Candidate curl binaries, tried in order. Absolute paths: the parent's PATH
 * could be attacker-influenced in a sloppy service environment. */
static const char *const CURL_PATHS[] = { "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/opt/homebrew/bin/curl", NULL };

extern char **environ;

static void fire_one(const char *url, const char *payload) {
  int p[2];
  pid_t pid = -1;
  const char *curl = NULL;
  for(int i = 0; CURL_PATHS[i]; i++) if(access(CURL_PATHS[i], X_OK) == 0) { curl = CURL_PATHS[i]; break; }
  if(!curl) { LOG(LOG_WARN, "webhook: curl not found in /usr/bin, /bin, /usr/local/bin or /opt/homebrew/bin"); return; }
  if(pipe(p) != 0) return;

  /* posix_spawn, not fork(): forking a multithreaded process and then calling
   * anything that may take a lock (setenv, stdio, the logger) can deadlock
   * the child before exec (audit STAB-M7). All child-side setup is expressed
   * as file actions and an explicit environment instead. */
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, p[0], 0);           /* curl config (the secret URL) on stdin */
  posix_spawn_file_actions_addclose(&fa, p[0]);
  posix_spawn_file_actions_addclose(&fa, p[1]);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

  /* environment: the parent's, minus PATH, plus a trusted PATH */
  int n = 0; while(environ && environ[n]) n++;
  char **env = calloc((size_t)n + 2, sizeof(char *));
  int m = 0;
  if(env) {
    for(int i = 0; i < n; i++) if(strncmp(environ[i], "PATH=", 5) != 0) env[m++] = environ[i];
    env[m++] = "PATH=/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin";
    env[m] = NULL;
  }
  /* Payload is not secret (event + IP), so -d on argv is fine; the URL is
   * supplied via the -K - config read from stdin and never appears in ps. */
  char *const argv[] = { "curl", "-s", "-K", "-", "-X", "POST",
                         "-H", "Content-Type: application/json",
                         "-d", (char *)payload, "--max-time", "10", NULL };
  int rc = posix_spawn(&pid, curl, &fa, NULL, argv, env ? env : environ);
  posix_spawn_file_actions_destroy(&fa);
  free(env);
  close(p[0]);
  if(rc == 0 && pid > 0) {
    char url_esc[1024];
    char cfg[1100];
    cfg_escape(url, url_esc, sizeof(url_esc));
    int len = snprintf(cfg, sizeof(cfg), "url = \"%s\"\n", url_esc);
    if(len > 0) {
      ssize_t w = write(p[1], cfg, (size_t)len);
      (void)w;
    }
  } else {
    LOG(LOG_WARN, "webhook: could not start %s: %s", curl, strerror(rc));
  }
  close(p[1]);   /* EOF on curl's stdin; the child is auto-reaped (SIGCHLD ignored) */
}

void webhook_notify(const char *event, const char *ip) {
  char payload[1024];
  char ts[32];

  if(g_webhook_count == 0 || !event) return;

  fmt_timestamp(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");

  /* JSON payload — works with Discord, Slack, ntfy, and generic webhooks */
  snprintf(payload, sizeof(payload),
    "{\"content\":\"[%s] **%s** from %s\","
    "\"username\":\"TCPSER\","
    "\"embeds\":[{\"title\":\"%s\","
    "\"description\":\"IP: %s\\nTime: %s\","
    "\"color\":%s}]}",
    ts, event, ip ? ip : "?",
    event,
    ip ? ip : "?", ts,
    strcmp(event, "CONNECTED") == 0 ? "3066993" :       /* green  */
    strcmp(event, "DISCONNECTED") == 0 ? "15158332" :   /* red    */
    strcmp(event, "GATE_FAIL") == 0 ? "15158332" :      /* red    */
    strcmp(event, "IP_BLOCKED") == 0 ? "10038562" :     /* dark   */
    "16776960"                                           /* yellow */
  );

  for(int i = 0; i < g_webhook_count; i++) {
    fire_one(g_webhook_urls[i], payload);
  }
  /* Parent continues immediately — SIGCHLD is ignored (auto-reap) */
  LOG(LOG_DEBUG, "Webhook fired for %s to %d target(s)", event, g_webhook_count);
}
