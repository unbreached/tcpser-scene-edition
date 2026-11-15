#include <string.h>
#include <stdio.h>

#include "debug.h"
#include "ip.h"
#include "modem_core.h"

#include "nvt.h"

/* Telnet (RFC 854) for the caller side of the line.
 *
 * One state machine handles every byte that arrives from the network, with
 * its state in nvt_vars, so sequences split across reads are parsed correctly
 * (the old parser dropped or leaked bytes when an IAC straddled two reads,
 * which corrupted binary uploads). Option negotiation keeps per-option state
 * (RFC 1143) and is role-aware: we offer ECHO/NAWS/TTYPE only to callers, not
 * to the BBS the C64 dialled. */

void nvt_init_config(nvt_vars *vars) {
  memset(vars, 0, sizeof(*vars));
  vars->cols = 80;
  vars->rows = 25;
  vars->role = NVT_ROLE_SERVER;
  vars->st = NVT_ST_DATA;
}

static const char *action_name(int action) {
  switch(action) {
    case NVT_WILL: return "WILL";
    case NVT_WONT: return "WONT";
    case NVT_DO:   return "DO";
    case NVT_DONT: return "DONT";
  }
  return "?";
}

static void send_cmd(int fd, int action, int opt) {
  unsigned char cmd[3];
  LOG(LOG_DEBUG, "Sending NVT command: %s %d", action_name(action), opt);
  cmd[0] = NVT_IAC;
  cmd[1] = (unsigned char)action;
  cmd[2] = (unsigned char)opt;
  ip_write(fd, cmd, 3);
}

static void sync_flags(nvt_vars *v) {
  v->binary_xmit = v->us[NVT_OPT_TRANSMIT_BINARY] == NVT_Q_YES;
  v->binary_recv = v->him[NVT_OPT_TRANSMIT_BINARY] == NVT_Q_YES;
  v->echo_local = v->us[NVT_OPT_ECHO] == NVT_Q_YES;
  v->sga_local = v->us[NVT_OPT_SUPPRESS_GO_AHEAD] == NVT_Q_YES;
  v->sga_remote = v->him[NVT_OPT_SUPPRESS_GO_AHEAD] == NVT_Q_YES;
  v->naws_enabled = v->him[NVT_OPT_NAWS] == NVT_Q_YES;
}

/* May WE enable this option when the peer says DO? */
static int us_ok(dce_config *cfg, nvt_vars *v, int opt) {
  switch(opt) {
    case NVT_OPT_TRANSMIT_BINARY:   return dce_get_parity(cfg) == 0;
    case NVT_OPT_SUPPRESS_GO_AHEAD: return TRUE;
    case NVT_OPT_ECHO:              return v->role == NVT_ROLE_SERVER;
    case NVT_OPT_TERMINAL_TYPE:
    case NVT_OPT_TERMINAL_SPEED:    return v->role == NVT_ROLE_CLIENT;
  }
  return FALSE;
}

/* May the PEER enable this option when it says WILL? */
static int him_ok(dce_config *cfg, nvt_vars *v, int opt) {
  switch(opt) {
    case NVT_OPT_TRANSMIT_BINARY:   return dce_get_parity(cfg) == 0;
    case NVT_OPT_SUPPRESS_GO_AHEAD: return TRUE;
    case NVT_OPT_ECHO:              return v->role == NVT_ROLE_CLIENT;
    case NVT_OPT_NAWS:
    case NVT_OPT_TERMINAL_TYPE:     return v->role == NVT_ROLE_SERVER;
  }
  return FALSE;
}

