/*
 * The option table and everything generated from it: getopt strings, -h,
 * README/man tables, config-file loading. See options.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include "options.h"
#include "version.h"

#define G_GENERAL  "GENERAL"
#define G_SERIAL   "SERIAL"
#define G_NET      "NETWORK"
#define G_GATE     "SECURITY / BOT GATE"
#define G_EVENT    "EVENT SCREENS"
#define G_LOG      "LOGGING"
#define G_LIMITS   "RECOVERY / LIMITS"
#define G_MON      "MONITORING / NOTIFY"
#define G_FILES    "FILES"

const opt_desc OPTS[] = {
  /* id, name, arg, metavar, group, help, detail, env, per_line */
  { LO_CONFIG, "config", OPT_ARG, "FILE", G_GENERAL,
    "read options from FILE (see tcpser help config)",
    "Read options from a config file (`key = value`, one per line, `[line]` sections for each modem; "
    "`tcpser help config` prints a template). Flags given on the command line are applied after the file.", NULL, 0 },
  { 'h', "help", OPT_FLAG, NULL, G_GENERAL, "this help", "Help. `tcpser help config` prints a config template, `help markdown` / `help man` the option tables.", NULL, 0 },
  { 'V', "version", OPT_FLAG, NULL, G_GENERAL, "print version and exit", "Print the version and exit.", NULL, 0 },

  { 'd', "device", OPT_ARG, "DEVICE", G_SERIAL, "serial device, e.g. /dev/ttyUSB0",
    "Serial device, e.g. `/dev/ttyUSB0`. Repeat (or mix with `-v`) for a multi-line BBS; each `-d`/`-v` starts a new line.", NULL, 1 },
  { 'v', "ip232", OPT_ARG, "PORT", G_SERIAL, "virtual RS232 over TCP (ip232 / VICE)",
    "Virtual RS232 over TCP (ip232 mode, e.g. VICE). Binds `127.0.0.1`; set `TCPSER_IP232_BIND` / `--ip232-bind` to expose it "
    "(whoever reaches this port controls the modem, so firewall it).", NULL, 1 },
  { 's', "speed", OPT_ARG, "SPEED", G_SERIAL, "serial port speed [38400]",
    "Serial port speed (default 38400; standard rates 300–230400).", NULL, 1 },
  { 'S', "connect-speed", OPT_ARG, "SPEED", G_SERIAL, "speed reported in CONNECT [= speed]",
    "Speed reported in the `CONNECT` message (default = `--speed`).", NULL, 1 },
  { 'i', "init", OPT_ARG, "STRING", G_SERIAL, "modem init string (no AT prefix)",
    "Modem init string without `AT`, e.g. `-i \"&C0&K3S0=1\"`. `ATZ` and `AT&F` restore it.", NULL, 1 },
  { 'I', "invert-dcd", OPT_FLAG, NULL, G_SERIAL, "invert the DCD line", "Invert the DCD line.", NULL, 1 },
  { 'b', "baud-menu", OPT_ARG, "LIST", G_SERIAL, "on-connect caller speed menu (CBase POKE 838)",
    "On-connect caller speed menu for CBase: `speed=pokeval` pairs, e.g. `-b 2400=6,9600=8,38400=10`. "
    "The caller picks a speed, tcpser sends `POKE 838,pokeval` to CBase and retunes the serial port.", NULL, 1 },

  { 'p', "listen", OPT_ARG, "PORT[:MODEMS]", G_NET, "TCP listen port [6400, all modems]",
    "TCP listen port (default 6400, all modems). Repeatable; `-p 23:0,1 -p 64128:2` routes ports to specific lines.", NULL, 0 },
  { 'n', "phonebook", OPT_ARG, "NUM=HOST:PORT", G_NET, "phone book entry for ATD<NUM>",
    "Phone book entry for `ATD<number>`, e.g. `-n 5551212=bbs.example.com:23`. Repeatable.", NULL, 0 },
  { 'D', "direct", OPT_ARG, "HOST:PORT", G_NET, "direct connection (: alone = receiver)",
    "Direct connection to `HOST:PORT` (`:` alone = receiver mode).", NULL, 1 },

  { 'x', "backspace-gate", OPT_FLAG, NULL, G_GATE, "require BACKSPACE x2 before the modem rings",
    "Require BACKSPACE twice before the modem rings (bots never send it).", NULL, 1 },
  { 'k', "password", OPT_ARG, "PASSWORD", G_GATE, "require a password (env:VAR reads $VAR)",
    "Require a password before the modem rings. `-k env:VAR` reads it from the environment so it never shows in `ps`.", NULL, 1 },
  { 'W', "gate-timeout", OPT_ARG, "SECONDS", G_GATE, "gate timeout [20, max 300]",
    "Gate timeout in seconds (default 20, max 300).", NULL, 1 },
  { 'F', "gate-fails", OPT_ARG, "COUNT", G_GATE, "fails before permanent block [3]",
    "Gate failures before the caller's IP is blocked permanently (default 3, range 1–100).", NULL, 1 },
  { 'P', "banner", OPT_ARG, "FILE|DIR", G_GATE, "pre-connect banner (PETSCII); DIR rotates",
    "Pre-connect PETSCII banner; a directory rotates banners randomly.", NULL, 1 },
  { 'G', "granted", OPT_ARG, "FILE", G_GATE, "access-granted screen", "Access-granted screen sent after the gate.", NULL, 1 },
  { 'E', "gate-hook", OPT_ARG, "NAME", G_GATE, "run scripts/NAME on gate success",
    "Gate-success hook: a script NAME (not a path) inside `/usr/local/tcpser/scripts/`; gets the caller IP in `TCPSER_PEER_IP`.", NULL, 1 },

  { 'a', "answer-local", OPT_ARG, "FILE", G_EVENT, "answer screen -> C64", "Sent to the C64 on answer.", NULL, 1 },
  { 'A', "answer-remote", OPT_ARG, "FILE", G_EVENT, "answer screen -> caller", "Sent to the caller on answer.", NULL, 1 },
  { 'c', "connect-local", OPT_ARG, "FILE", G_EVENT, "connect screen -> C64", "Sent to the C64 on connect.", NULL, 1 },
  { 'C', "connect-remote", OPT_ARG, "FILE", G_EVENT, "connect screen -> caller", "Sent to the caller on connect.", NULL, 1 },
  { 'N', "no-answer", OPT_ARG, "FILE", G_EVENT, "no-answer screen -> caller", "Sent to the caller when nobody answers.", NULL, 1 },
  { 'B', "busy", OPT_ARG, "FILE", G_EVENT, "all-lines-busy screen -> caller", "Sent to the caller when every line is busy.", NULL, 0 },
  { 'T', "timeout-screen", OPT_ARG, "FILE", G_EVENT, "inactivity-timeout screen -> caller", "Sent to the caller on inactivity timeout.", NULL, 1 },

  { 'l', "log-level", OPT_ARG, "0-7", G_LOG, "0 none, 1 fatal .. 4 info .. 7 trace [0]",
    "Log level: 0 none, 1 fatal … 4 info … 7 trace (default 0).", NULL, 0 },
  { 'L', "log-file", OPT_ARG, "FILE", G_LOG, "log to FILE [stderr]", "Log file (SIGHUP reopens it for logrotate).", NULL, 0 },
  { 't', "trace", OPT_ARG, "FLAGS", G_LOG, "hex traces: m/M modem, s/S serial, i/I IP",
    "Hex traces: `m`/`M` modem in/out, `s`/`S` serial in/out, `i`/`I` IP in/out (lower = in, upper = out).", NULL, 0 },
  { 'O', "connlog", OPT_ARG, "FILE", G_LOG, "connection log (CSV, mode 0600)", "Connection CSV log (created mode 0600).", NULL, 0 },
  { 'Y', "stats", OPT_ARG, "FILE", G_LOG, "persist counters across restarts", "Persist counters across restarts (saved every 60 s and on exit).", NULL, 0 },

  { 'w', "watchdog", OPT_ARG, "SECONDS", G_LIMITS, "BBS watchdog [120, 0=off, max 600]",
    "BBS watchdog: toggle DTR if the BBS does not re-arm after a disconnect (default 120 s, 0 = off, max 600).", NULL, 1 },
  { 'm', "max-session", OPT_ARG, "MINUTES", G_LIMITS, "max session time [0=unlimited]",
    "Maximum session time in minutes (default 0 = unlimited).", NULL, 1 },

  { 'M', "monitor", OPT_FLAG, NULL, G_MON, "terminal dashboard on stderr (needs a tty)",
    "Terminal dashboard on stderr (needs a tty). `TCPSER_TUI_PHOSPHOR`, `TCPSER_TUI_ASCII` and `NO_COLOR` tune it.", NULL, 0 },
  { 'H', "web", OPT_ARG, "PORT", G_MON, "web console + /metrics on PORT (127.0.0.1)",
    "Web console and Prometheus `/metrics` on `PORT`. Binds `127.0.0.1` unless `--web-bind` says otherwise.", NULL, 0 },
  { LO_WEB_BIND, "web-bind", OPT_ARG, "ADDR", G_MON, "web console bind address [127.0.0.1]",
    "Bind address of the web console (default `127.0.0.1`; expose only behind nginx + TLS).", "TCPSER_DASH_BIND", 0 },
  { LO_WEB_TOKEN, "web-token", OPT_ARG, "TOKEN", G_MON, "require ?token= on the web console",
    "Require `?token=TOKEN` once to sign in to the web console (letters, digits, `-`, `_`, `.`). Prefer the environment variable: a token on the command line is visible in `ps`.", "TCPSER_DASH_TOKEN", 0 },
  { LO_WEB_TRAFFIC, "web-traffic", OPT_FLAG, NULL, G_MON, "include payload bytes in the web traffic feed",
    "Include payload content in the web traffic feed (callers' keystrokes; keep it behind a token).", "TCPSER_DASH_TRAFFIC", 0 },
  { LO_NAME, "name", OPT_ARG, "NAME", G_MON, "BBS name shown in the web console", "BBS name shown in the web console header.", "TCPSER_DASH_NAME", 0 },
  { 'e', "webhook", OPT_ARG, "URL", G_MON, "webhook (Discord/Slack/ntfy/JSON POST; repeatable)",
    "Webhook URL for connection events (Discord, Slack, ntfy or plain JSON POST). Repeatable, up to 8.", NULL, 0 },
  { LO_PHOSPHOR, "phosphor", OPT_ARG, "green|amber|white", G_MON, "terminal dashboard phosphor",
    "Terminal dashboard phosphor (`green`, `amber` or `white`).", "TCPSER_TUI_PHOSPHOR", 0 },
  { LO_ASCII, "ascii", OPT_FLAG, NULL, G_MON, "ASCII box glyphs in the terminal dashboard",
    "Draw the terminal dashboard with ASCII glyphs even in a UTF-8 locale.", "TCPSER_TUI_ASCII", 0 },

  { LO_BLOCKLIST, "blocklist", OPT_ARG, "FILE", G_FILES, "persistent IP blocklist [./tcpser.blocklist]",
    "Persistent IP blocklist (default `tcpser.blocklist` in the working directory).", "TCPSER_BLOCKLIST", 0 },
  { LO_ALLOWLIST, "allowlist", OPT_ARG, "FILE", G_FILES, "IPs that skip blocklist, rate limit and gate",
    "Allowlist of IPs that skip the blocklist, the rate limiter and the gate (default `tcpser.allowlist` in the working directory).", "TCPSER_ALLOWLIST", 0 },
  { LO_IP232_BIND, "ip232-bind", OPT_ARG, "ADDR", G_FILES, "bind address of the -v listener [127.0.0.1]",
    "Bind address of the `-v` (ip232) listener (default `127.0.0.1`).", "TCPSER_IP232_BIND", 0 },
  { 'Z', "analyze", OPT_FLAG, NULL, G_FILES, "RS232 analyzer (diagnose wiring; needs -d)",
    "RS232 analyzer: diagnose adapter wiring (needs `-d`).", NULL, 0 },
};
const int OPTS_COUNT = (int)(sizeof(OPTS) / sizeof(OPTS[0]));

