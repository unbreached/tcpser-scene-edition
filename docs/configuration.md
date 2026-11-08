# Configuration

Everything tcpser does is set by options. Each option has a short flag, a long
flag and a config-file key of the same name, so `-s 2400`, `--speed 2400` and
`speed = 2400` are the same thing. Values are validated at startup; a bad one
prints a message and exits with code 2.

- [The config file](#the-config-file)
- [Checking a configuration](#checking-a-configuration)
- [Precedence](#precedence)
- [Multi-line BBS](#multi-line-bbs)
- [Environment variables](#environment-variables)
- [All options](#all-options)

The option tables and the template on this page are generated from the binary
(`make docs`; `make check` fails when they drift).

## The config file

```
tcpser --config /etc/tcpser/tcpser.conf
```

One `key = value` per line. `#` starts a comment, values may be quoted, flags
take `yes`/`no`. Everything before the first `[line]` section is global; each
`[line]` is one modem and must contain `device = /dev/...` or `ip232 = PORT`.
Per-line settings (speed, init string, gate, screens, watchdog, session
limit) carry over to the next `[line]` unless changed, exactly as the
command-line flags propagate to later `-d`/`-v`.

`tcpser help config` prints this template:

<!-- template:begin -->
```ini
# tcpser configuration  (tcpser --config THIS_FILE; check with: tcpser check --config THIS_FILE)
#
# One `key = value` per line; keys are the long option names (tcpser -h).
# Flags take yes/no. Values may be quoted. `#` starts a comment.
# Everything before the first [line] section is global. Each [line] section
# is one modem and needs `device = /dev/...` or `ip232 = PORT`; settings in
# a [line] apply to that line and carry over to the next one unless changed.
# Command-line flags are applied after this file and override it.
# Secrets: use `password = env:VAR` and the environment for web-token.

listen      = 6400
log-level   = 4
#log-file   = /var/log/tcpser/tcpser.log
#connlog    = /var/log/tcpser/connections.csv
#stats      = /var/lib/tcpser/stats
#blocklist  = /var/lib/tcpser/tcpser.blocklist

# web console on localhost (put nginx + TLS in front to expose it)
#web        = 8080
#web-bind   = 127.0.0.1
#name       = My BBS

# notifications
#webhook    = https://discord.com/api/webhooks/...

[line]
device        = /dev/ttyUSB0
speed         = 2400
#init         = &C0&K3
backspace-gate = yes
#password     = env:TCPSER_GATE_PASSWORD
gate-timeout  = 20
gate-fails    = 3
#banner       = /usr/local/share/tcpser/prelogin.seq
#granted      = /usr/local/share/tcpser/accessgranted.seq
watchdog      = 120
#max-session  = 120

# a second line, e.g. VICE over ip232 (inherits the gate settings above)
#[line]
#ip232        = 25232
#speed        = 38400
```
<!-- template:end -->

## Checking a configuration

```
tcpser check --config /etc/tcpser/tcpser.conf
tcpser check -d /dev/ttyUSB0 -s 2400 -p 6400 -x
```

`check` parses and validates everything the real start would (files must
exist, speeds must be supported, the password must be non-empty, the web
token URL-safe, ...), prints the resolved configuration and exits 0. On an
error it prints the file name and line number and exits 2. It opens no ports
and creates no files, so it is safe in `ExecStartPre=` and in a shell before a
restart.

## Precedence

1. Command-line flags, applied last, win.
2. The config file (`--config` may be given more than once; later files win).
3. Environment variables, for the settings that have one (see below).
4. Built-in defaults.

Secrets should not be in the file or on the command line: use
`password = env:VARNAME` and set the web token through `TCPSER_DASH_TOKEN`
(systemd: `EnvironmentFile=`).

## Multi-line BBS

Each `-d`/`-v` (or `[line]`) starts a new line, numbered from 0. Callers are
routed to the first idle line of the port they called. `-p PORT:MODEMS`
restricts a port to specific lines:

```ini
listen = 23:0,1      # port 23 rings lines 0 and 1
listen = 64128:2     # port 64128 rings line 2 only

[line]
device = /dev/ttyUSB0
speed  = 2400
[line]
device = /dev/ttyUSB1
speed  = 2400
[line]
ip232  = 25232
speed  = 38400
```

## Environment variables

| Variable | Option | Effect | Default |
|----------|--------|--------|---------|
| `TCPSER_BLOCKLIST` | `--blocklist` | persistent IP blocklist file | `tcpser.blocklist` in the working directory |
| `TCPSER_ALLOWLIST` | `--allowlist` | IPs that skip blocklist, rate limit and gate | `tcpser.allowlist` in the working directory |
| `TCPSER_DASH_BIND` | `--web-bind` | web console bind address | `127.0.0.1` |
| `TCPSER_DASH_TOKEN` | `--web-token` | require `?token=…` once to sign in | none |
| `TCPSER_DASH_TRAFFIC` | `--web-traffic` | `1` = payload bytes in the traffic feed | off |
| `TCPSER_DASH_NAME` | `--name` | BBS name in the web console | `TCPSER` |
| `TCPSER_IP232_BIND` | `--ip232-bind` | bind address of the `-v` listener | `127.0.0.1` |
| `TCPSER_TUI_PHOSPHOR` | `--phosphor` | terminal dashboard colour | `green` |
| `TCPSER_TUI_ASCII` | `--ascii` | ASCII glyphs in the terminal dashboard | off |
| `NO_COLOR` | | monochrome terminal dashboard | |
| `TCPSER_PEER_IP` | | set *by* tcpser for the `-E` hook: the caller's IP | |

## All options

<!-- options:begin -->

**General**

| Option | Config key | Description |
|--------|------------|-------------|
| `--config FILE` | `config` | Read options from a config file (`key = value`, one per line, `[line]` sections for each modem; `tcpser help config` prints a template). Flags given on the command line are applied after the file. |
| `-h`, `--help` | `help` | Help. `tcpser help config` prints a config template, `help markdown` / `help man` the option tables. |
| `-V`, `--version` | `version` | Print the version and exit. |

**Serial**

| Option | Config key | Description |
|--------|------------|-------------|
| `-d`, `--device DEVICE` | `device` | Serial device, e.g. `/dev/ttyUSB0`. Repeat (or mix with `-v`) for a multi-line BBS; each `-d`/`-v` starts a new line. |
| `-v`, `--ip232 PORT` | `ip232` | Virtual RS232 over TCP (ip232 mode, e.g. VICE). Binds `127.0.0.1`; set `TCPSER_IP232_BIND` / `--ip232-bind` to expose it (whoever reaches this port controls the modem, so firewall it). |
| `-s`, `--speed SPEED` | `speed` | Serial port speed (default 38400; standard rates 300–230400). |
| `-S`, `--connect-speed SPEED` | `connect-speed` | Speed reported in the `CONNECT` message (default = `--speed`). |
| `-i`, `--init STRING` | `init` | Modem init string without `AT`, e.g. `-i "&C0&K3S0=1"`. `ATZ` and `AT&F` restore it. |
| `-I`, `--invert-dcd` | `invert-dcd` | Invert the DCD line. |
| `-b`, `--baud-menu LIST` | `baud-menu` | On-connect caller speed menu for CBase: `speed=pokeval` pairs, e.g. `-b 2400=6,9600=8,38400=10`. The caller picks a speed, tcpser sends `POKE 838,pokeval` to CBase and retunes the serial port. |

**Network**

| Option | Config key | Description |
|--------|------------|-------------|
| `-p`, `--listen PORT[:MODEMS]` | `listen` | TCP listen port (default 6400, all modems). Repeatable; `-p 23:0,1 -p 64128:2` routes ports to specific lines. |
| `-n`, `--phonebook NUM=HOST:PORT` | `phonebook` | Phone book entry for `ATD<number>`, e.g. `-n 5551212=bbs.example.com:23`. Repeatable. |
| `-D`, `--direct HOST:PORT` | `direct` | Direct connection to `HOST:PORT` (`:` alone = receiver mode). |

**Security / bot gate**

| Option | Config key | Description |
|--------|------------|-------------|
| `-x`, `--backspace-gate` | `backspace-gate` | Require BACKSPACE twice before the modem rings (bots never send it). |
| `-k`, `--password PASSWORD` | `password` | Require a password before the modem rings. `-k env:VAR` reads it from the environment so it never shows in `ps`. |
| `-W`, `--gate-timeout SECONDS` | `gate-timeout` | Gate timeout in seconds (default 20, max 300). |
| `-F`, `--gate-fails COUNT` | `gate-fails` | Gate failures before the caller's IP is blocked permanently (default 3, range 1–100). |
| `-P`, `--banner FILE\|DIR` | `banner` | Pre-connect PETSCII banner; a directory rotates banners randomly. |
| `-G`, `--granted FILE` | `granted` | Access-granted screen sent after the gate. |
| `-E`, `--gate-hook NAME` | `gate-hook` | Gate-success hook: a script NAME (not a path) inside `/usr/local/tcpser/scripts/`; gets the caller IP in `TCPSER_PEER_IP`. |

**Event screens**

| Option | Config key | Description |
|--------|------------|-------------|
| `-a`, `--answer-local FILE` | `answer-local` | Sent to the C64 on answer. |
| `-A`, `--answer-remote FILE` | `answer-remote` | Sent to the caller on answer. |
| `-c`, `--connect-local FILE` | `connect-local` | Sent to the C64 on connect. |
| `-C`, `--connect-remote FILE` | `connect-remote` | Sent to the caller on connect. |
| `-N`, `--no-answer FILE` | `no-answer` | Sent to the caller when nobody answers. |
| `-B`, `--busy FILE` | `busy` | Sent to the caller when every line is busy. |
| `-T`, `--timeout-screen FILE` | `timeout-screen` | Sent to the caller on inactivity timeout. |

**Logging**

| Option | Config key | Description |
|--------|------------|-------------|
| `-l`, `--log-level 0-7` | `log-level` | Log level: 0 none, 1 fatal … 4 info … 7 trace (default 0). |
| `-L`, `--log-file FILE` | `log-file` | Log file (SIGHUP reopens it for logrotate). |
| `-t`, `--trace FLAGS` | `trace` | Hex traces: `m`/`M` modem in/out, `s`/`S` serial in/out, `i`/`I` IP in/out (lower = in, upper = out). |
| `-O`, `--connlog FILE` | `connlog` | Connection CSV log (created mode 0600). |
| `-Y`, `--stats FILE` | `stats` | Persist counters across restarts (saved every 60 s and on exit). |

**Recovery / limits**

| Option | Config key | Description |
|--------|------------|-------------|
| `-w`, `--watchdog SECONDS` | `watchdog` | BBS watchdog: toggle DTR if the BBS does not re-arm after a disconnect (default 120 s, 0 = off, max 600). |
| `-m`, `--max-session MINUTES` | `max-session` | Maximum session time in minutes (default 0 = unlimited). |

**Monitoring / notify**

| Option | Config key | Description |
|--------|------------|-------------|
| `-M`, `--monitor` | `monitor` | Terminal dashboard on stderr (needs a tty). `TCPSER_TUI_PHOSPHOR`, `TCPSER_TUI_ASCII` and `NO_COLOR` tune it. |
| `-H`, `--web PORT` | `web` | Web console and Prometheus `/metrics` on `PORT`. Binds `127.0.0.1` unless `--web-bind` says otherwise. |
| `--web-bind ADDR` | `web-bind` | Bind address of the web console (default `127.0.0.1`; expose only behind nginx + TLS). Also `TCPSER_DASH_BIND`. |
| `--web-token TOKEN` | `web-token` | Require `?token=TOKEN` once to sign in to the web console (letters, digits, `-`, `_`, `.`). Prefer the environment variable: a token on the command line is visible in `ps`. Also `TCPSER_DASH_TOKEN`. |
| `--web-traffic` | `web-traffic` | Include payload content in the web traffic feed (callers' keystrokes; keep it behind a token). Also `TCPSER_DASH_TRAFFIC`. |
| `--name NAME` | `name` | BBS name shown in the web console header. Also `TCPSER_DASH_NAME`. |
| `-e`, `--webhook URL` | `webhook` | Webhook URL for connection events (Discord, Slack, ntfy or plain JSON POST). Repeatable, up to 8. |
| `--phosphor green\|amber\|white` | `phosphor` | Terminal dashboard phosphor (`green`, `amber` or `white`). Also `TCPSER_TUI_PHOSPHOR`. |
| `--ascii` | `ascii` | Draw the terminal dashboard with ASCII glyphs even in a UTF-8 locale. Also `TCPSER_TUI_ASCII`. |

**Files**

| Option | Config key | Description |
|--------|------------|-------------|
| `--blocklist FILE` | `blocklist` | Persistent IP blocklist (default `tcpser.blocklist` in the working directory). Also `TCPSER_BLOCKLIST`. |
| `--allowlist FILE` | `allowlist` | Allowlist of IPs that skip the blocklist, the rate limiter and the gate (default `tcpser.allowlist` in the working directory). Also `TCPSER_ALLOWLIST`. |
| `--ip232-bind ADDR` | `ip232-bind` | Bind address of the `-v` (ip232) listener (default `127.0.0.1`). Also `TCPSER_IP232_BIND`. |
| `-Z`, `--analyze` | `analyze` | RS232 analyzer: diagnose adapter wiring (needs `-d`). |
<!-- options:end -->
