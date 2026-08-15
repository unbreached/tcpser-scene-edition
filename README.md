# TCPSER 1.4.0 - ELiTE RETRO BBS EDiTiON

**GENESiS-PROJECT // HARDENED TCPSER FOR FRoZEN FLoPPY BBS**

Custom fork of tcpser with security hardening, RS232 adapter diagnostics,
live dashboard, BBS recovery watchdog, and C64 optimizations. Transforms
a serial port into a Hayes-compatible modem over TCP/IP.

Modded/Improved by David Jacoby, david@unbreached.se

Designed for hosting Commodore 64 BBS systems on the Internet with
SwiftLink, Turbo232, GLINK232, and userport RS232 adapters.

---

## Quick Start

```bash
# 1. Build
make

# 2. Run the RS232 analyzer to find correct settings
./tcpser -d /dev/ttyUSB0 -s 2400 -Z

# 3. Run with the recommended settings
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x

# 4. Run with dashboard, logging, and connection log
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x -M \
  -L tcpser.log -O connections.csv
```

---

## Requirements

- GCC and Make (any POSIX system: Linux, macOS, BSD)
- A USB-serial adapter (FTDI recommended)
- A C64 RS232 cartridge: SwiftLink-232, CMD Turbo232, GGLABS GLINK232,
  or a userport adapter with level shifter (MAX232)

---

## Build & Install

### Building

| OS                  | Command                    |
|---------------------|----------------------------|
| Linux / macOS       | `make`                     |
| Solaris             | `make -f Makefile.solaris`  |
| BSD                 | `gmake`                    |

### Installing

```bash
sudo make install
```

Installs to `/usr/local` by default:
- Binary: `/usr/local/bin/tcpser`
- Man page: `/usr/local/share/man/man1/tcpser.1`
- Data files: `/usr/local/share/tcpser/` (SEQ screens, blocklist, service files)

Override the prefix:
```bash
sudo make install PREFIX=/opt/tcpser
```

### Uninstalling

```bash
sudo make uninstall
```

---

## Usage

```
tcpser -d <device> -s <speed> [options...]
tcpser -v <port>   -s <speed> [options...]
```

Run `tcpser -h` for the full option list, grouped by category.

### All Options

**Serial Port:**
| Flag | Description |
|------|-------------|
| `-d <device>` | Serial device (e.g. /dev/ttyUSB0) |
| `-v <port>` | Virtual RS232 via TCP (ip232 mode) |
| `-s <speed>` | Serial port speed (default: 38400) |
| `-S <speed>` | Speed reported in CONNECT message |
| `-I` | Invert DCD pin |
| `-i <string>` | Modem init string (omit AT prefix) |
| `-b <list>` | On-connect caller speed menu (requires CBase). `list` is comma-separated `speed=pokeval` pairs; the caller picks a speed, tcpser sends `POKE 838,<pokeval>` to CBase and retunes its own serial port to match. E.g. `-b 2400=6,9600=8,38400=10` (replace pokevals with your CBase POKE 838 values) |

**Network:**
| Flag | Description |
|------|-------------|
| `-p <port>` | TCP listen port (default: 6400) |
| `-n <num>=<addr>` | Phone book entry |
| `-D <addr:port>` | Direct connection |

**Security / Bot Gate:**
| Flag | Description |
|------|-------------|
| `-x` | Require BACKSPACE x2 before connect |
| `-k <password>` | Require password before connect (`-k env:VAR` reads it from `$VAR`, hidden from `ps`) |
| `-W <seconds>` | Gate timeout (default: 20, max: 300) |
| `-F <count>` | Failures before permanent block (default: 3) |
| `-P <file|dir>` | Pre-connect banner (file or directory for rotation) |
| `-G <file>` | Access granted screen |
| `-E <command>` | Execute on successful gate (async) |

**Event Files:**
| Flag | Event | Sent to |
|------|-------|---------|
| `-a <file>` | Answer | Serial (local) |
| `-A <file>` | Answer | TCP (remote) |
| `-c <file>` | Connect | Serial (local) |
| `-C <file>` | Connect | TCP (remote) |
| `-N <file>` | No answer | TCP (remote) |
| `-B <file>` | All busy | TCP (remote) |
| `-T <file>` | Inactivity timeout | TCP (remote) |