const opt_desc *opt_by_id(int id) {
  for(int i = 0; i < OPTS_COUNT; i++) if(OPTS[i].id == id) return &OPTS[i];
  return NULL;
}
const opt_desc *opt_by_name(const char *name) {
  for(int i = 0; i < OPTS_COUNT; i++) if(strcmp(OPTS[i].name, name) == 0) return &OPTS[i];
  return NULL;
}

const char *opt_short_string(void) {
  static char s[256];
  if(!s[0]) {
    size_t n = 0;
    s[n++] = ':';                                   /* distinguish "missing argument" from "unknown option" */
    for(int i = 0; i < OPTS_COUNT; i++) {
      if(OPTS[i].id > 255) continue;
      s[n++] = (char)OPTS[i].id;
      if(OPTS[i].has_arg) s[n++] = ':';
    }
    s[n] = 0;
  }
  return s;
}

const struct option *opt_long_options(void) {
  static struct option lo[sizeof(OPTS) / sizeof(OPTS[0]) + 1];
  if(!lo[0].name) {
    for(int i = 0; i < OPTS_COUNT; i++) {
      lo[i].name = OPTS[i].name;
      lo[i].has_arg = OPTS[i].has_arg ? required_argument : no_argument;
      lo[i].flag = NULL;
      lo[i].val = OPTS[i].id;
    }
    memset(&lo[OPTS_COUNT], 0, sizeof(lo[0]));
  }
  return lo;
}

