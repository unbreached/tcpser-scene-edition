#ifndef IP_H
#define IP_H

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* TCP keepalive defaults (override at compile time with -D) */
#ifndef TCPSER_KEEPIDLE
#define TCPSER_KEEPIDLE   60    /* seconds before first probe */
#endif
#ifndef TCPSER_KEEPINTVL
#define TCPSER_KEEPINTVL  30    /* seconds between probes */
#endif
#ifndef TCPSER_KEEPCNT
#define TCPSER_KEEPCNT    3     /* probes before giving up */
#endif

int ip_init(void);
int ip_init_server_conn(char *ip, int port);
int ip_connect(char *ip);
int ip_accept(int sSocket);
int ip_accept_ex(int sSocket, char *peer_ip, int peer_ip_len);
int ip_disconnect(int fd);
int ip_write(int fd, unsigned char *data, int len);
int ip_read(int fd, unsigned char *data, int len);

#endif
