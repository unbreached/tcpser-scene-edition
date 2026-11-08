#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <sys/param.h>
#include <pthread.h>

#include "bridge.h"
#include "debug.h"
#include "init.h"
#include "version.h"
#include "ip.h"
#include "modem_core.h"
#include "phone_book.h"
#include "util.h"
#include "analyzer.h"
#include "dashboard.h"
#include "webserver.h"

const char MDM_BUSY[] = "BUSY\n";

#define MAX_MODEMS 16

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_sighup = 0;
static volatile sig_atomic_t g_sigusr1 = 0;
static time_t g_start_time = 0;

static void handle_shutdown(int sig) {
  (void)sig;
  g_running = 0;
}

static void handle_sighup(int sig) {
  (void)sig;
  g_sighup = 1;
}

static void handle_sigusr1(int sig) {
  (void)sig;
  g_sigusr1 = 1;
}

/* sigaction instead of signal(): defined semantics everywhere, and NO
 * SA_RESTART so the main select() wakes promptly on SIGTERM/HUP/USR1. Worker
 * threads block these signals (spawn_thread), so their blocking I/O is never
 * interrupted and the gate cannot mistake EINTR for a timeout. */
static void install_handler(int sig, void (*fn)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
}

static void dump_stats(int modem_count) {
  time_t now = time(NULL);
  long uptime = (long)(now - g_start_time);
  long days = uptime / 86400;
  long hours = (uptime % 86400) / 3600;
  long mins = (uptime % 3600) / 60;
  /* the counters live in the dashboard state (the old file-local copies here
   * were never incremented, so SIGUSR1 always reported zeros) */
  unsigned long conns = 0, blocked = 0, pass = 0, fail = 0;
  dashboard_counters(&conns, &blocked, &pass, &fail);

  fprintf(stderr, "\n");
  fprintf(stderr, "  TCPSER %s - Statistics\n", VERSION);
  fprintf(stderr, "  ================================\n");
  fprintf(stderr, "  Uptime:       %ldd %ldh %ldm\n", days, hours, mins);
  fprintf(stderr, "  Modems:       %d configured\n", modem_count);
  fprintf(stderr, "  Connections:  %lu total\n", conns);
  fprintf(stderr, "  Blocked:      %lu (IP blocklist / rate limit)\n", blocked);
  fprintf(stderr, "  Gate passed:  %lu\n", pass);
  fprintf(stderr, "  Gate failed:  %lu\n", fail);
  fprintf(stderr, "  ================================\n\n");
}

