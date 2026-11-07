#include <stdio.h>
#include <stdarg.h>
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
#include "serial.h"
#include "secure_script.h"
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

/* Exit codes: 0 ok, 1 runtime, 2 usage/config error. */
#define EXIT_USAGE 2

static void usage_error(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void usage_error(const char *name, const char *fmt, ...) {
  va_list ap;
  fputs("\n  tcpser: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n  Run '%s -h' for the full option list.\n\n", name);
  exit(EXIT_USAGE);
}

/* Strict integer option: rejects "abc", "12abc", empty. */
static int opt_int(const char *name, char flag, const char *s) {
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if(!s[0] || !end || *end != '\0' || errno != 0 || v < -2147483647L || v > 2147483647L)
    usage_error(name, "-%c expects a number, got '%s'", flag, s);
  return (int)v;
}

/* Clamp with a visible warning (previously a LOG_WARN nobody saw at startup). */
static int opt_clamp(char flag, int v, int lo, int hi) {
  if(v < lo) { fprintf(stderr, "  Warning: -%c %d is below the minimum, using %d\n", flag, v, lo); return lo; }
  if(v > hi) { fprintf(stderr, "  Warning: -%c %d is above the maximum, using %d\n", flag, v, hi); return hi; }
  return v;
}

/* A file option must exist and be readable now, not fail silently per caller. */
static void opt_readable(const char *name, char flag, const char *path, int allow_dir) {
  struct stat st;
  if(stat(path, &st) != 0)
    usage_error(name, "-%c %s: %s", flag, path, strerror(errno));
  if(S_ISDIR(st.st_mode) && !allow_dir)
    usage_error(name, "-%c %s is a directory; a file is required", flag, path);
  if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
    usage_error(name, "-%c %s is not a regular file", flag, path);
  if(access(path, R_OK) != 0)
    usage_error(name, "-%c %s is not readable: %s", flag, path, strerror(errno));
}

void print_help(char* name) {
  fprintf(stdout,
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
"    -b LIST           on-connect caller speed menu (CBase POKE 838); LIST is\n"
"                      speed=pokeval pairs, e.g. -b 2400=6,9600=8,38400=10\n"
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
"    -H PORT           web dashboard (live stats/traffic) + /metrics (HTTP)\n"
"                      binds 127.0.0.1; TCPSER_DASH_BIND to expose,\n"
"                      TCPSER_DASH_TOKEN to require ?token=... auth,\n"
"                      TCPSER_DASH_TRAFFIC=1 to show payload content\n"
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
  cfg[0].idx = 0;
  cfg[0].dce_data.port_speed = 38400;
  cfg[0].line_speed = 38400;

  while(opt>-1 && i < max_modem) {
    opt=getopt(argc, argv, "p:s:S:d:v:hw:i:Il:L:t:n:a:A:c:C:N:B:T:P:G:E:k:xD:VZF:MO:m:e:Y:W:H:b:");
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
            default:
              usage_error(argv[0], "-t: unknown trace flag '%c' (use m M s S i I)", optarg[j]);
          }
          log_set_trace_flags(trace_flags);
        }
        break;
      case 'a':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].local_answer, optarg, sizeof(cfg[i].local_answer));
        break;
      case 'A':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].remote_answer, optarg, sizeof(cfg[i].remote_answer));
        break;
      case 'c':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].local_connect, optarg, sizeof(cfg[i].local_connect));
        break;
      case 'C':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].remote_connect, optarg, sizeof(cfg[i].remote_connect));
        break;
      case 'B':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(all_busy, optarg, all_busy_len);
        break;
      case 'N':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].no_answer, optarg, sizeof(cfg[i].no_answer));
        break;
      case 'T':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].inactive, optarg, sizeof(cfg[i].inactive));
        break;
      case 'i':
        safe_strncpy(cfg[i].cur_line, optarg, sizeof(cfg[i].cur_line));
        cfg[i].cur_line_idx = strlen(cfg[i].cur_line);
        break;
      case 'P':
        opt_readable(argv[0], opt, optarg, TRUE);   /* file, or a directory for rotation */
        safe_strncpy(cfg[i].pre_connect_file, optarg, sizeof(cfg[i].pre_connect_file));
        break;
      case 'G':
        opt_readable(argv[0], opt, optarg, FALSE);
        safe_strncpy(cfg[i].access_granted_file, optarg, sizeof(cfg[i].access_granted_file));
        break;
      case 'E':
        /* Fail at startup, not at the first successful gate with an invisible
         * LOG_ERROR. -E takes a NAME inside TCPSER_SCRIPT_DIR, not a path. */
        if(strchr(optarg, '/'))
          usage_error(argv[0], "-E takes a script NAME inside %s, not a path ('%s')", TCPSER_SCRIPT_DIR, optarg);
        if(!validate_script_name(optarg))
          usage_error(argv[0], "-E '%s': use letters, digits, '-', '_' and '.' only", optarg);
        {
          char spath[600];
          snprintf(spath, sizeof(spath), "%s/%s", TCPSER_SCRIPT_DIR, optarg);
          if(access(spath, X_OK) != 0)
            usage_error(argv[0], "-E %s: %s is not an executable file (%s)", optarg, spath, strerror(errno));
        }
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
          /* -k "" used to enable the password gate with a password nobody can
           * type (the gate requires a non-empty entry): every caller locked out,
           * silently. */
          if(optarg[0] == 0)
            usage_error(argv[0], "-k requires a non-empty password (or -k env:VAR)");
          if(strlen(optarg) >= sizeof(cfg[i].gate_password))
            usage_error(argv[0], "-k password is too long (max %zu characters)", sizeof(cfg[i].gate_password) - 1);
          safe_strncpy(cfg[i].gate_password, optarg, sizeof(cfg[i].gate_password));
        }
        break;
      case 'W':
        cfg[i].gate_timeout = opt_clamp(opt, opt_int(argv[0], opt, optarg), 1, 300);
        break;
      case 'F':
        cfg[i].gate_max_fails = opt_clamp(opt, opt_int(argv[0], opt, optarg), 1, 100);
        break;
      case 'w':
        cfg[i].watchdog_timeout = opt_clamp(opt, opt_int(argv[0], opt, optarg), 0, 600);
        break;
      case 'O':
        connlog_init(optarg);
        break;
      case 'm':
        cfg[i].max_session_minutes = opt_clamp(opt, opt_int(argv[0], opt, optarg), 0, 7 * 24 * 60);
        break;
      case 'e':
        if(strncmp(optarg, "http://", 7) != 0 && strncmp(optarg, "https://", 8) != 0)
          usage_error(argv[0], "-e expects an http:// or https:// webhook URL");
        webhook_init(optarg);
        break;
      case 'Y':
        dashboard_set_stats_path(optarg);
        break;
      case 'H':
        {
          int wport = opt_int(argv[0], opt, optarg);
          if(wport < 1 || wport > 65535)
            usage_error(argv[0], "-H %d: web dashboard port must be 1..65535", wport);
          webserver_init(wport);
        }
        break;
      case 'I':
        cfg[i].invert_dcd = TRUE;
        break;
      case 'b':
        /* -b speed=poke838val[,speed=poke838val...] : on-connect caller baud menu.
         * Each pair defines a selectable serial speed and the value CBase's
         * "POKE 838,<val>" needs to switch to it. Order = menu order. */
        {
          char list[256];
          safe_strncpy(list, optarg, sizeof(list));
          cfg[i].baud_menu_count = 0;
          char *pair = strtok(list, ",");
          while(pair && cfg[i].baud_menu_count < BAUD_MENU_MAX) {
            char *eq = strchr(pair, '=');
            if(eq) {
              *eq = 0;
              int sp = atoi(pair);
              int pv = atoi(eq + 1);
              if(sp > 0 && ser_get_bps_const(sp) >= 0) {
                cfg[i].baud_menu_speed[cfg[i].baud_menu_count] = sp;
                cfg[i].baud_menu_poke[cfg[i].baud_menu_count] = pv;
                cfg[i].baud_menu_count++;
              } else {
                fprintf(stderr, "Warning: -b ignoring unsupported speed '%s'\n", pair);
              }
            } else {
              fprintf(stderr, "Warning: -b entry '%s' is not speed=pokeval, ignoring\n", pair);
            }
            pair = strtok(NULL, ",");
          }
          /* Single-keypress menu tops out at 9 entries; warn if more were given. */
          if(pair != NULL) {
            fprintf(stderr, "Warning: -b supports at most %d speeds (single-key menu); ignoring the rest\n",
                    BAUD_MENU_MAX);
          }
          cfg[i].baud_menu_enabled = (cfg[i].baud_menu_count > 0);
        }
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
        {
          char *eq = strchr(optarg, '=');
          if(!eq || eq == optarg || eq[1] == 0)
            usage_error(argv[0], "-n expects NUMBER=host:port, got '%s'", optarg);
          tok = strtok(optarg, "=");
          if(pb_add(tok, strtok(NULL, "=")) != 0)
            usage_error(argv[0], "-n could not add phone book entry '%s' (book full?)", optarg);
        }
        break;
      case 'l':
        log_set_level(opt_clamp(opt, opt_int(argv[0], opt, optarg), 0, LOG_TRACE));
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
            usage_error(argv[0], "-L could not open log file %s: %s", optarg, strerror(errno));
          }
        }
        break;
      case 's':
        {
          int sp = opt_int(argv[0], opt, optarg);
          if(sp <= 0 || ser_get_bps_const(sp) < 0)
            usage_error(argv[0], "-s %d is not a supported serial speed (use e.g. 300, 1200, 2400, 9600, 19200, 38400, 57600, 115200)", sp);
          cfg[i].dce_data.port_speed = sp;
        }
        LOG(LOG_ALL, "Setting DTE speed to %d", cfg[i].dce_data.port_speed);
        if(dce_set == FALSE)
          cfg[i].line_speed = cfg[i].dce_data.port_speed;
        break;
      case '?':
        /* getopt already printed "illegal option" / "option requires an argument" */
        usage_error(argv[0], "invalid option. Usage: %s -d <device> | -v <port> [options]", argv[0]);
        break;
      case 'h':
        print_help(argv[0]);
        break;
      case 'd':
      case 'v':
        if (tty_set) {
          if (++i < max_modem) {
            dce_set = FALSE;
            mdm_init_config(&cfg[i]);
            cfg[i].idx = i;
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
            cfg[i].baud_menu_enabled = cfg[i - 1].baud_menu_enabled;
            cfg[i].baud_menu_count = cfg[i - 1].baud_menu_count;
            for(j = 0; j < cfg[i - 1].baud_menu_count; j++) {
              cfg[i].baud_menu_speed[j] = cfg[i - 1].baud_menu_speed[j];
              cfg[i].baud_menu_poke[j] = cfg[i - 1].baud_menu_poke[j];
            }
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
        cfg[i].line_speed = opt_int(argv[0], opt, optarg);
        if(cfg[i].line_speed <= 0)
          usage_error(argv[0], "-S expects a positive speed to report in CONNECT");
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
  } else if (argc <= 1) {
    /* Bare invocation (no arguments at all): treat like -h — show the help
     * cleanly and exit 0, instead of printing a scary FATAL error first. */
    print_help(argv[0]);              /* does not return (exit 0) */
  } else {
    /* Arguments were given but none selected a serial device. Give a precise,
     * friendly error pointing at the fix — not a giant help dump after a FATAL. */
    fprintf(stderr,
      "\n  tcpser: no serial device specified.\n"
      "  Give -d <device> for a real serial port (e.g. -d /dev/ttyUSB0)\n"
      "  or   -v <port>   for ip232 over TCP.\n"
      "  Run '%s -h' for the full option list.\n\n", argv[0]);
    exit(EXIT_USAGE);
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
