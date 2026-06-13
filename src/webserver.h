#ifndef WEBSERVER_H
#define WEBSERVER_H

void webserver_init(int port);
void webserver_start(void);
void webserver_stop(void);
int webserver_is_running(void);
int webserver_get_port(void);
const char *webserver_get_bind(void);
int webserver_has_token(void);

#endif