int main(int argc, char *argv[]) {
  modem_config cfg[MAX_MODEMS];
  int modem_count;
  int analyzer_mode = FALSE;
  port_config ports[MAX_PORTS];
  int port_count = 0;

  char all_busy[255] = {0};   /* read via strlen() later; was uninitialised when -B absent */
  int i;
  int rc = 0;
  fd_set readfs;
  int max_fd = 0;
  int accept_pending = FALSE;
  time_t accept_pending_since = 0;
  unsigned char buf[255];
  int cSocket;

  log_init();

  LOG_ENTER();

  log_set_level(LOG_FATAL);

  mdm_init();

  pb_init();
  
  install_handler(SIGIO, SIG_IGN);    /* Some Linux variant term on SIGIO by default */
  install_handler(SIGPIPE, SIG_IGN);  /* Avoid process termination on broken pipes */
  install_handler(SIGCHLD, SIG_IGN);  /* Auto-reap spawned children (gate scripts, curl) */
  install_handler(SIGTERM, handle_shutdown);
  install_handler(SIGINT, handle_shutdown);
  install_handler(SIGHUP, handle_sighup);   /* Reopen log file (for logrotate) */
  install_handler(SIGUSR1, handle_sigusr1); /* Dump statistics */

  g_start_time = time(NULL);

  modem_count = init(argc, argv, cfg, MAX_MODEMS, ports, &port_count, all_busy, sizeof(all_busy), &analyzer_mode);

  if(analyzer_mode) {
    if(modem_count < 1 || cfg[0].dce_data.tty[0] == '\0') {
      fprintf(stderr, "Analyzer mode requires -d <serial device> (and optionally -s <speed>)\n");
      fprintf(stderr, "Example: %s -d /dev/ttyUSB0 -s 2400 -Z\n", argv[0]);
      exit(1);
    }
    exit(analyzer_run(cfg[0].dce_data.tty, cfg[0].dce_data.port_speed));
  }
  /* Create server sockets for all configured ports */
  for(i = 0; i < port_count; i++) {
    ports[i].socket = ip_init_server_conn(ports[i].addr, ports[i].port);
    if(-1 == ports[i].socket) {
      ELOG(LOG_FATAL, "Could not listen on %s", ports[i].addr);
      exit (-1);
    }
  }

  print_config_summary(stderr, cfg, modem_count, ports, port_count);

  /* Initialize dashboard if -M flag is set */
  if(log_get_trace_flags() & TRACE_MONITOR) {
    int listen_port = port_count > 0 ? ports[0].port : 6400;
    /* Suppress log output to avoid bleeding into dashboard.
     * If no explicit -L logfile was set, redirect to /dev/null. */
    if(!log_get_filename()) {
      FILE *devnull = fopen("/dev/null", "w");
      if(devnull) log_set_file(devnull);
    }
    dashboard_init(
      cfg[0].dce_data.is_ip232 ? "(ip232)" : cfg[0].dce_data.tty,
      cfg[0].dce_data.port_speed,
      listen_port,
      modem_count
    );
    {
      const char *sp = dashboard_get_stats_path();
      if(sp) dashboard_load_stats(sp);
    }
  }

  /* Start web dashboard if -H flag is set */
  if(webserver_get_port() > 0) {
    /* The web dashboard reads live stats + traffic from g_dash. If the terminal
     * dashboard (-M) didn't already initialise it, bring it up headless so the
     * web view isn't empty (this also enables traffic capture). */
    if(!(log_get_trace_flags() & TRACE_MONITOR)) {
      int listen_port = port_count > 0 ? ports[0].port : 6400;
      dashboard_init_headless(
        cfg[0].dce_data.is_ip232 ? "(ip232)" : cfg[0].dce_data.tty,
        cfg[0].dce_data.port_speed, listen_port, modem_count);
      const char *sp = dashboard_get_stats_path();
      if(sp) dashboard_load_stats(sp);
    }
    if(webserver_start() != 0)
      fprintf(stderr, "  Web:     NOT listening on %s:%d (see log)\n", webserver_get_bind(), webserver_get_port());
  }

  for(i = 0; i < modem_count; i++) {
    LOG(LOG_INFO, "Creating modem #%d", i);
    if(-1 == pipe(cfg[i].mp[0])) {
      ELOG(LOG_FATAL, "Bridge task incoming IPC pipe could not be created");
      exit(-1);
    }
    if(-1 == pipe(cfg[i].mp[1])) {
      ELOG(LOG_FATAL, "Bridge task outgoing IPC pipe could not be created");
      exit(-1);
    }
    cfg[i].line_data.sfd = -1;  /* Will be set when connection accepted */

    spawn_thread(*bridge_task, (void *)&cfg[i], "BRIDGE");

  }

  while(g_running && !dashboard_quit_requested()) {
    FD_ZERO(&readfs);
    max_fd = 0;
    for(i = 0; i < modem_count; i++) {
      FD_SET(cfg[i].mp[0][0], &readfs);
      max_fd=MAX(max_fd, cfg[i].mp[0][0]);
    }
    if(accept_pending == FALSE) {
      for(i = 0; i < port_count; i++) {
        max_fd=MAX(max_fd, ports[i].socket);
        FD_SET(ports[i].socket, &readfs);
      }
    }
    LOG(LOG_ALL, "Waiting for incoming connections and/or indicators");
    {
      struct timeval tv;
      int tui = (log_get_trace_flags() & TRACE_MONITOR) != 0;
      tv.tv_sec = tui ? 0 : 2;          /* the diffed TUI is cheap to refresh: 4x/s for a live clock */
      tv.tv_usec = tui ? 250000 : 0;
      rc = select(max_fd+1, &readfs, NULL, NULL, &tv);
      /* Always check keyboard, resize and signals, even during active traffic */
      if(g_sighup) { g_sighup = 0; log_reopen(); LOG(LOG_INFO, "Log file reopened (SIGHUP)"); }
      /* A modem that never answers MSG_CALLING (e.g. stuck in a serial
       * reconnect loop) must not freeze every port forever (audit STAB-M8). */
      if(accept_pending && time(NULL) - accept_pending_since > 5) {
        LOG(LOG_WARN, "Routed call was not accepted within 5 s; releasing the accept gate");
        accept_pending = FALSE;
      }
      if(g_sigusr1) { g_sigusr1 = 0; dump_stats(modem_count); }
      dashboard_check_winch();    /* repaint on terminal resize (was only on idle ticks) */
      dashboard_check_keyboard(); /* process interactive keys */
      if(rc == 0) {
        dashboard_render();   /* periodic refresh (clock, durations) */
        dashboard_autosave(); /* save stats every 60s */
        continue;
      }
      if(rc < 0) continue;  /* interrupted by signal */
    }
    for(i = 0; i < modem_count; i++) {
      if (FD_ISSET(cfg[i].mp[0][0], &readfs)) {  // child pipe
        rc = read(cfg[i].mp[0][0], buf, sizeof(buf) -1);
        if(rc > -1) {
          buf[rc] = 0;
          LOG(LOG_DEBUG, "modem core #%d sent response '%s'", i, buf);
          accept_pending = FALSE;
        }
      }
    }
    /* Check which port received a connection */
    for(int port_idx = 0; port_idx < port_count; port_idx++) {
      if (FD_ISSET(ports[port_idx].socket, &readfs)) {  // IP traffic on this port
        if(!accept_pending) {
          LOG(LOG_DEBUG, "Incoming connection on %s", ports[port_idx].name);
          // first try for a modem assigned to this port that is listening.
          for(int m = 0; m < ports[port_idx].modem_count; m++) {
            i = ports[port_idx].modem_list[m];
            if(cfg[i].s[0] != 0 && cfg[i].is_off_hook == FALSE
               && cfg[i].line_data.is_connected == FALSE
               && (cfg[i].dce_data.is_ip232 || cfg[i].dce_data.is_connected)) {
              // send signal to pipe saying pick up...
              LOG(LOG_DEBUG, "Routing %s to listening modem #%d", ports[port_idx].name, i);
              cfg[i].line_data.sfd = ports[port_idx].socket;
              writePipe(cfg[i].mp[1][1], MSG_CALLING);
              accept_pending = TRUE; accept_pending_since = time(NULL);
              break;
            }
          }
          // now, send to any non-active modem assigned to this port.
          if(!accept_pending) {
            for(int m = 0; m < ports[port_idx].modem_count; m++) {
              i = ports[port_idx].modem_list[m];
              if(cfg[i].is_off_hook == FALSE
                 && cfg[i].line_data.is_connected == FALSE
                 && (cfg[i].dce_data.is_ip232 || cfg[i].dce_data.is_connected)) {
                // send signal to pipe saying pick up...
                LOG(LOG_DEBUG, "Routing %s to available modem #%d", ports[port_idx].name, i);
                cfg[i].line_data.sfd = ports[port_idx].socket;
                writePipe(cfg[i].mp[1][1], MSG_CALLING);
                accept_pending = TRUE; accept_pending_since = time(NULL);
                break;
              }
            }
          }
          if(!accept_pending) {
            LOG(LOG_DEBUG, "No available modems for %s, send busy and close", ports[port_idx].name);
            // no connections.., accept and print error
            cSocket = ip_accept(ports[port_idx].socket);
            if(cSocket > -1) {
              // No tracing on this data output
              if(strlen(all_busy) < 1) {
                ip_write(cSocket, (unsigned char *)MDM_BUSY, strlen(MDM_BUSY));
              } else {
                writeFile(all_busy, cSocket);
              }
              close(cSocket);
            }
          }
        }
        break;  /* Only handle one port per loop iteration */
      }
    }
  }
  LOG(LOG_INFO, "Shutting down (%s)", dashboard_quit_requested() ? "dashboard quit" : "signal received");
  /* Graceful drain: ask each modem to hang up its caller cleanly (sends
   * NO CARRIER) instead of yanking the TCP connection out from under an
   * active session. Give them a brief moment to flush before we exit. */
  for(i = 0; i < modem_count; i++) {
    writePipe(cfg[i].mp[1][1], MSG_HANGUP);
  }
  usleep(300000);
  webserver_stop();
  {
    const char *sp = dashboard_get_stats_path();
    if(sp) dashboard_save_stats(sp);
  }
  if(log_get_trace_flags() & TRACE_MONITOR) {
    extern void term_restore(void);
    term_restore();
    fprintf(stderr, "\033[?25h\033[0m\n");
  }
  LOG_EXIT();
  return 0;
}
