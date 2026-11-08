#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>       // for read...
#include <stdlib.h>
#include <string.h>       // for atoi...
#include <errno.h>
#include <netinet/tcp.h>

#include "debug.h"
#include "ip.h"
#include "phone_book.h"

const int BACK_LOG = 5;

int ip_init_server_conn(char *ip, int port) {
  int sSocket = 0, on = 0, rc = 0;
  struct sockaddr_in serverName = { 0 };

  LOG_ENTER();

  // Handle special case ":<port>"
  // Without it, ip_add will be set to port and port will be null

  /* parse "[addr]:port" on a private copy: the old strtok() wrote NULs into
   * the caller's string, truncating the address shown in the startup banner */
  char spec[128]; char *ip_addr = NULL;
  if(ip != NULL) {
    snprintf(spec, sizeof(spec), "%s", ip[0] == ':' ? ip + 1 : ip);
    char *colon = strchr(spec, ':');
    if(colon) {
      *colon = 0;
      if(colon[1]) port = atoi(colon + 1);
      if(spec[0]) ip_addr = spec;
    } else if(spec[0]) {
      port = atoi(spec);
    }
  }

  LOG(LOG_DEBUG, "Creating server socket");

  sSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (-1 == sSocket) {
    ELOG(LOG_FATAL, "Server socket could not be created");
  } else {

    /*
     * turn off bind address checking, and allow 
     * port numbers to be reused - otherwise
     * the TIME_WAIT phenomenon will prevent 
     * binding to these addresses.
     */

    on = 1;

    rc = setsockopt(sSocket, 
                    SOL_SOCKET, 
                    SO_REUSEADDR,
                    (const char *) &on, 
                    sizeof(on)
                   );
    if (-1 == rc) {
      ELOG(LOG_ERROR, "bind address checking could not be turned off");
    }

    if (ip_addr != NULL && strcmp(ip_addr,"") != 0) { /* changed to strcmp test -04Mar17 gwb */
      LOG(LOG_DEBUG, "Using specified ip address '%s'", ip_addr);
      serverName.sin_addr.s_addr = inet_addr(ip_addr);
    } else {
      serverName.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    serverName.sin_family = AF_INET;

    /* network-order */
    serverName.sin_port = htons(port);

    LOG(LOG_DEBUG, "Binding server socket to port %d", port);
    rc = bind(sSocket,
              (struct sockaddr *) &serverName,
              sizeof(serverName)
             );

    if (-1 == rc) {
      ELOG(LOG_FATAL, "Server socket could not be bound to port");
      close(sSocket);
      sSocket = -1;
    } else {
      LOG(LOG_INFO, "Server socket bound to port");

      rc = listen(sSocket, BACK_LOG);
      LOG(LOG_INFO, "Server socket listening for connections");
      if (-1 == rc) {
        ELOG(LOG_FATAL, "Server socket could not listen on port");
        close(sSocket);
        sSocket = -1;
      }
    }
  }
  LOG_EXIT();
  return sSocket;
}

int ip_connect(char *ip) {
  struct sockaddr_in pin;
  int sd = 0;
  int port = 23;
  char spec[PH_ENTRY_SIZE > 256 ? PH_ENTRY_SIZE : 256];
  char portstr[8];

  LOG_ENTER();
  /* "host[:port]" on a private copy (no strtok: it is not reentrant and it
   * mutates the caller's phone-book entry) */
  if(!ip) { LOG(LOG_ERROR, "No host address to connect to"); return -1; }
  snprintf(spec, sizeof(spec), "%s", ip);
  char *colon = strchr(spec, ':');
  if(colon) { *colon = 0; if(colon[1]) port = atoi(colon + 1); }
  if(!spec[0]) {
    /* the dial string "ATD:" or an empty phone-book entry */
    LOG(LOG_ERROR, "No host address to connect to");
    return -1;
  }
  if(port <= 0 || port > 65535) { LOG(LOG_ERROR, "Port out of range in %s", ip); return -1; }

  LOG(LOG_DEBUG, "Calling %s", ip);
  memset(&pin, 0, sizeof(pin));

  /* getaddrinfo: thread-safe (gethostbyname shares one static result between
   * threads) and handles dotted quads and names alike */
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;          /* the line/ip232 code is IPv4 */
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", port);
  int gai = getaddrinfo(spec, portstr, &hints, &res);
  if(gai != 0 || !res) {
    LOG(LOG_ERROR, "Host %s was invalid: %s", spec, gai_strerror(gai));
    return -1;
  }
  memcpy(&pin, res->ai_addr, sizeof(pin));
  freeaddrinfo(res);

  /* grab an Internet domain socket */
  if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    ELOG(LOG_ERROR, "could not create client socket");
    return -1;
  }

  /* connect to PORT on HOST */
  if (connect(sd, (struct sockaddr *)&pin, sizeof(pin)) == -1) {
    ELOG(LOG_ERROR, "could not connect to address");
    close(sd);
    return -1;
  }
  LOG(LOG_INFO, "Connection to %s established", ip);
  LOG_EXIT();
  return sd;
}