**Logging:**
| Flag | Description |
|------|-------------|
| `-l <level>` | Log level: 0=none 1=fatal .. 7=trace |
| `-L <file>` | Log to file (supports SIGHUP rotation) |
| `-O <file>` | Connection log (CSV: time,ip,result,duration,bytes; created mode 0600) |
| `-Y <file>` | Statistics file (persist counters across restarts) |
| `-t <flags>` | Trace: m/M=modem s/S=serial i/I=IP |

**BBS Recovery:**
| Flag | Description |
|------|-------------|
| `-w <seconds>` | BBS watchdog timeout (default: 120, 0=off) |
| `-m <minutes>` | Max session time (default: 0=unlimited) |

**Monitoring & Notifications:**
| Flag | Description |
|------|-------------|
| `-M` | Live terminal dashboard (full-screen, on stderr) |
| `-H <port>` | Web dashboard + `/metrics` (HTTP; binds 127.0.0.1 by default) |
| `-e <url>` | Webhook on events (Discord/Slack/ntfy/JSON POST; repeatable) |

**Tools:**
| Flag | Description |
|------|-------------|
| `-Z` | RS232 analyzer mode |
| `-V` | Print version and exit |
| `-h` | Help |

### Common Examples

**Basic BBS hosting:**
```bash
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x
```

**Production BBS with all features:**
```bash
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 \
  -x -W 30 \
  -P data/prelogin.seq \
  -G data/accessgranted.seq \
  -O /var/log/tcpser-connections.csv \
  -L /var/log/tcpser.log -l 4 \
  -w 120 -m 60 -M
```

**Multi-line BBS (two modems, one TCP port):**
```bash
tcpser -d /dev/ttyUSB0 -s 2400 -d /dev/ttyUSB1 -s 2400 -p 6400 -x
```

**Analyzer mode (diagnose RS232 adapter):**
```bash
tcpser -d /dev/ttyUSB0 -s 2400 -Z
```

---

## Live Dashboard (-M)

Full-screen terminal dashboard with:
- Modem activity LEDs (RX/TX for Caller, BBS, Modem)
- Hex + ASCII traffic view with direction indicators
- Connection history with color-coded results
- Live statistics (connections, blocked IPs, bytes, DCD/DSR state)
- Uptime counter and peer connection duration

### Dashboard Hotkeys

| Key | Action |
|-----|--------|
| `H` | Force hangup (disconnect current caller) |
| `R` | Factory reset modem (AT&F equivalent) |
| `S` | Refresh display |
| `B` | Show IP blocklist |
| `U` | Clear all timed blocks |
| `Q` | Quit tcpser |

All hotkey actions are thread-safe (sent through the bridge pipe protocol).

---

## BBS Recovery

### Watchdog (-w)

Detects when the BBS is hung after a caller disconnects ungracefully
(killed terminal, lost network). If no serial data arrives from the BBS
within the timeout (default: 120 seconds), tcpser toggles DTR/DCD to
force the 6551 ACIA to signal carrier loss to the BBS software.

- 1st-3rd attempt: DTR toggle (500ms low pulse)
- After 3 failed toggles: sends serial BREAK signal
- Resets when any serial data arrives from the BBS

Works with CBASE's `chkcarr` routine which polls DCD via the 6551
status register.

### Consecutive No-Answer Detection

