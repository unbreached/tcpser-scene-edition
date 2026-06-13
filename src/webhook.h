#ifndef WEBHOOK_H
#define WEBHOOK_H 1

void webhook_init(const char *url);
void webhook_notify(const char *event, const char *ip);

#endif
