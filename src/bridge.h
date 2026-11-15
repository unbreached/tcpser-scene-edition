#ifndef BRIDGE_H
#define BRIDGE_H

#include <time.h>
#include "modem_core.h"

#define MSG_CALLING       'C'
#define MSG_BUSY          'B'
#define MSG_DTR_UP        'D'
#define MSG_DTR_DOWN      'd'
#define MSG_LE_UP         'L'
#define MSG_LE_DOWN       'l'
#define MSG_DISCONNECT    'H'
#define MSG_NOTIFY        'N'
#define MSG_HANGUP        'h'  /* dashboard: force hangup */
#define MSG_RESET         'R'  /* dashboard: factory reset modem */
#define MSG_GATE_DONE     'G'  /* gate thread: banner/gate/baud menu finished (result in cfg) */
#define MSG_DIAL_DONE     'O'  /* dial thread: outgoing call finished (result in cfg) */
#define MSG_SER_LOST      'X'  /* ctrl_thread: serial status unavailable (USB adapter gone?) */

int accept_connection(modem_config *);
void *bridge_task(void *arg);
int blocklist_count(void);
int blocklist_list(char buf[][16], int max);
typedef struct { char ip[16]; int permanent; time_t until; } block_info_t;
int blocklist_list_ex(block_info_t *out, int max);
int blocklist_unblock_all_timed(void);
/* allowlist, editable from the dashboards (the file stays the source of truth) */
int allowlist_list(char out[][16], int max);
int allowlist_has(const char *ip);
int allowlist_add(const char *ip);      /* 0 added, 1 already listed, -1 not an IPv4 address, -2 file error, -3 list full */
int allowlist_remove(const char *ip);   /* 0 removed, 1 was not listed, -2 file error */

#endif
