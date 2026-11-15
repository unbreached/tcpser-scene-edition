#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include "debug.h"
#include "modem_core.h"
#include "phone_book.h"
#include "ip.h"
#include "bridge.h"
#include "line.h"


void reset_config(line_config *cfg) {
  pthread_mutex_lock(&cfg->lock);
  cfg->fd = -1;
  cfg->is_telnet = FALSE;
  cfg->is_data_received = FALSE;
  cfg->is_connected = FALSE;
  cfg->peer_ip[0] = 0;
  nvt_init_config(&cfg->nvt_data);
  cfg->gen++;
  pthread_mutex_unlock(&cfg->lock);
}

void line_init_config(line_config *cfg) {
  pthread_mutex_init(&cfg->lock, NULL);
  reset_config(cfg);
}

int line_read(line_config *cfg, unsigned char *data, int len) {
  return ip_read(cfg->fd, data, len);
}

int line_filter(line_config *cfg, dce_config *dce, unsigned gen, int online,
                const unsigned char *in, int len, unsigned char *out) {
  int n = 0;

  pthread_mutex_lock(&cfg->lock);
  if(cfg->gen != gen || cfg->fd < 0) {
    pthread_mutex_unlock(&cfg->lock);
    return -1;
  }
  int detect = (cfg->is_data_received == FALSE);
  if(online && detect && len > 0 && in[0] == 0x1a && !cfg->is_telnet) {
    /* legacy: some terminals open with ^Z instead of an IAC */
    LOG(LOG_INFO, "Detected telnet client");
    cfg->is_telnet = TRUE;
    nvt_negotiate_proactive(dce, cfg->fd, &cfg->nvt_data);
  }
  n = nvt_filter(dce, cfg->fd, &cfg->nvt_data, &cfg->is_telnet, detect, in, len, out);
  if(online && detect) {
    /* The first chunk of the established call decides: a peer that has not
     * spoken telnet by now is raw, and 0xFF from it is data from here on
     * (binary transfers from non-telnet terminals). */
    cfg->is_data_received = TRUE;
    n += nvt_flush_pending(&cfg->nvt_data, cfg->is_telnet, out + n);
  }
  pthread_mutex_unlock(&cfg->lock);
  return n;
}

/* Returns bytes written, or -1 if the peer is gone or has stopped reading
 * (send timeout): the caller should hang up. */
int line_write(line_config *cfg, unsigned char* data, int len) {
  int retval;
  int i = 0;
  int double_iac = FALSE;
  unsigned char text[1024];
  int text_len = 0;
  int mask = 0x7f;
  int is_telnet, binary;
  int fd;

  pthread_mutex_lock(&cfg->lock);
  is_telnet = cfg->is_telnet;
  binary = cfg->nvt_data.binary_xmit;
  fd = cfg->fd;
  pthread_mutex_unlock(&cfg->lock);
  if(fd < 0) return -1;

  if(is_telnet) {
    if(binary) {
      mask = 0xff;
    }
    retval = 0;
    while(i < len) {
      if (double_iac) {
        text[text_len++] = NVT_IAC;
        double_iac = FALSE;
        i++;
      } else {
        if(NVT_IAC == data[i]) {
          text[text_len++] = NVT_IAC;
          double_iac = TRUE;
        } else {
          text[text_len++] = data[i++] & mask;
        }
      }
      if(text_len == sizeof(text)) {
        retval = ip_write(fd, text, text_len);
        text_len = 0;
        if(retval < 0) return -1;
      }
    }
    if(text_len) {
      retval = ip_write(fd, text, text_len);
    }
    return retval;
  }

  return ip_write(fd, data, len);
}

int line_listen(line_config *cfg) {
  return 0;
}

int line_accept(line_config *cfg) {
  char peer[INET_ADDRSTRLEN];
  int fd = ip_accept_ex(cfg->sfd, peer, sizeof(peer));
  if(fd > -1) {
    LOG(LOG_ALL, "Connection accepted");
    pthread_mutex_lock(&cfg->lock);
    nvt_init_config(&cfg->nvt_data);
    cfg->nvt_data.role = NVT_ROLE_SERVER;
    cfg->is_telnet = FALSE;
    cfg->is_data_received = FALSE;
    snprintf(cfg->peer_ip, sizeof(cfg->peer_ip), "%s", peer);
    cfg->fd = fd;
    cfg->gen++;
    cfg->is_connected = TRUE;
    pthread_mutex_unlock(&cfg->lock);
    return 0;
  }
  LOG(LOG_ALL, "Could not accept connection");
  return -1;
}

int line_off_hook(line_config *cfg) {
  return 0;
}

void line_attach(line_config *cfg, int fd) {
  pthread_mutex_lock(&cfg->lock);
  nvt_init_config(&cfg->nvt_data);
  cfg->nvt_data.role = NVT_ROLE_CLIENT;
  cfg->is_telnet = FALSE;
  cfg->is_data_received = FALSE;
  cfg->peer_ip[0] = 0;
  cfg->fd = fd;
  cfg->gen++;
  cfg->is_connected = TRUE;
  pthread_mutex_unlock(&cfg->lock);
}

int line_connect(line_config *cfg, char *number) {
  char address[PH_ENTRY_SIZE];
  int fd;

  LOG(LOG_INFO, "Connecting line");
  pb_search(number, address);
  fd = ip_connect(address);
  if(fd > -1) {
    LOG(LOG_ALL, "Connected to %s", address);
    line_attach(cfg, fd);
    return 0;
  } else {
    LOG(LOG_ALL, "Could not connect to %s",address);
    return -1;
  }
}

void line_abort(line_config *cfg) {
  int fd = cfg->fd;
  if(cfg->is_connected == TRUE && fd >= 0) shutdown(fd, SHUT_RDWR);
}

int line_disconnect(line_config *cfg) {
  LOG(LOG_INFO, "Disconnecting line");
  /* Order matters (audit STAB-M4): clear the shared state FIRST so ip_thread
   * stops treating the descriptor as ours, then shutdown() to wake a reader
   * blocked on it, then close. Closing first left a window where the number
   * was reused by the next accept() while ip_thread still read from it. */
  int fd = cfg->fd;
  int was = cfg->is_connected;
  cfg->is_connected = FALSE;
  cfg->fd = -1;
  if(was == TRUE && fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    ip_disconnect(fd);
  }
  reset_config(cfg);
  return 0;
}