int ip_accept(int sSocket) {
  struct sockaddr_in clientName = { 0 };
  socklen_t clientLength = sizeof(clientName);
  int cSocket = -1;

  LOG_ENTER();
  (void) memset(&clientName, 0, sizeof(clientName));

  cSocket = accept(sSocket,
                   (struct sockaddr *)&clientName, 
                   &clientLength
                  );
  if (-1 == cSocket) {
    ELOG(LOG_ERROR, "Could not accept incoming connection");
    return -1;
  }

  if(-1 == getpeername(cSocket,
                        (struct sockaddr *)&clientName, 
                        &clientLength
                       )) {
    ELOG(LOG_WARN, "Could not obtain peer name");
  } else {
    LOG(LOG_INFO, 
        "Connection accepted from %s",
        inet_ntoa(clientName.sin_addr)
       );
  }
  LOG_EXIT();
  return cSocket;
}

int ip_accept_ex(int sSocket, char *peer_ip, int peer_ip_len) {
  struct sockaddr_in clientName = { 0 };
  socklen_t clientLength = sizeof(clientName);
  int cSocket = -1;

  LOG_ENTER();
  (void) memset(&clientName, 0, sizeof(clientName));

  cSocket = accept(sSocket,
                   (struct sockaddr *)&clientName,
                   &clientLength
                  );
  if (-1 == cSocket) {
    ELOG(LOG_ERROR, "Could not accept incoming connection");
    return -1;
  }

  /* Keepalive helps clean up dead sessions (NAT timeouts, scanners, etc.) */
  {
    int on = 1;
    (void)setsockopt(cSocket, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
    int idle = TCPSER_KEEPIDLE;
    (void)setsockopt(cSocket, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = TCPSER_KEEPINTVL;
    (void)setsockopt(cSocket, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = TCPSER_KEEPCNT;
    (void)setsockopt(cSocket, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
  }

  if(peer_ip && peer_ip_len > 0) {
    const char *saddr = inet_ntoa(clientName.sin_addr);
    snprintf(peer_ip, peer_ip_len, "%s", saddr ? saddr : "unknown");
  }

  LOG(LOG_INFO,
      "Connection accepted from %s",
      peer_ip && peer_ip[0] ? peer_ip : inet_ntoa(clientName.sin_addr)
     );

  LOG_EXIT();
  return cSocket;
}

int ip_disconnect(int fd) {
  if(fd > -1)
    close(fd);
  return 0;
}

int ip_write(int fd, unsigned char *data, int len) {
  log_trace(TRACE_IP_OUT, data, len);
  int total = 0;
  while(total < len) {
    int rc = (int)write(fd, data + total, len - total);
    if(rc > 0) {
      total += rc;
      continue;
    }
    if(rc == 0) {
      break;
    }
    if(errno == EINTR) continue;
    return -1;
  }
  return total;
}

int ip_read(int fd, unsigned char *data, int len) {
  int res;

  res = recv(fd, data, len, 0);
  if(0 < res)
  	log_trace(TRACE_IP_IN, data, res);
  return res;
}
