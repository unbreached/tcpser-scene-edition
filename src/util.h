#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif



int writePipe(int fd, char msg);
int readPipe(int fd, unsigned char *buf, int len);
int writeFile(char *name, int fd);
void spawn_thread(void * thread, void *arg, char *name);

/* Shared string/time helpers (see util.c). */
void safe_strncpy(char *dst, const char *src, size_t size);
int  ct_streq(const char *a, const char *b);
void fmt_timestamp(char *buf, size_t size, const char *fmt);

/* Seconds on a clock that never jumps (CLOCK_MONOTONIC). Every timeout and
 * deadline uses this: a Raspberry Pi without an RTC steps the wall clock by
 * decades when NTP syncs after boot. Never 0 (0 means "not set" to callers). */
#include <time.h>
time_t mono_now(void);
long long mono_ms(void);
void set_cloexec(int fd);

#endif
