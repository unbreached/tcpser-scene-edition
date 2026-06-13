#include <pthread.h>
#include <stdlib.h>       // for exit...
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "util.h"

int writePipe(int fd, char msg) {
  char tmp[1];

  tmp[0] = msg;

  return write(fd, tmp, 1);
}

int readPipe(int fd, unsigned char *buf, int len) {
  return read(fd, buf, len);
}

int writeFile(char *name, int fd) {
  FILE *file;
  unsigned char buf[255];
  size_t res;
  size_t size = 1;
  size_t max = sizeof(buf);

  if(NULL != (file = fopen(name, "rb"))) {
    do {
      res = fread(buf, size, max, file);
      if(res > 0)
        res = write(fd, buf, res);
    } while (res > 0);
    fclose(file);
    return 0;
  }
  return -1;
}

void spawn_thread(void * thread, void *arg, char *name) {
  int rc;
  pthread_t thread_id;

  /* pthread_create returns an error number (>0) on failure, 0 on success —
   * never a negative value, so the old `rc < 0` test never fired and we
   * detached an uninitialized thread id. Check rc != 0. */
  rc = pthread_create(&thread_id, NULL, thread, arg);
  if(rc != 0) {
    ELOG(LOG_FATAL, "%s thread could not be started", name);
    exit(-1);
  }
  LOG(LOG_ALL, "%s thread ID = %ld", name, (long)thread_id);
  pthread_detach(thread_id);
}

/* Safe string copy: always NUL-terminates, unlike strncpy(). */
void safe_strncpy(char *dst, const char *src, size_t size) {
  if(!dst || size == 0) return;
  snprintf(dst, size, "%s", src ? src : "");
}

/* Constant-time string equality. Avoids leaking the matching-prefix length
 * through timing, unlike strcmp(). Used for password/token comparison. */
int ct_streq(const char *a, const char *b) {
  if(!a || !b) return a == b;
  size_t la = strlen(a);
  size_t lb = strlen(b);
  size_t n = la > lb ? la : lb;
  unsigned char diff = (unsigned char)(la ^ lb);
  for(size_t i = 0; i < n; i++) {
    unsigned char ca = i < la ? (unsigned char)a[i] : 0;
    unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
    diff |= (unsigned char)(ca ^ cb);
  }
  return diff == 0;
}

/* Format the current local time into buf using strftime fmt, with a NULL-tm
 * guard (localtime() can return NULL). Several modules duplicated this idiom
 * and three of them lacked the guard. */
void fmt_timestamp(char *buf, size_t size, const char *fmt) {
  if(!buf || size == 0) return;
  buf[0] = 0;
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  if(tm) {
    strftime(buf, size, fmt, tm);
  } else {
    snprintf(buf, size, "?");
  }
}