static void handle_option(dce_config *cfg, int fd, nvt_vars *v, int action, int opt) {
  LOG(LOG_DEBUG, "Received NVT command: %s %d", action_name(action), opt);
  switch(action) {
    case NVT_WILL:
      if(v->him[opt] == NVT_Q_YES) break;                       /* no change: no reply */
      if(v->him[opt] == NVT_Q_WANTYES) { v->him[opt] = NVT_Q_YES; }
      else if(him_ok(cfg, v, opt)) { v->him[opt] = NVT_Q_YES; send_cmd(fd, NVT_DO, opt); }
      else { send_cmd(fd, NVT_DONT, opt); break; }
      if(opt == NVT_OPT_TERMINAL_TYPE) {
        unsigned char ask[6] = { NVT_IAC, NVT_SB, NVT_OPT_TERMINAL_TYPE, NVT_SB_SEND, NVT_IAC, NVT_SE };
        ip_write(fd, ask, sizeof(ask));
      }
      break;
    case NVT_WONT:
      if(v->him[opt] == NVT_Q_YES) send_cmd(fd, NVT_DONT, opt);
      v->him[opt] = NVT_Q_NO;
      break;
    case NVT_DO:
      if(v->us[opt] == NVT_Q_YES) break;
      if(v->us[opt] == NVT_Q_WANTYES) { v->us[opt] = NVT_Q_YES; }
      else if(us_ok(cfg, v, opt)) { v->us[opt] = NVT_Q_YES; send_cmd(fd, NVT_WILL, opt); }
      else send_cmd(fd, NVT_WONT, opt);
      break;
    case NVT_DONT:
      if(v->us[opt] == NVT_Q_YES) send_cmd(fd, NVT_WONT, opt);
      v->us[opt] = NVT_Q_NO;
      break;
  }
  sync_flags(v);
}

static void handle_subneg(dce_config *cfg, int fd, nvt_vars *v) {
  unsigned char resp[96];
  int n = 0;
  int opt;

  if(v->sb_len < 1) return;
  opt = v->sb[0];

  if(opt == NVT_OPT_NAWS && v->sb_len >= 5) {
    int cols = (v->sb[1] << 8) | v->sb[2];
    int rows = (v->sb[3] << 8) | v->sb[4];
    if(cols > 0 && cols < 500) v->cols = cols;
    if(rows > 0 && rows < 500) v->rows = rows;
    LOG(LOG_INFO, "NAWS: terminal size %dx%d", v->cols, v->rows);
    return;
  }
  if(opt == NVT_OPT_TERMINAL_TYPE && v->sb_len >= 2 && v->sb[1] == NVT_SB_IS) {
    int k = 0;
    for(int i = 2; i < v->sb_len && k < (int)sizeof(v->ttype) - 1; i++) {
      unsigned char c = v->sb[i];
      v->ttype[k++] = (c >= 0x21 && c < 0x7f) ? (char)c : '?';   /* printable only: it goes to the log */
    }
    v->ttype[k] = 0;
    LOG(LOG_INFO, "Telnet: caller terminal type %s", v->ttype);
    return;
  }
  if(v->sb_len >= 2 && v->sb[1] == NVT_SB_SEND) {
    char text[40];
    text[0] = 0;
    if(opt == NVT_OPT_TERMINAL_TYPE && v->us[opt] == NVT_Q_YES) {
      snprintf(text, sizeof(text), "VT100");
      LOG(LOG_INFO, "Telnet: reporting terminal type %s", text);
    } else if(opt == NVT_OPT_TERMINAL_SPEED && v->us[opt] == NVT_Q_YES) {
      snprintf(text, sizeof(text), "%i,%i", cfg->port_speed, cfg->port_speed);
      LOG(LOG_INFO, "Telnet: reporting speed %s", text);
    }
    if(text[0]) {
      size_t tl = strlen(text);
      resp[n++] = NVT_IAC; resp[n++] = NVT_SB; resp[n++] = (unsigned char)opt; resp[n++] = NVT_SB_IS;
      memcpy(resp + n, text, tl); n += (int)tl;
      resp[n++] = NVT_IAC; resp[n++] = NVT_SE;
      ip_write(fd, resp, n);
    }
  }
}

/*
 * Opening negotiation, sent once when a telnet peer is recognised.
 *
 * To a caller (we are the server):
 *   WILL ECHO, WILL SGA, DO SGA    character-at-a-time, no double echo
 *   DO NAWS, DO TERMINAL_TYPE      window size and terminal name
 *   WILL BINARY, DO BINARY         8-bit clean (PETSCII, file transfers)
 * To a BBS the C64 dialled (we are the client): SGA and BINARY only; ECHO,
 * NAWS and TTYPE are the server's to ask for.
 * BINARY is not offered while the C64 side is known to use parity (7 bits).
 */