/* ---- -h ---------------------------------------------------------------- */
static void opt_label(const opt_desc *o, char *buf, size_t sz) {
  if(o->id <= 255) snprintf(buf, sz, "-%c, --%s%s%s", o->id, o->name, o->has_arg ? " " : "", o->has_arg ? o->metavar : "");
  else             snprintf(buf, sz, "    --%s%s%s", o->name, o->has_arg ? " " : "", o->has_arg ? o->metavar : "");
}

void opt_print_help(FILE *out, const char *prog) {
  fprintf(out,
    "\n"
    "  TCPSER %s - %s\n"
    "  Hayes modem emulator over TCP/IP for C64 BBS systems\n"
    "  =====================================================================\n"
    "\n"
    "  Usage: %s -d DEVICE [options]        real serial port\n"
    "         %s -v PORT   [options]        ip232 over TCP (VICE)\n"
    "         %s --config FILE [overrides]  everything from a file\n"
    "         %s check [same options]       validate and print the configuration\n"
    "         %s help config|markdown|man   config template / option tables\n"
    "\n"
    "  QUICK START\n"
    "    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -x\n"
    "    (serial @2400 baud, listen on TCP 6400, BACKSPACE-x2 bot gate)\n",
    VERSION, EDITION, prog, prog, prog, prog, prog, prog);
  const char *group = NULL;
  for(int i = 0; i < OPTS_COUNT; i++) {
    const opt_desc *o = &OPTS[i];
    if(!group || strcmp(group, o->group) != 0) { group = o->group; fprintf(out, "\n  %s\n", group); }
    char lab[64]; opt_label(o, lab, sizeof(lab));
    fprintf(out, "    %-36s %s\n", lab, o->help);
    if(o->env) fprintf(out, "    %-36s (also %s)\n", "", o->env);
  }
  fprintf(out,
    "\n"
    "  -d/-v may be repeated for a multi-line BBS; speed, init string and gate\n"
    "  settings propagate to later lines. Note: -M (dashboard) is not -t M.\n"
    "  Bad values exit with code 2 and a message.\n"
    "\n"
    "  EXAMPLES\n"
    "    Basic BBS:    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -x\n"
    "    With gate:    %s -d /dev/ttyUSB0 -s 2400 -p 6400 -k env:BBS_PASSWORD\n"
    "    Dashboards:   %s -d /dev/ttyUSB0 -s 2400 -p 6400 -M -H 8080\n"
    "    Config file:  %s --config /etc/tcpser/tcpser.conf\n"
    "    Check it:     %s check --config /etc/tcpser/tcpser.conf\n"
    "    Analyzer:     %s -d /dev/ttyUSB0 -s 2400 -Z\n"
    "\n", prog, prog, prog, prog, prog, prog);
}

