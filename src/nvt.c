#include <string.h>

#include "debug.h"
#include "ip.h"
#include "modem_core.h"

#include "nvt.h"

void nvt_init_config(nvt_vars *vars) {
  int i;

  vars->binary_xmit = FALSE;
  vars->binary_recv = FALSE;
  vars->sga_local = FALSE;
  vars->sga_remote = FALSE;
  vars->echo_local = FALSE;
  vars->naws_enabled = FALSE;
  vars->cols = 80;
  vars->rows = 25;
  vars->negotiated = FALSE;
  for (i = 0; i < 256; i++)
    vars->term[i] = 0;
}

unsigned char get_nvt_cmd_response(unsigned char action, unsigned char type) {
  unsigned char rc = 0;

  if(type == TRUE) {
    switch (action) {
      case NVT_DO:
        rc = NVT_WILL;
        break;
      case NVT_DONT:
        rc = NVT_WONT;
        break;
      case NVT_WILL:
        rc = NVT_DO;
        break;
      case NVT_WONT:
        rc = NVT_DONT;
        break;
    }
  } else {
    switch (action) {
      case NVT_DO:
      case NVT_DONT:
        rc = NVT_WONT;
        break;
      case NVT_WILL:
      case NVT_WONT:
        rc = NVT_DONT;
        break;
    }
  }
  return rc;
}