If 3 consecutive callers receive "No Answer" (BBS didn't pick up),
tcpser assumes the BBS is stuck and forces a DTR toggle to recover.

### Max Session Time (-m)

Disconnects callers after a configurable number of minutes. Useful as
a safety net to prevent line hogging on a single-line BBS. Set to 0
(default) for unlimited.

### USB Serial Reconnect

If the USB-serial adapter disconnects (glitch, cable bump), tcpser
automatically waits up to 60 seconds for the device to reappear and
reopens it. No manual restart needed.

---

## Connection Log (-O)

Append-only CSV log of all connection events:

```csv
timestamp,ip,result,duration,bytes_in,bytes_out
2026-04-03 14:30:22,192.168.1.50,CONNECTED,0,0,0
2026-04-03 14:35:10,192.168.1.50,HANGUP,288,4096,8192
2026-04-03 14:36:01,10.0.0.5,BLOCKED,0,0,0
2026-04-03 14:37:15,10.0.0.99,GATE_FAIL,0,0,0
```

Result types: `CONNECTED`, `HANGUP`, `BLOCKED`, `RATE_LIMITED`, `GATE_FAIL`, `NO_ANSWER`

---

## IP Allowlist

Trusted IPs that skip the blocklist, rate limiter, and gate entirely.
One IP per line, comments with `#`.

- Default location: `data/tcpser.allowlist` or `tcpser.allowlist`
- Override: `export TCPSER_ALLOWLIST=/path/to/file`

```
# Friends of the BBS
192.168.1.100
10.0.0.50
```

---

## IP Blocklist

Persistent file-based blocklist with automatic reload:
- Default location: `data/tcpser.blocklist`
- Override: `export TCPSER_BLOCKLIST=/path/to/file`
- Atomic append with file locking (no duplicates)
- Auto-blocks after configurable gate failures (`-F`)
- Auto-reloads every 10 seconds when file changes
- Comments supported (`#` prefix)
- View from dashboard with `B`, clear timed blocks with `U`

---

## Statistics Persistence

Connection statistics (total connections, blocked, gate pass/fail,
bytes in/out) are persisted **only when you pass `-Y <file>`**. With
`-Y` set, counters are auto-saved every 60 seconds and on shutdown, and
restored on startup so they survive restarts. Without `-Y`, no stats
file is written or read.

```bash
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -M -Y data/tcpser.stats
```

---

## Web Dashboard (-H) and Metrics

`-H <port>` starts a read-only HTTP dashboard (mobile-friendly, SOC/NOC
styled) plus a Prometheus/OpenMetrics endpoint. It works on its own —
you do **not** need `-M` — and includes a **live traffic feed**:

- `GET /`            — HTML dashboard (live stats, connection history, live traffic)
- `GET /api/status`  — JSON status (stats + history + traffic)
- `GET /api/events`  — Server-Sent Events stream (auto-updates the page)
- `GET /metrics`     — Prometheus text exposition (`tcpser_*` counters)

**Security:** the dashboard exposes caller IP addresses and stats, so it
binds to `127.0.0.1` by default. The live traffic feed shows direction and
byte counts always, but the **payload content preview is opt-in** because it
can contain what callers type into the BBS (even passwords). Control exposure,
auth, and content with env vars:

| Variable | Effect | Default |
|----------|--------|---------|
| `TCPSER_DASH_BIND`    | Bind address (`0.0.0.0` to expose on the LAN/Internet) | `127.0.0.1` |
| `TCPSER_DASH_TOKEN`   | Require `?token=<value>` on every request (constant-time check) | _(none)_ |
| `TCPSER_DASH_TRAFFIC` | `1` = include ASCII payload content in the traffic feed | off (metadata only) |

If you expose the dashboard beyond localhost without a token, tcpser logs
a warning. Enabling `TCPSER_DASH_TRAFFIC` while exposed logs a louder one —
only turn it on behind localhost + a token. With a token set, open
`http://host:port/?token=SECRET` (the page forwards the token to the
API/SSE/metrics calls automatically). Per-connection read/write timeouts and a
bounded handler-thread pool protect against slow-client / exhaustion abuse.

```bash
export TCPSER_DASH_BIND=0.0.0.0
export TCPSER_DASH_TOKEN=$(head -c16 /dev/urandom | xxd -p)
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -H 8080
```

---

## Webhooks (-e)

`-e <url>` fires a JSON POST (Discord/Slack/ntfy/generic) on events:
`CONNECTED`, `DISCONNECTED`, `GATE_FAIL`, and `IP_BLOCKED`. Repeat `-e`
for multiple targets (up to 8). The webhook URL often embeds a secret
token, so it is passed to `curl` via a stdin config — it does **not**
appear in the process table (`ps`) and is masked in logs. Requires the
`curl` binary on `PATH`.

```bash
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -k secret \
  -e "https://discord.com/api/webhooks/XXX/YYY" \
  -e "https://ntfy.sh/my-bbs"
```

---

## Banner Rotation

If `-P` points to a directory instead of a file, tcpser picks a
random banner file for each new connection:

```bash
tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x -P data/banners/
```

Place multiple `.seq` files in the directory for variety.

---

## RS232 Analyzer Mode

The analyzer (`-Z`) is an interactive diagnostic tool that determines
the correct tcpser settings for your specific C64 RS232 adapter.

### Supported Adapters

- **SwiftLink-232** (6551 ACIA, max 38400 baud)
- **CMD Turbo232** (6551 + extension register, max 230400 baud)
- **GGLABS GLINK232** (SwiftLink compatible, 3 BOM configurations)
- **Userport adapters** (software UART, 300-2400 baud)
- **UP9600 mod** (CIA shift register trick, 9600 baud)

### How It Works

**Phases 1-4 (interactive)** require you to change C64 state:

| Phase | You do this | It tests |
|-------|------------|----------|
| 1 | Keep C64 OFF | Baseline signal levels |
| 2 | Power ON (no BBS) | Which lines respond to power |
| 3 | Start BBS software | Which lines change when BBS is ready |
| 4 | (automatic) | DTR/RTS toggle to map null-modem wiring |
| 4b | (automatic) | DCD gating (6551 ACIA receiver gate test) |
| 4c | (automatic) | DSR disconnect behavior |

**Phase 5 (fully autonomous, ~4-7 minutes):**

Sweeps 90 combinations of baud rate, flow control, and parity.
For each one it configures the port, sends RING + CONNECT, listens
for BBS response, sends NO CARRIER to reset, and moves on.

**Phase 6** verifies the best settings with a full simulated call.

**Phase 7** (interactive) you shut down the BBS to observe which
control lines drop.

**Phase 8** identifies your cartridge type and produces a recommended
command line.

### DCD Gating (6551 ACIA)

All cartridges using the 6551 ACIA chip have a hardware behavior where
**the receiver is disabled when DCD is not asserted**. tcpser defaults
to DCD following carrier state. The analyzer confirms this behavior in
Phase 4b.

---

## Security Features

### Bot Protection Gate

Pre-authentication filter that runs before any serial/modem traffic.

| Flag | Mode |
|------|------|
| `-x` | Require BACKSPACE pressed twice (accepts 0x08, 0x14, 0x7F) |
| `-k <password>` | Require password (PETSCII/ASCII normalized) |
| `-W <seconds>` | Gate timeout (default: 20, max: 300) |
| `-F <count>` | Failures before permanent IP block (default: 3) |

### Rate Limiting

5-second minimum between connection attempts from the same IP.
Prevents rapid reconnection abuse.

### External Hook

```bash
-E "/path/to/script.sh"
```

Executes asynchronously after successful gate. Environment variable
`TCPSER_PEER_IP` contains the caller's IP address.

---

## AT Command Reference

### Supported Commands

| Command | Effect |
|---------|--------|
| `ATA` | Answer incoming call |
| `ATD<number>` | Dial (TCP connect) |
| `ATDL` | Redial last number |
| `ATH0` | Hang up |
| `ATH1` | Go off-hook |
| `ATO` | Return to online data mode |
| `ATE0` / `ATE1` | Echo off / on |
| `ATQ0` / `ATQ1` | Responses on / off |
| `ATV0` / `ATV1` | Numeric / verbose result codes |
| `ATX0`-`ATX4` | Result code level |
| `ATW0`-`ATW2` | Connect message format |
| `ATZ` | Reset modem |
| `AT&C0` / `AT&C1` | DCD always on / follows carrier |
| `AT&D0`-`AT&D2` | DTR: ignore / command mode / hang up |
| `AT&F` | Load factory defaults |
| `AT&K0` / `AT&K3` / `AT&K4` | Flow: none / RTS-CTS / XON-XOFF |
| `ATS<n>=<v>` | Set S-register |
| `ATS<n>?` | Query S-register |
| `A/` | Repeat last command |
| `+++` | Escape to command mode (with guard time) |

### Key S-Registers

| Register | Purpose | Default |
|----------|---------|---------|
| S0 | Rings to auto-answer (0=never) | 0 |
| S2 | Escape character | 43 (+) |
| S3 | Line termination (CR) | 13 |
| S4 | Response formatting (LF) | 10 |
| S5 | Backspace character | 8 |
| S7 | Carrier wait timeout (sec) | 50 |
| S12 | Escape guard time (1/50 sec) | 50 (1.0s) |
| S30 | Inactivity timeout (10s units) | 0 (off) |

### Phone Book

```bash
tcpser -n 5551212=bbs.example.com:6400
```

---

## Signal Handling

| Signal | Action |
|--------|--------|
| `SIGHUP` | Reopen log file (for logrotate) |
| `SIGUSR1` | Dump statistics to stderr |
| `SIGTERM` / `SIGINT` | Graceful shutdown (save stats) |
| `SIGPIPE` / `SIGIO` | Ignored (prevents crashes) |
| `SIGCHLD` | Auto-reap child processes |

---

## Cable Wiring

tcpser uses the PC serial port's DTR line to signal DCD to the C64.

### Simplified Bi-directional (recommended)

```
    PC          C64
    CTS---------RTS
    RTS---------CTS
    TXD---------RXD
    RXD---------TXD
    DTR---------DCD
    DCD---------DTR
    GND---------GND
```

### Minimal 3-Wire

If your adapter only has TX/RX/GND, use `-i "&D0"` to ignore DTR
(tcpser cannot detect C64 hang-up without control lines).

---

## Auto-Start Service

### Linux (systemd)

```bash
# Edit paths in the service file first
sudo cp data/tcpser.service /etc/systemd/system/
sudo systemctl enable tcpser
sudo systemctl start tcpser
```

### macOS (launchd)

```bash
# Edit paths in the plist first
sudo cp data/com.tcpser.bbs.plist /Library/LaunchDaemons/
sudo launchctl load /Library/LaunchDaemons/com.tcpser.bbs.plist
```

Both service files have `KeepAlive`/`Restart=always` so tcpser
automatically restarts on crash.

---

## Telnet Support

When tcpser detects a telnet client (IAC byte 0xFF), it automatically
negotiates telnet protocol including echo suppression, binary
transmission, and terminal type/speed negotiation.

---

## ip232 Mode

For use with WinVICE or other emulators with virtual RS232:

```bash
tcpser -v 25232 -s 38400 -l 4 -p 6400
```

---

## Project Structure

```
tcpser-scene-edition/
  src/               Source code (25 C modules)
    bridge.c         Connection handling, gate, blocklist, watchdog
    modem_core.c     AT command emulation, Hayes modem
    dashboard.c      Live full-screen terminal dashboard
    webserver.c      HTTP web dashboard + /metrics (auth, timeouts)
    webhook.c        Event webhooks (Discord/Slack/ntfy)
    analyzer.c       RS232 adapter diagnostic tool
    connlog.c        CSV connection logging
    secure_http.c    Hardened HTTP request parsing
    secure_paths.c   Path-traversal-safe file validation
    secure_script.c  Sandboxed gate-success script execution
    phone_book.c     Dial-target phone book
    util.c           Shared helpers (safe copy, constant-time compare, time)
    tcpser.c         Main loop, signal handling, graceful drain
    ip.c             TCP/IP networking, keepalive
    serial.c         Serial port I/O
    dce.c            DCE (modem) abstraction
    line.c           Line (TCP) abstraction
    nvt.c            Telnet NVT negotiation
    ...
  data/              PETSCII screens, blocklist, service files
  docs/              Documentation (see docs/README.md)
  samples/           systemd / fail2ban / firewall / nginx examples
  man/               Man page
  Makefile           Build system (Linux/macOS, hardened flags)
```

---

## Stability Features

- **SIGPIPE/SIGIO** ignored (prevents crashes on broken connections)
- **SIGTERM/SIGINT** graceful shutdown with stats persistence
- **TCP keepalive** enabled (60s idle, 30s interval, 3 probes)
- **Robust I/O** with EINTR retry and short-write handling
- **Atomic blocklist** updates with file locking
- **Answer timeout** prevents scanners from holding modem lines
- **DTR grace period** prevents BBS init race on reconnect
- **USB serial reconnect** survives adapter disconnects
- **Thread-safe dashboard** commands via pipe protocol

---

## Troubleshooting

### No data from BBS at any baud rate
1. TX/RX are likely swapped. Try a null-modem adapter.
2. Check that the RS232 cartridge is firmly seated.
3. Try a different USB-serial adapter (FTDI most reliable).
4. Run the analyzer (`-Z`) to systematically diagnose.

### BBS receives RING but doesn't answer
1. Set auto-answer in init string: `-i "s0=1"`
2. Check that DCD is asserted.

### Callers connect but see garbage
1. Baud rate mismatch between tcpser (`-s`) and BBS software.
2. Run the analyzer to detect the correct baud rate.

### BBS hangs after ungraceful disconnect
1. The watchdog (`-w 120`, default) will toggle DTR after 2 minutes.
2. After 3 consecutive no-answers, DTR is toggled automatically.
3. Press `H` in the dashboard for immediate hangup.

### Dashboard shows "Caller Online" after no-answer
Fixed in 1.2.0 — the connected state is now cleared on all
no-answer paths.

---

## License

TCPSER is distributed under the GPL 2.0 or later.

## Credits

- **Jim Brain** (brain@jbrain.com) - Original tcpser
- **Anthony Tolle** (atolle@gcns.com) - ip232 support
- **David Jacoby** (david@unbreached.se) - Security hardening, dashboard, analyzer, BBS recovery, C64 optimizations
