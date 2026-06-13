/*
 * Webhook notifications — fire-and-forget HTTP POST via fork+curl.
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

static void fire_one(const char *url, const char *payload) {
  int p[2];
  pid_t pid;

  if(pipe(p) != 0) return;

  pid = fork();
  if(pid == 0) {
    /* Child — read curl config (the secret URL) from stdin, fire, exit. */
    dup2(p[0], 0);
    close(p[0]);
    close(p[1]);
    int dn = open("/dev/null", O_WRONLY);
    if(dn >= 0) {
      dup2(dn, 1);
      dup2(dn, 2);
      if(dn > 2) close(dn);
    }
    /* Reset PATH to trusted system locations so a hijacked/inherited PATH can't
     * make us exec a malicious 'curl'. */
    setenv("PATH", "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin", 1);
    /* Payload is not secret (event + IP), so -d on argv is fine; the URL is
     * supplied via the -K - config read from stdin and never appears in ps. */
    execlp("curl", "curl", "-s", "-K", "-", "-X", "POST",
           "-H", "Content-Type: application/json",
           "-d", payload,
           "--max-time", "10",
           (char *)NULL);
    _exit(0);
  }

  close(p[0]);
  if(pid > 0) {
    char url_esc[1024];
    char cfg[1100];
    int len;
    cfg_escape(url, url_esc, sizeof(url_esc));
    len = snprintf(cfg, sizeof(cfg), "url = \"%s\"\n", url_esc);
    if(len > 0) {
      ssize_t w = write(p[1], cfg, (size_t)len);
      (void)w;
    }
  }
  close(p[1]);
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
