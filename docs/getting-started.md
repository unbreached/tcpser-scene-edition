# Getting started: your first call in 15 minutes

This walks from a bare machine to a BBS answering calls, on a Raspberry Pi
(or any Linux) and on a Mac. Every command below was run while writing it.

1. [Build](#1-build)
2. [Try it without hardware](#2-try-it-without-hardware) (ip232: fake C64 and fake caller on one machine)
3. [Cable and adapter](#3-cable-and-adapter)
4. [Let the analyzer find your settings](#4-let-the-analyzer-find-your-settings)
5. [Host the BBS](#5-host-the-bbs)
6. [Turn on the gate](#6-turn-on-the-gate)
7. [Watch it: dashboards](#7-watch-it-dashboards)
8. [Make it a service](#8-make-it-a-service)
9. [Where to go next](#9-where-to-go-next)

---

## 1. Build

```bash
git clone https://github.com/unbreached/tcpser-scene-edition
cd tcpser-scene-edition
make            # gcc or clang + make; no libraries beyond libc and pthreads
./tcpser -V
```

Raspberry Pi OS / Debian: `sudo apt install build-essential git`.
macOS: `xcode-select --install`.

Optional: `make check` runs the regression suite against the binary you just
built (a full modem session, the gates, the web console, a config file, a
webhook, a serial-loss scenario). It needs only Python 3.

## 2. Try it without hardware

tcpser can present the modem on a TCP port instead of a serial device (`-v`,
the ip232 protocol used by the VICE emulator). That lets you play both sides
on one machine. Open three terminals.

**Terminal 1, tcpser:**

```bash
./tcpser -v 25232 -p 6400 -x -l 4
```

`-v 25232` is the "C64" side, `-p 6400` is where callers come in, `-x` is the
BACKSPACE-twice bot gate, `-l 4` logs at info level to stderr.

**Terminal 2, the C64 (a plain TCP client typing AT commands):**

```bash
nc 127.0.0.1 25232
ATZ
```

You should see `OK`. Leave it open.

**Terminal 3, a caller:**

```bash
telnet 127.0.0.1 6400
```

You get the pre-connect banner (PETSCII, so it looks odd in telnet). Press
**BACKSPACE twice**: the banner changes to `ACCESS GRANTED` and terminal 2 shows
`RING`. Type `ATA` in terminal 2. Terminal 2 says `CONNECT 38400`; from now on
anything typed in terminal 2 appears in terminal 3 and vice versa. You just
made a call through the modem emulation. Type `+++` then `ATH0` in terminal 2
to hang up (or just close terminal 3).

The odd `ÿ` bytes you may see on the C64 side are ip232 control codes (DCD
and DTR state); a real emulator consumes them.

## 3. Cable and adapter

You need a USB-serial adapter on the PC side (FTDI chips are the most
reliable) and an RS232 cartridge on the C64 side: SwiftLink-232, CMD Turbo232,
GGLABS GLINK232, or a userport adapter with a MAX232 level shifter.

```
   PC (USB-serial)        C64 cartridge
        TXD ─────────────── RXD
        RXD ─────────────── TXD
        RTS ─────────────── CTS
        CTS ─────────────── RTS
        DTR ─────────────── DCD      ← tcpser signals carrier here
        DCD ─────────────── DTR
        GND ─────────────── GND
```

A 3-wire cable (TX, RX, GND) works for data but tcpser then cannot see the
BBS drop DTR to hang up; add `init = &D0` (or `-i "&D0"`) in that case.

Find the device name:

```bash
ls /dev/ttyUSB* /dev/ttyACM*        # Linux / Raspberry Pi
ls /dev/cu.usbserial*               # macOS
```

On Linux your user needs to be in the `dialout` group to open it:
`sudo usermod -aG dialout $USER`, then log out and in.

## 4. Let the analyzer find your settings

Start (or restart) the BBS software on the C64 so it sends its modem init
string, then:

```bash
./tcpser -d /dev/ttyUSB0 -s 2400 -Z
```

The analyzer listens at each baud rate until it sees valid AT commands,
toggles DTR and RTS to see which inputs move (this catches null-modem
cross-wiring and 3-wire cables), answers `OK` and checks the BBS accepts it,
then prints the cartridge type it inferred and a recommended command line.
Flow control and parity are not probed; treat the `&K` suggestion as something
to verify.

If nothing arrives at any speed, TX and RX are probably swapped. Try a
null-modem adapter or another USB-serial adapter.

There is also a C64-side companion, `c64/tcpser-link.prg`, which shows the
6551 registers and cable lines live on the C64 and lets you send `AT` from the
C64 to see tcpser's `OK`. See [`../c64/README.md`](../c64/README.md).

## 5. Host the BBS

Use the speed the analyzer found (2400 here) and a TCP port:

```bash
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400
```

From another machine: `telnet <your-ip> 6400` (or SyncTERM / CGTerm for
proper PETSCII). The BBS should answer the RING. Forward port 6400 on your
router if callers come from the Internet.

Common adjustments:

| Symptom | Fix |
|---------|-----|
| BBS gets `RING` but never answers | enable auto-answer in the init string, `-i "S0=1"`, or let the BBS send `ATA` |
| BBS ignores everything while idle | 6551 cartridges ignore input while DCD is low: `-i "&C0"`, or `-I` if the adapter inverts the line |
| callers see garbage | baud mismatch between `-s` and the BBS; rerun the analyzer |
| BBS hangs after an ungraceful disconnect | the watchdog (`-w 120`, on by default) pulses DTR; `H` in the dashboard hangs up now |

## 6. Turn on the gate

Scanners and bots connect to every open telnet port on the Internet and hold
your single line busy. The gate makes the caller prove they are a human
before the modem rings:

```bash
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x          # BACKSPACE twice
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -k env:BBS_PASSWORD   # or a password from $BBS_PASSWORD
```

After `-F 3` (default) failures an IP is blocked permanently in the blocklist
file, and one IP gets one attempt per 5 seconds. Point the blocklist at a
writable place: `--blocklist /var/lib/tcpser/tcpser.blocklist` (or the
`TCPSER_BLOCKLIST` variable). Put your own PETSCII screens in with
`-P banner.seq -G granted.seq`; the `data/` directory has examples.

The password is compared case-insensitively with bit 7 stripped so PETSCII
and ASCII callers type the same thing, so choose a long password rather than a
mixed-case one.

## 7. Watch it: dashboards

**Terminal** (`-M`): a 5250-style full-screen monitor on stderr. Works over SSH
on a Pi. Hotkeys: `H` hang up, `R` reset, `B` blocklist, `U` unblock, `P`
phosphor, `?` help, `Q` quit. It adapts from 60×16 up; add `-L tcpser.log` so
logging goes to a file instead of fighting the screen.

```bash
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x -M -L tcpser.log
```

**Web** (`-H 8080`): the same information as a web console with activity
charts and a live traffic feed, plus `/metrics` for Prometheus. It binds
`127.0.0.1` only. From your laptop:

```bash
ssh -L 8080:127.0.0.1:8080 pi@bbs-pi      # then open http://127.0.0.1:8080/
```

To expose it for real, put nginx with TLS in front and set a token
(`--web-token` or `TCPSER_DASH_TOKEN`); see
[`SECURITY_GUIDE.md`](SECURITY_GUIDE.md) and [`../samples/nginx/`](../samples/nginx/).

## 8. Make it a service

Move the command line into a config file so it survives reboots and edits.
`tcpser help config` prints a commented template; the keys are the long
option names:

```ini
listen     = 6400
log-level  = 4
log-file   = /var/log/tcpser/tcpser.log
blocklist  = /var/lib/tcpser/tcpser.blocklist
web        = 8080

[line]
device         = /dev/ttyUSB0
speed          = 2400
backspace-gate = yes
banner         = /usr/local/share/tcpser/prelogin.seq
granted        = /usr/local/share/tcpser/accessgranted.seq
```

Validate it, then install:

```bash
sudo make install
sudo install -d /etc/tcpser && sudo cp /usr/local/share/tcpser/tcpser.conf /etc/tcpser/
sudo nano /etc/tcpser/tcpser.conf
tcpser check --config /etc/tcpser/tcpser.conf       # prints what would run, exit 0, or 2 with the line number
```

**Linux (systemd):**

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin -G dialout tcpser
sudo cp /usr/local/share/tcpser/tcpser.service /etc/systemd/system/
sudo systemctl enable --now tcpser
journalctl -u tcpser -f
```

The unit runs `tcpser check` first, so a typo gives one clear message in the
journal instead of a restart loop. Secrets (a gate password, the web token) go
in `/etc/tcpser/tcpser.env` as `TCPSER_GATE_PASSWORD=...` /
`TCPSER_DASH_TOKEN=...`, referenced from the config as
`password = env:TCPSER_GATE_PASSWORD`.
[`../samples/deployment/install_secure.sh`](../samples/deployment/install_secure.sh)
does all of this, plus log rotation.

**macOS (launchd):**

```bash
sudo mkdir -p /usr/local/etc/tcpser && sudo cp /usr/local/share/tcpser/tcpser.conf /usr/local/etc/tcpser/
sudo nano /usr/local/etc/tcpser/tcpser.conf      # device = /dev/cu.usbserial-XXXX
sudo cp /usr/local/share/tcpser/com.tcpser.bbs.plist /Library/LaunchDaemons/
sudo launchctl load /Library/LaunchDaemons/com.tcpser.bbs.plist
```

Both restart tcpser if it exits. `kill -HUP` reopens the log (logrotate),
`kill -USR1` prints the counters.

## 9. Where to go next

- Every option and config key: [`configuration.md`](configuration.md)
- Hosting on the public Internet: [`SECURITY_GUIDE.md`](SECURITY_GUIDE.md), then fail2ban and firewall samples in [`../samples/`](../samples/README.md)
- Multi-line BBS: repeat `-d`/`-v` (or `[line]` sections) and route ports with `-p 23:0,1 -p 64128:2`
- Notifications: `-e https://discord.com/api/webhooks/...` (Discord, Slack, ntfy or plain JSON POST)
- Something is wrong: the Troubleshooting section of the [README](../README.md#troubleshooting)
