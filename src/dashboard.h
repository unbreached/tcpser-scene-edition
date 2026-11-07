#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <time.h>

#define DASH_TRAFFIC_LINES 16
#define DASH_HISTORY_LINES 12
#define DASH_MAX_LINES     8     /* modem lines tracked individually */
#define DASH_EVENTS        16    /* system events ring (web "Events" panel) */

/* One modem line (one -d/-v) as the dashboards see it. */
typedef struct {
  int    inited;
  char   device[64];
  int    speed;
  int    connected;             /* caller online on this line */
  char   peer_ip[64];
  time_t session_start;
  unsigned long bytes_in;       /* this session */
  unsigned long bytes_out;
  int    dtr;                   /* BBS DTR as seen on the serial port (1 = asserted) */
  void  *cfg;                   /* modem_config*, for web/TUI actions */
} dash_line_t;

typedef struct {
  time_t when;
  char   text[80];
} dash_event_t;

typedef struct {
  char dir[16];
  unsigned char raw[96];  /* raw bytes for hex display */
  int  rawlen;
  int  bytes;             /* total bytes in original packet */
  int  type;
} dash_traffic_line_t;

typedef struct {
  char   ip[64];
  time_t connect_time;
  time_t disconnect_time;
  int    duration;
  int    result;        /* 0=blocked, 1=gate_fail, 2=gate_pass, 3=connected */
  int    bytes_in;
  int    bytes_out;
} dash_history_entry_t;

typedef struct {
  char tty[256];
  int  speed;
  int  port;
  int  modem_count;
  int  serial_fd;       /* serial port fd (unused now, kept for compat) */
  void *modem_cfg;      /* pointer to modem_config for direct control */

  time_t start_time;
  int    connected;
  char   peer_ip[64];
  time_t session_start;
  int    dcd_on;
  int    dsr_on;

  unsigned long total_connections;
  unsigned long blocked;
  unsigned long gate_pass;
  unsigned long gate_fail;
  unsigned long bytes_in;
  unsigned long bytes_out;
  unsigned long session_bytes_in;
  unsigned long session_bytes_out;

  /* Activity LED timestamps (for blinking indicators) */
  time_t last_caller_rx;        /* last data from caller (TCP in) */
  time_t last_caller_tx;        /* last data to caller (TCP out) */
  time_t last_bbs_rx;           /* last data from BBS (serial in) */
  time_t last_bbs_tx;           /* last data to BBS (serial out) */

  dash_traffic_line_t traffic[DASH_TRAFFIC_LINES];
  int traffic_head;
  int traffic_count;

  dash_history_entry_t history[DASH_HISTORY_LINES];
  int history_head;
  int history_count;

  dash_line_t  lines[DASH_MAX_LINES];
  dash_event_t events[DASH_EVENTS];
  int events_head;
  int events_count;

  /* Activity time series for the web charts. Buckets are indexed by wall-clock
   * position (hour of day, minute of hour) and carry the stamp of the period
   * they belong to, so stale buckets read as zero without a sweeper. */
  unsigned hour_calls[24];      /* accepted connections per hour */
  unsigned hour_fail[24];       /* gate failures + drops per hour */
  long     hour_stamp[24];      /* time/3600 of the period the bucket holds */
  unsigned long min_in[60];     /* bytes caller->BBS per minute */
  unsigned long min_out[60];    /* bytes BBS->caller per minute */
  long     min_stamp[60];       /* time/60 of the period the bucket holds */

  int enabled;
} dashboard_state_t;

extern dashboard_state_t g_dash;

void dashboard_init(const char *tty, int speed, int port, int modem_count);
void dashboard_init_headless(const char *tty, int speed, int port, int modem_count);
int  dashboard_capture_active(void);
void dashboard_set_serial_fd(int fd);
void dashboard_set_modem_cfg(void *cfg);
void dashboard_lock(void);
void dashboard_unlock(void);
void dashboard_event(int type, unsigned char *data, int len);
/* per-line state (idx = modem_config.idx) */
void dashboard_line_init(int idx, const char *device, int speed, void *cfg);
void dashboard_line_state(int idx, int connected, const char *peer_ip);
void dashboard_line_closed(int idx);          /* records the history entry */
void dashboard_line_dtr(int idx, int dtr);
/* system event for the Events panel (printf-style, <= 79 chars kept) */
void dashboard_note(const char *fmt, ...);
void dashboard_stat_connection(const char *ip);
void dashboard_stat_blocked(const char *ip);
void dashboard_stat_gate(int passed, const char *ip);
void dashboard_connection_closed(void);
void dashboard_render(void);
int  dashboard_check_winch(void);
void dashboard_check_keyboard(void);
int  dashboard_quit_requested(void);   /* 'q' pressed: main loop exits via the graceful path */
void dashboard_save_stats(const char *path);
void dashboard_load_stats(const char *path);
void dashboard_set_stats_path(const char *path);
const char *dashboard_get_stats_path(void);
void dashboard_autosave(void);

#endif
