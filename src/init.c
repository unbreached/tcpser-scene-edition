#include <stdio.h>
#include <stdlib.h>       // for exit,atoi
#include <string.h>
#include <unistd.h>
#include "version.h"
#include "debug.h"
#include "phone_book.h"
#include "init.h"
#include "connlog.h"
#include "webhook.h"
#include "dashboard.h"
#include "webserver.h"
#include "util.h"

void print_help(char* name) {
  fprintf(stderr,
"\n"
"  TCPSER %s - %s\n"
"  Hayes Modem Emulator over TCP/IP\n"
"  =====================================================================\n"
"\n"
"  Usage: %s -d <device> [options]      (real serial port)\n"
"         %s -v <port>   [options]      (ip232 over TCP)\n"
"\n"
"  QUICK START\n"
"    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -k secret\n"
"    (serial @2400 baud, listen on TCP 6400, password gate)\n"
"\n", VERSION, EDITION, name, name, name);

  fprintf(stderr,
"  SERIAL\n"
"    -d DEVICE         serial device                  e.g. /dev/ttyUSB0\n"
"    -v PORT           virtual RS232 (ip232 over TCP)\n"
"    -s SPEED          DTE / serial port speed        [38400]\n"
"    -S SPEED          speed reported in CONNECT       [= -s]\n"
"    -i STRING         modem init string (no AT prefix)\n"
"    -I                invert DCD pin\n"
"\n"
"  NETWORK\n"
"    -p PORT[:MODEMS]  TCP listen port                [6400, all modems]\n"
"                      e.g. -p 23:0,1   -p 64128:2\n"
"    -n NUM=ADDR       phone book entry               5551212=bbs.com:23\n"
"    -D ADDR:PORT      direct connection (: = receiver mode)\n"
"\n");

  fprintf(stderr,
"  SECURITY / BOT GATE   (recommended for any public BBS)\n"
"    -k PASSWORD       require password to connect\n"
"                      (use -k env:VAR to read it from $VAR, hidden from ps)\n"
"    -x                require BACKSPACE x2 to connect\n"
"    -W SECONDS        gate timeout                   [20, max 300]\n"
"    -F COUNT          fails before permanent block   [3, range 1..100]\n"
"    -P FILE           pre-connect banner (PETSCII/ANSI)\n"
"    -G FILE           access-granted screen\n"
"    -E SCRIPT         run on gate success; name only, from\n"
"                      /usr/local/tcpser/scripts/ (alphanumeric)\n"
"\n"
"  EVENT FILES   (sent automatically when the named event occurs)\n"
"    -a FILE  answer  -> C64        -A FILE  answer  -> caller\n"
"    -c FILE  connect -> C64        -C FILE  connect -> remote\n"
"    -N FILE  no-answer -> caller   -B FILE  busy    -> caller\n"
"    -T FILE  timeout -> caller\n"
"\n");

  fprintf(stderr,
"  LOGGING\n"
"    -l LEVEL          log level: 0=none 1=fatal .. 7=trace   [0]\n"
"    -L FILE           log to file                    [stderr]\n"
"    -t FLAGS          trace: m/M=modem s/S=serial i/I=IP\n"
"    -O FILE           connection log (CSV, mode 0600)\n"
"    -Y FILE           stats file (persist counters across restarts)\n"
"\n"
"  RECOVERY / LIMITS\n"
"    -w SECONDS        BBS watchdog                   [120, 0=off, max 600]\n"
"    -m MINUTES        max session time               [0=unlimited]\n"
"\n"
"  MONITORING / NOTIFY\n"
"    -M                live ASCII traffic monitor (color, on stderr)\n"
"    -H PORT           web dashboard + /metrics (HTTP)\n"
"                      binds 127.0.0.1; set TCPSER_DASH_BIND to expose,\n"
"                      TCPSER_DASH_TOKEN to require ?token=... auth\n"
"    -e URL            webhook (Discord/Slack/ntfy/JSON POST; repeatable)\n"
"\n"
"  TOOLS\n"
"    -Z                RS232 analyzer (diagnose wiring; needs -d)\n"
"    -V                print version and exit\n"
"    -h                this help\n"
"\n"
"  -d/-v repeatable for multi-line BBS; -s/-S/-i and gate settings\n"
"  propagate to later modems.  Note: -M (monitor) differs from -t M\n"
"  (trace modem-out).\n"
"\n");

  fprintf(stderr,
"  EXAMPLES\n"
"    Basic BBS:    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -x\n"
"    With gate:    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -k secret\n"
"    Dashboard:    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -H 8080\n"
"    Full logging: %s -d /dev/ttyUSB0 -s 2400 -l 7 -tsSiI -p 6400\n"
"    Analyzer:     %s -d /dev/ttyUSB0 -s 2400 -Z\n"
"\n", name, name, name, name, name);
  exit(0);
}

