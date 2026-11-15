#ifndef NVT_H
#define NVT_H 1

#include <stddef.h>
#include "dce.h"

typedef enum {
  NVT_SE = 240,
  NVT_NOP = 241,
  NVT_DM = 242,
  NVT_SB = 250,
  NVT_WILL = 251,
  NVT_WONT = 252,
  NVT_DO = 253,
  NVT_DONT = 254,
  NVT_IAC = 255,
} nvt_command;

typedef enum {
  NVT_OPT_TRANSMIT_BINARY = 0,
  NVT_OPT_ECHO = 1,
  NVT_OPT_SUPPRESS_GO_AHEAD = 3,
  NVT_OPT_STATUS = 5,
  NVT_OPT_RCTE = 7,
  NVT_OPT_TIMING_MARK = 6,
  NVT_OPT_NAOCRD = 10,
  NVT_OPT_TERMINAL_TYPE = 24,
  NVT_OPT_NAWS = 31,
  NVT_OPT_TERMINAL_SPEED = 32,
  NVT_OPT_LINEMODE = 34,
  NVT_OPT_X_DISPLAY_LOCATION = 35,
  NVT_OPT_ENVIRON = 36,
  NVT_OPT_NEW_ENVIRON = 39,
} nvt_option;

#define NVT_SB_IS 0
#define NVT_SB_SEND 1

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* Option state per RFC 1143 (simplified Q method): a request is answered only
 * when it changes state, so a peer that repeats DO/WILL cannot make us loop. */
enum { NVT_Q_NO = 0, NVT_Q_YES = 1, NVT_Q_WANTYES = 2 };

/* Which end of the call we are: a caller dialled in (we are the "server"), or
 * the C64 dialled out with ATD (we are the "client"). */
enum { NVT_ROLE_SERVER = 0, NVT_ROLE_CLIENT = 1 };

/* Parser state, kept across reads so an IAC sequence split over two TCP
 * segments is still one sequence. */
enum { NVT_ST_DATA = 0, NVT_ST_IAC, NVT_ST_OPT, NVT_ST_SB, NVT_ST_SB_IAC };

typedef struct nvt_vars {
  int binary_xmit;
  int binary_recv;
  int sga_local;      /* we suppress go-ahead */
  int sga_remote;     /* peer suppresses go-ahead */
  int echo_local;     /* we handle echo */
  int naws_enabled;   /* peer sends window size */
  int cols;           /* terminal columns (from NAWS) */
  int rows;           /* terminal rows (from NAWS) */
  int negotiated;     /* proactive negotiation sent */
  int role;           /* NVT_ROLE_* */
  unsigned char us[256];    /* NVT_Q_* for options on our side (WILL/WONT) */
  unsigned char him[256];   /* NVT_Q_* for options on the peer's side (DO/DONT) */
  int st;             /* NVT_ST_* */
  unsigned char cmd;  /* WILL/WONT/DO/DONT waiting for its option byte */
  unsigned char sb[64];
  int sb_len;
  int last_cr;        /* previous data byte was CR (for CR NUL) */
  char ttype[41];     /* peer's terminal type, if it told us */
} nvt_vars;

void nvt_init_config(nvt_vars *vars);
void nvt_negotiate_proactive(dce_config *cfg, int fd, nvt_vars *vars);
/* Run received bytes through the telnet parser. Data bytes go to out (which
 * must hold len + 1 bytes); negotiation is answered on fd. *is_telnet is set
 * when a telnet peer is recognised, which can only happen while detect is
 * non-zero. Returns the number of data bytes. */
int nvt_filter(dce_config *cfg, int fd, nvt_vars *vars, int *is_telnet, int detect,
               const unsigned char *in, int len, unsigned char *out);
/* Called when the detection window closes: a lone 0xFF still held back is data. */
int nvt_flush_pending(nvt_vars *vars, int is_telnet, unsigned char *out);

#endif
