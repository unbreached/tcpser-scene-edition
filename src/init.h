#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#include "modem_core.h"

#define MAX_PORTS 8

typedef struct {
  char addr[64];         /* bind address:port (e.g., ":23", "127.0.0.1:6400") */
  int  port;             /* port number */
  int  socket;           /* server socket fd */
  int  modem_list[16];   /* assigned modem indices */
  int  modem_count;      /* number of assigned modems */
  char name[32];         /* display name for logging */
} port_config;

void print_help(char *name);
int init(int argc,
         char **argv,
         modem_config cfg[],
         int max_modem,
         port_config ports[],
         int *port_count,
         char *all_busy,
         int all_busy_len,
         int *analyzer_mode
        );

