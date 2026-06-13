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

#endif
