#include <stdio.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include "connlog.h"
#include "util.h"

static char g_logpath[256] = {0};
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Open the connection log for appending, restricting it to owner-only (0600).
 * The log records caller IP addresses (PII), so it must not be world-readable
 * regardless of the process umask. */
static FILE *connlog_open(void) {
  FILE *f = fopen(g_logpath, "a");
  if(f) {
    (void)chmod(g_logpath, S_IRUSR | S_IWUSR);
  }
  return f;
}

void connlog_init(const char *path) {
  FILE *f;
  if(!path || !path[0]) return;
  safe_strncpy(g_logpath, path, sizeof(g_logpath));

  /* Write header if file is new/empty */
  f = connlog_open();
  if(f) {
    if(ftell(f) == 0) {
      fprintf(f, "timestamp,ip,result,duration,bytes_in,bytes_out\n");
    }
    fclose(f);
  }
}

static void write_entry(const char *ip, const char *result, int duration,
                         long bytes_in, long bytes_out) {
  char ts[32];
  FILE *f;

  if(!g_logpath[0]) return;

  fmt_timestamp(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");

  pthread_mutex_lock(&g_log_mutex);
  f = connlog_open();
  if(f) {
    fprintf(f, "%s,%s,%s,%d,%ld,%ld\n", ts, ip ? ip : "?", result,
            duration, bytes_in, bytes_out);
    fclose(f);
  }
  pthread_mutex_unlock(&g_log_mutex);
}

void connlog_event(const char *ip, const char *result) {
  write_entry(ip, result, 0, 0, 0);
}

void connlog_session_end(const char *ip, int duration, long bytes_in, long bytes_out) {
  write_entry(ip, CONNLOG_HANGUP, duration, bytes_in, bytes_out);
}
