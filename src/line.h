#ifndef LINE_H
#define LINE_H 1

#include "nvt.h"

#include <netinet/in.h>
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#include <pthread.h>
#include <stdatomic.h>
typedef struct line_config {
  _Atomic int fd;            /* read by ip_thread, written by bridge_task */
  int sfd;
  _Atomic int is_connected;
  /* Per-connection telnet state. Used by the reader thread, the gate thread
   * and the bridge thread, so it is guarded by `lock`; `gen` is bumped on
   * every connect and disconnect so a thread that was still working on the
   * previous call can tell and drop its result instead of applying it to the
   * next caller. */
  pthread_mutex_t lock;
  _Atomic unsigned gen;
  int is_telnet;
  int is_data_received;
  nvt_vars nvt_data;

  char peer_ip[INET_ADDRSTRLEN]; /* remote peer IPv4 (dotted) */
} line_config;

void line_init_config(line_config *cfg);
int line_init_conn(line_config *cfg);
int line_read(line_config *cfg, unsigned char *data, int len);
int line_write(line_config *cfg, unsigned char *data, int len);
int line_listen(line_config *cfg);
int line_accept(line_config *cfg);
int line_off_hook(line_config *cfg);
int line_connect(line_config *cfg, char* dialno);
/* Adopt an already connected outgoing socket (from the dial thread). */
void line_attach(line_config *cfg, int fd);
int line_disconnect(line_config *cfg);
/* Wake whoever is blocked on the socket without closing it (the owner closes). */
void line_abort(line_config *cfg);
/* Telnet-filter received bytes into out (len + 2 bytes). gen is the value of
 * cfg->gen when the bytes were read: returns -1 if the call has ended since.
 * online = the call is established (closes the telnet detection window after
 * this chunk); 0 while the caller is still in the gate. */
int line_filter(line_config *cfg, dce_config *dce, unsigned gen, int online,
                const unsigned char *in, int len, unsigned char *out);

#endif