int parse_nvt_subcommand(dce_config *cfg, int fd, nvt_vars *vars, unsigned char *data, int len) {
  nvt_option opt;
  unsigned char resp[100];
  unsigned char *response = resp + 3;
  int resp_len = 0;
  int response_len = 0;
  char tty_type[] = "VT100";
  int rc;
  char buf[50];
  int slen = 0;

  if(len < 4) return len;  /* need at least IAC SB <opt> <data> */
  opt = data[2];

  for (rc = 2; rc < len - 1; rc++) {
    if (NVT_IAC == data[rc])
      if (NVT_SE == data[rc + 1]) {
        rc += 2;
        break;
      }
  }

  /* NAWS: IAC SB 31 <cols-hi> <cols-lo> <rows-hi> <rows-lo> IAC SE */
  if(opt == NVT_OPT_NAWS && rc >= 9) {
    int cols = (data[3] << 8) | data[4];
    int rows = (data[5] << 8) | data[6];
    if(cols > 0 && cols < 500) vars->cols = cols;
    if(rows > 0 && rows < 500) vars->rows = rows;
    LOG(LOG_INFO, "NAWS: terminal size %dx%d", vars->cols, vars->rows);
    return rc;
  }

  if (rc > 5 && (NVT_SB_SEND == data[3])) {
    switch(opt) {
      case NVT_OPT_TERMINAL_TYPE:
      case NVT_OPT_X_DISPLAY_LOCATION:
      case NVT_OPT_ENVIRON:
      case NVT_OPT_NEW_ENVIRON:
      case NVT_OPT_TERMINAL_SPEED:
        response[response_len++] = NVT_SB_IS;
        switch(opt) {
          case NVT_OPT_TERMINAL_TYPE:
            slen = strlen(tty_type);
            memcpy(response + response_len, tty_type, slen);
            response_len += slen;
            LOG(LOG_INFO, "Telnet: reporting terminal type %s", tty_type);
            break;
          case NVT_OPT_TERMINAL_SPEED:
            slen = snprintf(buf, sizeof(buf), "%i,%i", cfg->port_speed, cfg->port_speed);
            if (slen >= (int)sizeof(buf)) slen = sizeof(buf) - 1;
            if (response_len + slen < 97) {  /* Ensure we don't overflow response buffer */
              memcpy(response + response_len, buf, slen);
              response_len += slen;
              LOG(LOG_INFO, "Telnet: reporting speed %s", buf);
            } else {
              LOG(LOG_ERROR, "Telnet response buffer overflow prevented");
            }
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }

  if (response_len) {
    resp[resp_len++] = NVT_IAC;
    resp[resp_len++] = NVT_SB;
    resp[resp_len++] = opt;
    resp_len += response_len;
    resp[resp_len++] = NVT_IAC;
    resp[resp_len++] = NVT_SE;
    ip_write(fd, resp, resp_len);
  }
  return rc;
}

int get_action_safe(char *txt, size_t txt_size, nvt_command action) {
  const char *action_str;

  switch (action) {
  case NVT_WILL:
    action_str = "WILL";
    break;
  case NVT_WONT:
    action_str = "WONT";
    break;
  case NVT_DO:
    action_str = "DO";
    break;
  case NVT_DONT:
    action_str = "DONT";
    break;
  default:
    action_str = "UNKNOWN";
    break;
  }

  return snprintf(txt, txt_size, "%s", action_str);
}

int send_nvt_command(int fd, nvt_vars *vars, nvt_command action, nvt_option opt) {
  unsigned char cmd[3];
  char txt[20];

  get_action_safe(txt, sizeof(txt), action);
  LOG(LOG_DEBUG, "Sending NVT command: %s %d", txt, opt);
  cmd[0] = NVT_IAC;
  cmd[1] = action;
  cmd[2] = opt;

  ip_write(fd, cmd, 3);
  vars->term[opt] = action;

  return 0;
}

int parse_nvt_command(dce_config *cfg, int fd, nvt_vars *vars, nvt_command action, nvt_option opt) {
  int accept = FALSE;
  char txt[20];
  int resp;

  get_action_safe(txt, sizeof(txt), action);
  LOG(LOG_DEBUG, "Received NVT command: %s %d", txt, opt);
  switch (opt) {
    case NVT_OPT_TRANSMIT_BINARY:
      switch (action) {
        case NVT_DO:
          if(!dce_get_parity(cfg)) {
            LOG(LOG_INFO, "Enabling telnet binary xmit");
            vars->binary_xmit = TRUE;
            accept = TRUE;
          }
          break;
        case NVT_DONT:
          LOG(LOG_INFO, "Disabling telnet binary xmit");
          vars->binary_xmit = FALSE;
          accept = TRUE;
          break;
        case NVT_WILL:
          if(!dce_get_parity(cfg)) {
            LOG(LOG_INFO, "Enabling telnet binary recv");
            vars->binary_recv = TRUE;
            accept = TRUE;
          }
          break;
        case NVT_WONT:
          LOG(LOG_INFO, "Disabling telnet binary recv");
          vars->binary_recv = FALSE;
          accept = TRUE;
          break;
        default:
          break;
      }
      resp = get_nvt_cmd_response(action, accept);
      break;
    case NVT_OPT_ECHO:
      /* Track echo state */
      if(action == NVT_DO) {
        vars->echo_local = TRUE;
        LOG(LOG_INFO, "Telnet: peer accepted ECHO");
      } else if(action == NVT_DONT) {
        vars->echo_local = FALSE;
      }
      resp = get_nvt_cmd_response(action, TRUE);
      break;
    case NVT_OPT_SUPPRESS_GO_AHEAD:
      /* Track SGA state */
      if(action == NVT_DO) {
        vars->sga_local = TRUE;
        LOG(LOG_INFO, "Telnet: peer accepted SGA (local)");
      } else if(action == NVT_WILL) {
        vars->sga_remote = TRUE;
        LOG(LOG_INFO, "Telnet: peer will SGA (remote)");
      }
      resp = get_nvt_cmd_response(action, TRUE);
      break;
    case NVT_OPT_NAWS:
      /* Accept NAWS — actual size arrives in subnegotiation */
      if(action == NVT_WILL) {
        vars->naws_enabled = TRUE;
        LOG(LOG_INFO, "Telnet: peer supports NAWS");
      }
      resp = get_nvt_cmd_response(action, TRUE);
      break;
    case NVT_OPT_TERMINAL_TYPE:
    case NVT_OPT_X_DISPLAY_LOCATION:
    case NVT_OPT_ENVIRON:
    case NVT_OPT_NEW_ENVIRON:
    case NVT_OPT_TERMINAL_SPEED:
      resp = get_nvt_cmd_response(action, TRUE);
      break;
    default:
      /* Refuse unknown options per RFC 854 */
      resp = get_nvt_cmd_response(action, FALSE);
      break;
  }
  send_nvt_command(fd, vars, resp, opt);
  return 0;
}

/*
 * Proactive telnet negotiation (RFC 854/857/858/1073/1143).
 *
 * Called immediately when we detect a telnet client (IAC as first byte).
 * Announces our capabilities so that well-behaved clients (Linux telnet,
 * PuTTY, SyncTERM, CGTerm, etc.) know what to expect:
 *
 *   WILL ECHO          - we handle echo (prevents double chars)
 *   WILL SGA           - we suppress go-ahead (character-at-a-time mode)
 *   DO   SGA           - request peer to suppress go-ahead too
 *   DO   NAWS          - request window size updates
 *   DO   TERMINAL_TYPE - request terminal type identification
 *   WILL TRANSMIT_BINARY / DO TRANSMIT_BINARY - 8-bit clean
 *
 * This batch is sent as a single write to minimize round-trips.
 */
void nvt_negotiate_proactive(int fd, nvt_vars *vars) {
  unsigned char batch[24];
  int n = 0;

  if(vars->negotiated) return;
  vars->negotiated = TRUE;

  LOG(LOG_INFO, "Telnet: sending proactive negotiation batch");

  /* WILL ECHO */
  batch[n++] = NVT_IAC; batch[n++] = NVT_WILL; batch[n++] = NVT_OPT_ECHO;
  /* WILL SGA */
  batch[n++] = NVT_IAC; batch[n++] = NVT_WILL; batch[n++] = NVT_OPT_SUPPRESS_GO_AHEAD;
  /* DO SGA */
  batch[n++] = NVT_IAC; batch[n++] = NVT_DO;   batch[n++] = NVT_OPT_SUPPRESS_GO_AHEAD;
  /* DO NAWS */
  batch[n++] = NVT_IAC; batch[n++] = NVT_DO;   batch[n++] = NVT_OPT_NAWS;
  /* DO TERMINAL_TYPE */
  batch[n++] = NVT_IAC; batch[n++] = NVT_DO;   batch[n++] = NVT_OPT_TERMINAL_TYPE;
  /* WILL BINARY */
  batch[n++] = NVT_IAC; batch[n++] = NVT_WILL; batch[n++] = NVT_OPT_TRANSMIT_BINARY;
  /* DO BINARY */
  batch[n++] = NVT_IAC; batch[n++] = NVT_DO;   batch[n++] = NVT_OPT_TRANSMIT_BINARY;

  ip_write(fd, batch, n);

  vars->echo_local = TRUE;
  vars->sga_local = TRUE;
  vars->term[NVT_OPT_ECHO] = NVT_WILL;
  vars->term[NVT_OPT_SUPPRESS_GO_AHEAD] = NVT_WILL;
  vars->term[NVT_OPT_NAWS] = NVT_DO;
  vars->term[NVT_OPT_TERMINAL_TYPE] = NVT_DO;
  vars->term[NVT_OPT_TRANSMIT_BINARY] = NVT_WILL;
}