/* ---- README table ------------------------------------------------------ */
static const char *md_detail(const opt_desc *o) { return o->detail ? o->detail : o->help; }

void opt_print_markdown(FILE *out) {
  const char *group = NULL;
  for(int i = 0; i < OPTS_COUNT; i++) {
    const opt_desc *o = &OPTS[i];
    if(!group || strcmp(group, o->group) != 0) {
      group = o->group;
      /* "SECURITY / BOT GATE" -> "Security / bot gate" */
      char title[64]; snprintf(title, sizeof(title), "%s", group);
      for(char *p = title + 1; *p; p++) *p = (char)tolower((unsigned char)*p);
      fprintf(out, "\n**%s**\n\n| Option | Config key | Description |\n|--------|------------|-------------|\n", title);
    }
    char flag[96];
    if(o->id <= 255) snprintf(flag, sizeof(flag), "`-%c`, `--%s%s%s`", o->id, o->name, o->has_arg ? " " : "", o->has_arg ? o->metavar : "");
    else             snprintf(flag, sizeof(flag), "`--%s%s%s`", o->name, o->has_arg ? " " : "", o->has_arg ? o->metavar : "");
    fprintf(out, "| %s | `%s` | %s%s%s%s |\n", flag, o->name, md_detail(o), o->env ? " Also `" : "", o->env ? o->env : "", o->env ? "`." : "");
  }
}