void nvt_negotiate_proactive(dce_config *cfg, int fd, nvt_vars *v) {
  unsigned char batch[32];
  int n = 0;

  if(v->negotiated) return;
  v->negotiated = TRUE;

  LOG(LOG_INFO, "Telnet: sending opening negotiation (%s role)", v->role == NVT_ROLE_SERVER ? "server" : "client");
#define OFFER(action, opt) do { \
    batch[n++] = NVT_IAC; batch[n++] = (action); batch[n++] = (opt); \
    if((action) == NVT_WILL) { if(v->us[opt] == NVT_Q_NO) v->us[opt] = NVT_Q_WANTYES; } \
    else { if(v->him[opt] == NVT_Q_NO) v->him[opt] = NVT_Q_WANTYES; } \
  } while(0)

  if(v->role == NVT_ROLE_SERVER) OFFER(NVT_WILL, NVT_OPT_ECHO);
  OFFER(NVT_WILL, NVT_OPT_SUPPRESS_GO_AHEAD);
  OFFER(NVT_DO, NVT_OPT_SUPPRESS_GO_AHEAD);
  if(v->role == NVT_ROLE_SERVER) {
    OFFER(NVT_DO, NVT_OPT_NAWS);
    OFFER(NVT_DO, NVT_OPT_TERMINAL_TYPE);
  }
  if(dce_get_parity(cfg) == 0) {
    OFFER(NVT_WILL, NVT_OPT_TRANSMIT_BINARY);
    OFFER(NVT_DO, NVT_OPT_TRANSMIT_BINARY);
  }
#undef OFFER
  ip_write(fd, batch, n);
}

int nvt_flush_pending(nvt_vars *v, int is_telnet, unsigned char *out) {
  if(!is_telnet && v->st == NVT_ST_IAC) {
    v->st = NVT_ST_DATA;
    out[0] = NVT_IAC;
    return 1;
  }
  return 0;
}

int nvt_filter(dce_config *cfg, int fd, nvt_vars *v, int *is_telnet, int detect,
               const unsigned char *in, int len, unsigned char *out) {
  int o = 0;

  for(int i = 0; i < len; i++) {
    unsigned char ch = in[i];
    switch(v->st) {
      case NVT_ST_DATA:
      data_byte:
        if(ch == NVT_IAC && (*is_telnet || detect)) {
          v->st = NVT_ST_IAC;
          break;
        }
        if(*is_telnet && !v->binary_recv) {
          /* NVT line ending: CR NUL is a bare CR */
          if(v->last_cr && ch == 0) { v->last_cr = FALSE; break; }
          v->last_cr = (ch == '\r');
        }
        out[o++] = ch;
        break;
      case NVT_ST_IAC:
        if(!*is_telnet) {
          /* first 0xFF from an unknown peer: telnet only if a command follows */
          if(ch >= NVT_SB && ch <= NVT_DONT) {
            LOG(LOG_INFO, "Detected telnet client");
            *is_telnet = TRUE;
            nvt_negotiate_proactive(cfg, fd, v);
          } else {
            out[o++] = NVT_IAC;       /* it was data */
            v->st = NVT_ST_DATA;
            goto data_byte;
          }
        }
        if(ch == NVT_IAC) { out[o++] = NVT_IAC; v->last_cr = FALSE; v->st = NVT_ST_DATA; }
        else if(ch >= NVT_WILL && ch <= NVT_DONT) { v->cmd = ch; v->st = NVT_ST_OPT; }
        else if(ch == NVT_SB) { v->sb_len = 0; v->st = NVT_ST_SB; }
        else v->st = NVT_ST_DATA;     /* NOP, DM, BRK, AYT, GA ...: nothing to do */
        break;
      case NVT_ST_OPT:
        handle_option(cfg, fd, v, v->cmd, ch);
        v->st = NVT_ST_DATA;
        break;
      case NVT_ST_SB:
        if(ch == NVT_IAC) v->st = NVT_ST_SB_IAC;
        else if(v->sb_len < (int)sizeof(v->sb)) v->sb[v->sb_len++] = ch;
        break;
      case NVT_ST_SB_IAC:
        if(ch == NVT_SE) { handle_subneg(cfg, fd, v); v->st = NVT_ST_DATA; }
        else if(ch == NVT_IAC) { if(v->sb_len < (int)sizeof(v->sb)) v->sb[v->sb_len++] = NVT_IAC; v->st = NVT_ST_SB; }
        else v->st = NVT_ST_DATA;     /* malformed: drop the subnegotiation */
        break;
    }
  }
  return o;
}
