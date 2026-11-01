#ifndef MODEM_CORE_H
#define MODEM_CORE_H 1

#include <time.h>

typedef enum {
  MDM_RESP_OK = 0,
  MDM_RESP_CONNECT = 1,
  MDM_RESP_RING = 2,
  MDM_RESP_NO_CARRIER = 3,
  MDM_RESP_ERROR = 4,
  MDM_RESP_CONNECT_1200 = 5,
  MDM_RESP_NO_DIALTONE = 6,
  MDM_RESP_BUSY = 7,
  MDM_RESP_NO_ANSWER = 8,
  MDM_RESP_CONNECT_0600 = 9,
  MDM_RESP_CONNECT_2400 = 10,
  MDM_RESP_CONNECT_4800 = 11,
  MDM_RESP_CONNECT_9600 = 12,
  MDM_RESP_CONNECT_7200 = 13,
  MDM_RESP_CONNECT_12000 = 14,
  MDM_RESP_CONNECT_14400 = 15,
  MDM_RESP_CONNECT_19200 = 16,
  MDM_RESP_CONNECT_38400 = 17,
  MDM_RESP_CONNECT_57600 = 18,
  MDM_RESP_CONNECT_115200 = 19,
  MDM_RESP_CONNECT_230400 = 20,
  MDM_RESP_END_OF_LIST
} modem_response;

#define MDM_FC_RTS 1
#define MDM_FC_XON 2

#define MDM_CONN_NONE 0
#define MDM_CONN_OUTGOING 1
#define MDM_CONN_INCOMING 2

/* Max entries in the on-connect caller baud-selection menu (-b).
 * The menu is selected by a single keypress ('1'..'9'), so at most 9 entries
 * can ever be chosen — keep the capacity in sync so we never display an
 * unselectable option. */
#define BAUD_MENU_MAX 9

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#include "dce.h"
#include "line.h"
#include "nvt.h"

/* Operator profile: the modem settings in force after the -i init string and
 * command-line flags (-I etc.) have been applied. ATZ / AT&F restore THIS, the
 * way a real modem restores its stored profile, instead of compiled-in factory
 * defaults — otherwise the first ATZ every C64 BBS sends wiped the operator's
 * inverted-DCD / &C0 / &K3 wiring configuration. */
typedef struct mdm_profile {
  int valid;
  int send_responses;
  int connect_response;
  int response_code_level;
  int text_responses;
  int is_echo;
  int dtr_mode;
  int force_dsr;
  int force_dcd;
  int invert_dsr;
  int invert_dcd;
  int flow_control;
  int s[100];
} mdm_profile;

enum {
  S_REG_RINGS = 0,
  S_REG_BREAK = 2,
  S_REG_CR = 3,
  S_REG_LF = 4,
  S_REG_BS = 5,
  S_REG_BLIND_WAIT = 6,
  S_REG_CARRIER_WAIT = 7,
  S_REG_COMMA_PAUSE = 8,
  S_REG_CARRIER_TIME = 9,
  S_REG_CARRIER_LOSS = 10,
  S_REG_DTMF_TIME = 11,
  S_REG_GUARD_TIME = 12,
  S_REG_INACTIVITY_TIME = 30
};

