#ifndef WEBSERVER_H
#define WEBSERVER_H

void webserver_init(int port);
void webserver_read_env(void);  /* apply TCPSER_DASH_* after all options are parsed */
int  webserver_start(void);   /* 0 = listening, -1 = failed (already logged) */
void webserver_stop(void);
int webserver_is_running(void);
int webserver_get_port(void);
const char *webserver_get_bind(void);
int webserver_has_token(void);

#endif