/* ---- man page ---------------------------------------------------------- */
/* backticks -> bold, hyphens escaped */
static void man_text(FILE *out, const char *s) {
  int bold = 0;
  for(; *s; s++) {
    if(*s == '`') { fputs(bold ? "\\fP" : "\\fB", out); bold = !bold; }
    else if(*s == '-') fputs("\\-", out);
    else if(*s == '\\') fputs("\\\\", out);
    else fputc(*s, out);
  }
  if(bold) fputs("\\fP", out);
}

void opt_print_man(FILE *out) {
  const char *group = NULL;
  for(int i = 0; i < OPTS_COUNT; i++) {
    const opt_desc *o = &OPTS[i];
    if(!group || strcmp(group, o->group) != 0) {
      group = o->group;
      char title[64]; snprintf(title, sizeof(title), "%s", group);
      for(char *p = title + 1; *p; p++) *p = (char)tolower((unsigned char)*p);
      fprintf(out, ".SS \"%s\"\n", title);
    }
    fputs(".TP\n", out);
    if(o->id <= 255) fprintf(out, "\\fB\\-%c\\fP, ", o->id);
    fputs("\\fB\\-\\-", out); man_text(out, o->name); fputs("\\fP", out);
    if(o->has_arg) { fputs(" \\fI", out); man_text(out, o->metavar); fputs("\\fP", out); }
    fputc('\n', out);
    man_text(out, md_detail(o));
    if(o->env) { fputs(" Also the environment variable \\fB", out); fputs(o->env, out); fputs("\\fP.", out); }
    fputc('\n', out);
  }
}