typedef struct modem_config {

  // master configuration information
  int mp[2][2];
  int cp[2][2];
  int wp[2][2];
  char no_answer[256];
  char local_connect[256];
  char remote_connect[256];
  char local_answer[256];
  char remote_answer[256];
  char inactive[256];
  int direct_conn;
  char direct_conn_num[256];

  // need to eventually change these
  dce_config dce_data;
  line_config line_data;
  int line_speed;
  int conn_type;
  time_t conn_established;  /* time connection was established (for DTR grace) */
  int is_ringing;
  int is_off_hook;
  int dsr_active;
  int force_dsr;
  int force_dcd;
  int invert_dsr;
  int invert_dcd;
  int allow_transmit;
  int is_binary_negotiated;
  int rings;
  // command information
  int pre_break_delay;
  unsigned char first_ch;
  int is_cmd_started;
  int is_cmd_mode;
  char cur_line[1024];
  int cur_line_idx;
  int last_line_idx;
  // dailing information
  char dialno[256];
  char last_dialno[256];
  char dial_type;
  char last_dial_type;
  int memory_dial;
  // modem config
  int connect_response;
  int response_code_level;
  int send_responses;
  int text_responses;
  int is_echo;
  int s[100];
  int break_len;
  int disconnect_delay;
  char crlf[3];

  // DTR behavior (&D0/&D1/&D2):
  // 0 = ignore, 1 = command mode (keep carrier), 2 = hang up
  int dtr_mode;

  // Pre-connect (TCP) banner / bot gate (runs BEFORE any serial/modem traffic)
  char pre_connect_file[256];   // optional file to send immediately after TCP accept
  char access_granted_file[256]; // optional file to show upon successful gate (sent to TCP client)
  char on_gate_success_script[256]; // optional external command to execute asynchronously on successful gate
  int  gate_mode;               // 0=off, 1=esc, 2=password
  char gate_password[64];       // used when gate_mode==2
  int  gate_timeout;            // seconds (0 -> default 20, max 300)
  int  gate_max_fails;          // gate failures before permanent block (default 3)

  // Incoming-call answer timeout (TCP caller connected, local serial/BBS must answer)
  // Used to send -N (no answer) and drop the TCP connection if the BBS doesn't answer.
  int  incoming_waiting_answer;
  time_t incoming_answer_deadline;
  int  incoming_answer_timeout;   // seconds (defaults to 10)

  // BBS watchdog: after disconnect, if no serial data within this many seconds,
  // toggle DTR to force BBS to notice carrier loss and reset.
  // 0 = disabled, default = 120 (2 minutes).  Set via -w flag.
  int  watchdog_timeout;

  // Max session time in minutes (0 = unlimited).  Set via -m flag.
  // Disconnects caller after this many minutes to prevent line hogging.
  int  max_session_minutes;

  // On-connect caller baud selection (-b). When enabled, an incoming TCP caller
  // is shown a speed menu; on selection tcpser sends "POKE 838,<val>" to CBase
  // (which runs it from its input stream) and retunes its own serial port to the
  // chosen baud so the whole session runs at that speed.
  int  baud_menu_enabled;
  int  baud_menu_count;
  int  baud_menu_speed[BAUD_MENU_MAX];   // e.g. 9600
  int  baud_menu_poke[BAUD_MENU_MAX];    // POKE 838 value that selects that speed

  mdm_profile profile;                   // restored by ATZ / AT&F (see mdm_save_profile)
} modem_config;

void mdm_init(void);
void mdm_init_config(modem_config *cfg);
void mdm_save_profile(modem_config *cfg);
void mdm_reset_profile(modem_config *cfg);
int get_new_cts_state(modem_config *cfg, int up);
int get_new_dsr_state(modem_config *cfg, int up);
int get_new_dcd_state(modem_config *cfg, int up);
int mdm_set_control_lines(modem_config *cfg);
void mdm_write_char(modem_config *cfg, unsigned char data);
int  mdm_write(modem_config *cfg, unsigned char *data, int len);
void mdm_send_response(int msg, modem_config *cfg);
int mdm_off_hook(modem_config *cfg);
int mdm_answer(modem_config *cfg);
int mdm_print_speed(modem_config *cfg);
int mdm_connect(modem_config *cfg);
int mdm_listen(modem_config *cfg);
int mdm_disconnect(modem_config *cfg, unsigned char force);
int mdm_parse_cmd(modem_config *cfg);
int mdm_handle_char(modem_config *cfg, unsigned char ch);
int mdm_clear_break(modem_config *cfg);
int mdm_parse_data(modem_config *cfg, unsigned char *data, int len);
int mdm_handle_timeout(modem_config *cfg);
int mdm_send_ring(modem_config *cfg);
int mdm_read(modem_config *cfg, unsigned char *data, int len);

#endif
