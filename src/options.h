#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdio.h>

/*
 * One table describes every command-line option: short letter, long name,
 * argument, group, one-line help and the longer description. From it we build
 * the getopt string and the getopt_long array, print -h, generate the option
 * tables in README.md and man/tcpser.1 (tcpser help --markdown / --man, checked
 * by `make check-docs`), and accept the same names as keys in a config file.
 */

enum { OPT_FLAG = 0, OPT_ARG = 1 };

/* long-only options get ids above 255 */
enum {
  LO_CONFIG = 256,
  LO_WEB_BIND, LO_WEB_TOKEN, LO_WEB_TRAFFIC, LO_NAME,
  LO_IP232_BIND, LO_BLOCKLIST, LO_ALLOWLIST,
  LO_PHOSPHOR, LO_ASCII
};

typedef struct {
  int         id;        /* short option character, or an LO_* id */
  const char *name;      /* long option / config key */
  int         has_arg;   /* OPT_FLAG or OPT_ARG */
  const char *metavar;   /* shown in help, e.g. "DEVICE" */
  const char *group;     /* help heading */
  const char *help;      /* one line for -h */
  const char *detail;    /* README / man paragraph (may be NULL = same as help) */
  const char *env;       /* for options that set an environment variable */
  int         per_line;  /* 1 = applies to the current -d/-v line (allowed in [line] sections) */
} opt_desc;

extern const opt_desc OPTS[];
extern const int OPTS_COUNT;

const opt_desc *opt_by_id(int id);
const opt_desc *opt_by_name(const char *name);

/* getopt plumbing built from the table */
const char *opt_short_string(void);              /* "p:s:S:d:..." */
struct option;
const struct option *opt_long_options(void);     /* terminated by {0,0,0,0} */

/* output */
void opt_print_help(FILE *out, const char *prog);
void opt_print_markdown(FILE *out);
void opt_print_man(FILE *out);
void opt_print_config_template(FILE *out);

/* Config file -> argv. Appends "--key" ["value"] pairs to *argv (which must
 * have room for `cap` entries), returns the new argc or -1 after printing the
 * error. Format: `key = value`, `# comment`, `[line]` starts a new modem (its
 * device/ip232 key is emitted first), `[tcpser]`/`[global]` return to globals.
 * Flags take yes/no/true/false/on/off/1/0. */
int opt_load_config(const char *path, char **argv, int argc, int cap, const char *prog);

#endif