/* ---- config template --------------------------------------------------- */
void opt_print_config_template(FILE *out) {
  fprintf(out,
    "# tcpser configuration  (tcpser --config THIS_FILE; check with: tcpser check --config THIS_FILE)\n"
    "#\n"
    "# One `key = value` per line; keys are the long option names (tcpser -h).\n"
    "# Flags take yes/no. Values may be quoted. `#` starts a comment.\n"
    "# Everything before the first [line] section is global. Each [line] section\n"
    "# is one modem and needs `device = /dev/...` or `ip232 = PORT`; settings in\n"
    "# a [line] apply to that line and carry over to the next one unless changed.\n"
    "# Command-line flags are applied after this file and override it.\n"
    "# Secrets: use `password = env:VAR` and the environment for web-token.\n"
    "\n"
    "listen      = 6400\n"
    "log-level   = 4\n"
    "#log-file   = /var/log/tcpser/tcpser.log\n"
    "#connlog    = /var/log/tcpser/connections.csv\n"
    "#stats      = /var/lib/tcpser/stats\n"
    "#blocklist  = /var/lib/tcpser/tcpser.blocklist\n"
    "\n"
    "# web console on localhost (put nginx + TLS in front to expose it)\n"
    "#web        = 8080\n"
    "#web-bind   = 127.0.0.1\n"
    "#name       = My BBS\n"
    "\n"
    "# notifications\n"
    "#webhook    = https://discord.com/api/webhooks/...\n"
    "\n"
    "[line]\n"
    "device        = /dev/ttyUSB0\n"
    "speed         = 2400\n"
    "#init         = &C0&K3\n"
    "backspace-gate = yes\n"
    "#password     = env:TCPSER_GATE_PASSWORD\n"
    "gate-timeout  = 20\n"
    "gate-fails    = 3\n"
    "#banner       = /usr/local/share/tcpser/prelogin.seq\n"
    "#granted      = /usr/local/share/tcpser/accessgranted.seq\n"
    "watchdog      = 120\n"
    "#max-session  = 120\n"
    "\n"
    "# a second line, e.g. VICE over ip232 (inherits the gate settings above)\n"
    "#[line]\n"
    "#ip232        = 25232\n"
    "#speed        = 38400\n");
}

