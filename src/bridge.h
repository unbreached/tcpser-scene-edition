#ifndef BRIDGE_H
#define BRIDGE_H

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
#define MSG_SER_LOST      'X'  /* ctrl_thread: serial status unavailable (USB adapter gone?) */

int accept_connection(modem_config *);
int parse_ip_data(modem_config *cfg, unsigned char *data, int len);
void *bridge_task(void *arg);
int blocklist_count(void);
int blocklist_list(char buf[][16], int max);
int blocklist_unblock_all_timed(void);

#endif
