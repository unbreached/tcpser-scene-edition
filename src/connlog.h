#ifndef CONNLOG_H
#define CONNLOG_H 1

/* Connection log results */
#define CONNLOG_BLOCKED    "BLOCKED"
#define CONNLOG_RATE_LIMIT "RATE_LIMITED"
#define CONNLOG_GATE_FAIL  "GATE_FAIL"
#define CONNLOG_GATE_PASS  "GATE_PASS"
#define CONNLOG_CONNECTED  "CONNECTED"
#define CONNLOG_NO_ANSWER  "NO_ANSWER"
#define CONNLOG_HANGUP     "HANGUP"

void connlog_init(const char *path);
void connlog_set_path(const char *path);      /* remember only, no file I/O (check mode) */
const char *connlog_get_path(void);           /* NULL when disabled */
void connlog_event(const char *ip, const char *result);
void connlog_session_end(const char *ip, int duration, long bytes_in, long bytes_out);

#endif