/* ---- config file -> argv ----------------------------------------------- */
static char *trim(char *s) {
  while(isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while(e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static int is_true(const char *v)  { return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcmp(v, "1"); }
static int is_false(const char *v) { return !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcmp(v, "0") || !*v; }

typedef struct { const opt_desc *o; char *val; int lineno; } kv_t;

static int cfg_error(const char *path, int lineno, const char *fmt, const char *a) {
  fprintf(stderr, "\n  tcpser: %s:%d: ", path, lineno);
  fprintf(stderr, fmt, a);
  fputs("\n  Run 'tcpser help config' for a template.\n\n", stderr);
  return -1;
}

static int push(char **argv, int *argc, int cap, const char *s) {
  if(*argc >= cap - 1) return -1;
  argv[(*argc)++] = strdup(s);
  argv[*argc] = NULL;
  return 0;
}

/* emit one key=value as --key [value] */
static int emit(char **argv, int *argc, int cap, const kv_t *kv, const char *path) {
  char key[80]; snprintf(key, sizeof(key), "--%s", kv->o->name);
  if(kv->o->has_arg) {
    if(push(argv, argc, cap, key) || push(argv, argc, cap, kv->val)) return cfg_error(path, kv->lineno, "too many settings%s", "");
  } else {
    if(is_true(kv->val)) { if(push(argv, argc, cap, key)) return cfg_error(path, kv->lineno, "too many settings%s", ""); }
    else if(!is_false(kv->val)) return cfg_error(path, kv->lineno, "flag expects yes/no, got '%s'", kv->val);
  }
  return 0;
}

/* flush a [line] section: device/ip232 first so the other keys land on that line */
static int flush_line(char **argv, int *argc, int cap, kv_t *kvs, int n, int in_line, int section_line, const char *path) {
  if(!in_line) {
    for(int i = 0; i < n; i++) if(emit(argv, argc, cap, &kvs[i], path)) return -1;
    return 0;
  }
  int dev = -1;
  for(int i = 0; i < n; i++) if(kvs[i].o->id == 'd' || kvs[i].o->id == 'v') { if(dev >= 0) return cfg_error(path, kvs[i].lineno, "a [line] section takes one device or ip232 key%s", ""); dev = i; }
  if(dev < 0) return cfg_error(path, section_line, "[line] section has no 'device' or 'ip232' key%s", "");
  if(emit(argv, argc, cap, &kvs[dev], path)) return -1;
  for(int i = 0; i < n; i++) if(i != dev && emit(argv, argc, cap, &kvs[i], path)) return -1;
  return 0;
}

int opt_load_config(const char *path, char **argv, int argc, int cap, const char *prog) {
  (void)prog;
  FILE *f = fopen(path, "r");
  if(!f) { fprintf(stderr, "\n  tcpser: --config %s: %s\n\n", path, strerror(errno)); return -1; }
  char line[1024];
  kv_t kvs[128]; int n = 0, in_line = 0, section_line = 0, lineno = 0, rc = 0;
  while(fgets(line, sizeof(line), f)) {
    lineno++;
    if(strlen(line) >= sizeof(line) - 1 && line[sizeof(line) - 2] != '\n') { rc = cfg_error(path, lineno, "line too long%s", ""); break; }
    char *s = trim(line);
    if(!*s || *s == '#' || *s == ';') continue;
    if(*s == '[') {
      char *e = strchr(s, ']');
      if(!e) { rc = cfg_error(path, lineno, "unterminated section name%s", ""); break; }
      *e = 0; char *name = trim(s + 1);
      if(flush_line(argv, &argc, cap, kvs, n, in_line, section_line, path)) { rc = -1; break; }
      for(int i = 0; i < n; i++) free(kvs[i].val);
      n = 0;
      if(!strcasecmp(name, "line") || !strncasecmp(name, "line.", 5) || !strncasecmp(name, "line ", 5)) { in_line = 1; section_line = lineno; }
      else if(!strcasecmp(name, "tcpser") || !strcasecmp(name, "global") || !strcasecmp(name, "bbs")) in_line = 0;
      else { rc = cfg_error(path, lineno, "unknown section [%s] (use [line] or [tcpser])", name); break; }
      continue;
    }
    char *eq = strchr(s, '=');
    if(!eq) eq = strchr(s, ':');
    if(!eq) { rc = cfg_error(path, lineno, "expected key = value, got '%s'", s); break; }
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    for(char *p = key; *p; p++) { if(*p == '_') *p = '-'; else *p = (char)tolower((unsigned char)*p); }
    /* strip one pair of quotes; a trailing " # comment" after an unquoted value is dropped */
    size_t vl = strlen(val);
    if(vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') || (val[0] == '\'' && val[vl - 1] == '\''))) { val[vl - 1] = 0; val++; }
    else { char *h = strstr(val, " #"); if(h) *h = 0; val = trim(val); }
    const opt_desc *o = opt_by_name(key);
    if(!o && !strcmp(key, "port")) o = opt_by_name("listen");
    if(!o && !strcmp(key, "serial")) o = opt_by_name("device");
    if(!o) { rc = cfg_error(path, lineno, "unknown key '%s' (keys are the long option names, see tcpser -h)", key); break; }
    if(o->id == LO_CONFIG || o->id == 'h' || o->id == 'V') { rc = cfg_error(path, lineno, "'%s' cannot be set from a config file", key); break; }
    if(in_line && !o->per_line && o->id != 'd' && o->id != 'v') { rc = cfg_error(path, lineno, "'%s' is a global setting; put it before the first [line]", key); break; }
    if(n >= (int)(sizeof(kvs) / sizeof(kvs[0]))) { rc = cfg_error(path, lineno, "too many keys in one section%s", ""); break; }
    kvs[n].o = o; kvs[n].val = strdup(val); kvs[n].lineno = lineno; n++;
  }
  fclose(f);
  if(rc == 0 && flush_line(argv, &argc, cap, kvs, n, in_line, section_line, path)) rc = -1;
  for(int i = 0; i < n; i++) free(kvs[i].val);
  return rc < 0 ? -1 : argc;
}