int init(int argc,
         char **argv,
         modem_config cfg[],
         int max_modem,
         port_config ports[],
         int *port_count,
         char *all_busy,
         int all_busy_len,
         int *analyzer_mode
         ) {
  int i = 0;
  int j = 0;
  int opt = 0;
  int trace_flags = 0;
  char *tok;

  *port_count = 0;
  int dce_set = FALSE;
  int tty_set = FALSE;

  LOG_ENTER();
  mdm_init_config(&cfg[0]);
  cfg[0].dce_data.port_speed = 38400;
  cfg[0].line_speed = 38400;

  while(opt>-1 && i < max_modem) {
    opt=getopt(argc, argv, "p:s:S:d:v:hw:i:Il:L:t:n:a:A:c:C:N:B:T:P:G:E:k:xD:VZF:MO:m:e:Y:W:H:");
    switch(opt) {
      case 't':
        trace_flags = log_get_trace_flags();
        for(j = 0; optarg[j] != '\0'; j++) {
          switch(optarg[j]) {
            case 'm':
              trace_flags |= TRACE_MODEM_IN;
              break;
            case 'M':
              trace_flags |= TRACE_MODEM_OUT;
              break;
            case 's':
              trace_flags |= TRACE_SERIAL_IN;
              break;
            case 'S':
              trace_flags |= TRACE_SERIAL_OUT;
              break;
            case 'i':
              trace_flags |= TRACE_IP_IN;
              break;
            case 'I':
              trace_flags |= TRACE_IP_OUT;
              break;
          }
          log_set_trace_flags(trace_flags);
        }
        break;
      case 'a':
        safe_strncpy(cfg[i].local_answer, optarg, sizeof(cfg[i].local_answer));
        break;
      case 'A':
        safe_strncpy(cfg[i].remote_answer, optarg, sizeof(cfg[i].remote_answer));
        break;
      case 'c':
        safe_strncpy(cfg[i].local_connect, optarg, sizeof(cfg[i].local_connect));
        break;
      case 'C':
        safe_strncpy(cfg[i].remote_connect, optarg, sizeof(cfg[i].remote_connect));
        break;
      case 'B':
        safe_strncpy(all_busy, optarg, all_busy_len);
        break;
      case 'N':
        safe_strncpy(cfg[i].no_answer, optarg, sizeof(cfg[i].no_answer));
        break;
      case 'T':
        safe_strncpy(cfg[i].inactive, optarg, sizeof(cfg[i].inactive));
        break;
      case 'i':
        safe_strncpy(cfg[i].cur_line, optarg, sizeof(cfg[i].cur_line));
        cfg[i].cur_line_idx = strlen(cfg[i].cur_line);
        break;
      case 'P':
        safe_strncpy(cfg[i].pre_connect_file, optarg, sizeof(cfg[i].pre_connect_file));
        break;
      case 'G':
        safe_strncpy(cfg[i].access_granted_file, optarg, sizeof(cfg[i].access_granted_file));
        break;
      case 'E':
        safe_strncpy(cfg[i].on_gate_success_script, optarg, sizeof(cfg[i].on_gate_success_script));
        break;
      case 'x':
        cfg[i].gate_mode = 1; // esc
        break;
      case 'k':
        cfg[i].gate_mode = 2; // password
        /* "-k env:VARNAME" reads the password from environment variable VARNAME
         * so the secret never appears in the process table (ps). A literal
         * "-k password" still works for convenience but is visible in ps. */
        if(strncmp(optarg, "env:", 4) == 0) {
          const char *v = getenv(optarg + 4);
          if(v && v[0]) {
            safe_strncpy(cfg[i].gate_password, v, sizeof(cfg[i].gate_password));
          } else {
            fprintf(stderr, "Error: -k env:%s but that environment variable is unset or empty\n", optarg + 4);
            exit(1);
          }
        } else {
          safe_strncpy(cfg[i].gate_password, optarg, sizeof(cfg[i].gate_password));
        }
        break;
      case 'W':
        cfg[i].gate_timeout = atoi(optarg);
        if(cfg[i].gate_timeout <= 0) cfg[i].gate_timeout = 20;
        if(cfg[i].gate_timeout > 300) {
          LOG(LOG_WARN, "Gate timeout clamped to 300 seconds (was %d)", cfg[i].gate_timeout);
          cfg[i].gate_timeout = 300;
        }
        break;
      case 'F':
        cfg[i].gate_max_fails = atoi(optarg);
        if(cfg[i].gate_max_fails < 1) cfg[i].gate_max_fails = 1;
        if(cfg[i].gate_max_fails > 100) cfg[i].gate_max_fails = 100;
        break;
      case 'w':
        cfg[i].watchdog_timeout = atoi(optarg);
        if(cfg[i].watchdog_timeout < 0) cfg[i].watchdog_timeout = 0;
        if(cfg[i].watchdog_timeout > 600) cfg[i].watchdog_timeout = 600;
        break;
      case 'O':
        connlog_init(optarg);
        break;
      case 'm':
        cfg[i].max_session_minutes = atoi(optarg);
        if(cfg[i].max_session_minutes < 0) cfg[i].max_session_minutes = 0;
        break;
      case 'e':
        webhook_init(optarg);
        break;
      case 'Y':
        dashboard_set_stats_path(optarg);
        break;
      case 'H':
        webserver_init(atoi(optarg));
        break;
      case 'I':
        cfg[i].invert_dcd = TRUE;
        break;
      case 'p':
        /* Parse -p port[:modem_list] */
        if(*port_count >= MAX_PORTS) {
          fprintf(stderr, "Error: Maximum %d ports supported\n", MAX_PORTS);
          exit(1);
        }
        {
          port_config *pc = &ports[*port_count];
          char *colon = strchr(optarg, ':');
          int port_num;

          /* Parse port number */
          if(colon) *colon = 0;
          port_num = atoi(optarg);
          if(port_num <= 0 || port_num > 65535) {
            fprintf(stderr, "Error: Invalid port number %s\n", optarg);
            exit(1);
          }

          pc->port = port_num;
          pc->socket = -1;
          pc->modem_count = 0;
          snprintf(pc->addr, sizeof(pc->addr), ":%d", port_num);
          snprintf(pc->name, sizeof(pc->name), "port%d", port_num);

          /* Parse modem list (if specified) */
          if(colon) {
            char *modem_list = colon + 1;
            char *modem_tok = strtok(modem_list, ",");
            while(modem_tok && pc->modem_count < 16) {
              int modem_idx = atoi(modem_tok);
              if(modem_idx >= 0) {  /* Just check non-negative, validate count later */
                pc->modem_list[pc->modem_count++] = modem_idx;
              }
              modem_tok = strtok(NULL, ",");
            }
          }
          (*port_count)++;
        }
        break;
      case 'n':
        tok = strtok(optarg, "=");
        pb_add(tok, strtok(NULL, "="));
        break;
      case 'l':
        log_set_level(atoi(optarg));
        break;
      case 'L':
        {
          FILE *lf = fopen(optarg, "a");
          if(lf) {
            log_set_file(lf);
            /* Save name for SIGHUP log rotation */
            extern void log_set_file_by_name(const char *, FILE *);
            log_set_file_by_name(optarg, lf);
          } else {
            fprintf(stderr, "Warning: could not open log file %s\n", optarg);
          }
        }
        break;
      case 's':
        cfg[i].dce_data.port_speed = atoi(optarg);
        LOG(LOG_ALL, "Setting DTE speed to %d", cfg[i].dce_data.port_speed);
        if(dce_set == FALSE)
          cfg[i].line_speed = cfg[i].dce_data.port_speed;
        break;
      case '?':
      case 'h':
        print_help(argv[0]);
        break;
      case 'd':
      case 'v':
        if (tty_set) {
          if (++i < max_modem) {
            dce_set = FALSE;
            mdm_init_config(&cfg[i]);
            cfg[i].dce_data.port_speed = cfg[i - 1].dce_data.port_speed;
            cfg[i].line_speed = cfg[i - 1].line_speed;
            cfg[i].dce_data.is_ip232 = FALSE;
            safe_strncpy(cfg[i].cur_line, cfg[i - 1].cur_line, sizeof(cfg[i].cur_line));
            safe_strncpy(cfg[i].local_connect, cfg[i - 1].local_connect, sizeof(cfg[i].local_connect));
            safe_strncpy(cfg[i].remote_connect, cfg[i - 1].remote_connect, sizeof(cfg[i].remote_connect));
            safe_strncpy(cfg[i].local_answer, cfg[i - 1].local_answer, sizeof(cfg[i].local_answer));
            safe_strncpy(cfg[i].remote_answer, cfg[i - 1].remote_answer, sizeof(cfg[i].remote_answer));
            safe_strncpy(cfg[i].no_answer, cfg[i - 1].no_answer, sizeof(cfg[i].no_answer));
            safe_strncpy(cfg[i].inactive, cfg[i - 1].inactive, sizeof(cfg[i].inactive));
            safe_strncpy(cfg[i].pre_connect_file, cfg[i - 1].pre_connect_file, sizeof(cfg[i].pre_connect_file));
            safe_strncpy(cfg[i].access_granted_file, cfg[i - 1].access_granted_file, sizeof(cfg[i].access_granted_file));
            safe_strncpy(cfg[i].on_gate_success_script, cfg[i - 1].on_gate_success_script, sizeof(cfg[i].on_gate_success_script));
            cfg[i].gate_mode = cfg[i - 1].gate_mode;
            safe_strncpy(cfg[i].gate_password, cfg[i - 1].gate_password, sizeof(cfg[i].gate_password));
            cfg[i].gate_timeout = cfg[i - 1].gate_timeout;
            cfg[i].gate_max_fails = cfg[i - 1].gate_max_fails;
            cfg[i].watchdog_timeout = cfg[i - 1].watchdog_timeout;
            cfg[i].max_session_minutes = cfg[i - 1].max_session_minutes;
          } else {
            LOG(LOG_WARN, "Maximum modems defined - ignoring extra");
            break;
          }
        }
        safe_strncpy(cfg[i].dce_data.tty, optarg, sizeof(cfg[i].dce_data.tty));
        LOG(LOG_ALL, "Setting TTY to %s", optarg);
        cfg[i].dce_data.is_ip232 = ('v' == opt);
        tty_set = TRUE;
        break;
      case 'S':
        cfg[i].line_speed = atoi(optarg);
        dce_set = TRUE;
        break;
      case 'D':
        cfg[i].direct_conn = TRUE;
        safe_strncpy(cfg[i].direct_conn_num, optarg, sizeof(cfg[i].direct_conn_num));
        break;
      case 'Z':
        if(analyzer_mode) *analyzer_mode = TRUE;
        break;
      case 'M':
        trace_flags = log_get_trace_flags();
        trace_flags |= TRACE_MONITOR;
        log_set_trace_flags(trace_flags);
        break;
      case 'V':
        printf("TCPSER %s - %s\n", VERSION, EDITION);
        printf("Original by Jim Brain (tcpser@jbrain.com)\n");
        printf("Modded/Improved by David Jacoby, david@unbreached.se\n");
        exit(0);
        break;
    }
  }

  if (tty_set) {
    if (i < max_modem)
      i++;
  } else {
    // no modems defined
    LOG(LOG_FATAL, "No modems defined");
    print_help(argv[0]);
  }

  LOG(LOG_DEBUG, "Read configuration for %i serial port(s)", i);

  /* Configure ports: set default if none specified, assign modems */
  if(*port_count == 0) {
    /* No -p flags specified, use default port 6400 with all modems */
    ports[0].port = 6400;
    ports[0].socket = -1;
    snprintf(ports[0].addr, sizeof(ports[0].addr), ":6400");
    snprintf(ports[0].name, sizeof(ports[0].name), "port6400");
    ports[0].modem_count = i;
    for(j = 0; j < i; j++) ports[0].modem_list[j] = j;
    *port_count = 1;
  } else {
    /* Validate modem indices and assign unassigned modems to ports */
    for(j = 0; j < *port_count; j++) {
      if(ports[j].modem_count == 0) {
        /* This port has no specific modem assignments, give it all modems */
        ports[j].modem_count = i;
        for(int k = 0; k < i; k++) ports[j].modem_list[k] = k;
      } else {
        /* Validate specified modem indices */
        int valid_count = 0;
        for(int k = 0; k < ports[j].modem_count; k++) {
          if(ports[j].modem_list[k] < i) {
            ports[j].modem_list[valid_count++] = ports[j].modem_list[k];
          } else {
            fprintf(stderr, "Warning: Port %d references non-existent modem %d (only %d modems configured)\n",
                    ports[j].port, ports[j].modem_list[k], i);
          }
        }
        ports[j].modem_count = valid_count;
        if(valid_count == 0) {
          /* No valid modems, give it all modems as fallback */
          fprintf(stderr, "Warning: Port %d has no valid modems, assigning all modems\n", ports[j].port);
          ports[j].modem_count = i;
          for(int k = 0; k < i; k++) ports[j].modem_list[k] = k;
        }
      }
    }
  }

  LOG_EXIT();
  return i;
}
