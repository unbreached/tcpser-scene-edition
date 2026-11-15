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
/* A write to a peer that has stopped reading gives up after this many seconds
 * (the line is then hung up) instead of blocking the line forever. */
#ifndef TCPSER_SEND_TIMEOUT
#define TCPSER_SEND_TIMEOUT 20
#endif

int ip_init(void);
int ip_init_server_conn(char *ip, int port);
int ip_connect(char *ip);
/* Why an outgoing call failed (ip_connect_ex) */
enum { IP_ERR_NONE = 0, IP_ERR_DNS, IP_ERR_REFUSED, IP_ERR_TIMEOUT, IP_ERR_ABORTED, IP_ERR_OTHER };
/* Connect with a timeout. aborted(arg), if given, is polled a few times a
 * second and cancels the attempt when it returns non-zero. */
int ip_connect_ex(char *ip, int timeout_s, int (*aborted)(void *), void *arg, int *why);
void ip_tune_socket(int fd);
int ip_accept(int sSocket);
int ip_accept_ex(int sSocket, char *peer_ip, int peer_ip_len);
int ip_disconnect(int fd);
int ip_write(int fd, unsigned char *data, int len);
int ip_read(int fd, unsigned char *data, int len);

#endif
