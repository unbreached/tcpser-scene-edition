# TCPSER documentation

One path through the docs, in reading order:

| Read | When |
|------|------|
| [`../README.md`](../README.md) | what tcpser is, features, dashboards, security summary, troubleshooting, AT commands |
| [`getting-started.md`](getting-started.md) | **you have a C64 and a cable and want a first call**: build, try it without hardware, analyzer, host, gate, dashboards, service (Pi and Mac) |
| [`configuration.md`](configuration.md) | the config file, `tcpser check`, precedence, multi-line routing, every option (generated from the binary) |
| [`SECURITY_GUIDE.md`](SECURITY_GUIDE.md) | **hosting on the public Internet**: threat model, the gate, blocklist/allowlist, `-E` hook sandbox, web console exposure, systemd/firewall/fail2ban hardening. Authoritative when two documents disagree |
| [`security-setup.md`](security-setup.md) | condensed secure-install walkthrough and the `-E` script rules |
| [`../samples/README.md`](../samples/README.md) · [`../samples/QUICK_START.md`](../samples/QUICK_START.md) | the shipped systemd units, installer, nginx + TLS, fail2ban, firewall and monitoring scripts |
| [`../c64/README.md`](../c64/README.md) | the C64-side link analyzer (6551 registers, cable lines, send AT from the C64) |
| `man tcpser` · `tcpser -h` | the manual page and built-in help, both generated from the same option table |
| [`AUDIT-2026-11-01.md`](AUDIT-2026-11-01.md) | the full security / stability / UX audit and the roadmap; the status block at the top says what is done |
| [`../CHANGES`](../CHANGES) · [`SECURITY_CHANGELOG.md`](SECURITY_CHANGELOG.md) | release notes and the security history |

Quick answers:

- **Just run it:** `./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x`, see [getting started](getting-started.md#5-host-the-bbs).
- **Which cartridge / cable settings?** `./tcpser -d /dev/ttyUSB0 -s 2400 -Z`.
- **Config file template:** `tcpser help config`; validate with `tcpser check --config FILE`.
- **Exposing the web console?** Only behind nginx + TLS with a token: [SECURITY_GUIDE.md](SECURITY_GUIDE.md).
- **Where are the screenshots?** [`img/`](img/) (web console screens, terminal dashboard, C64 tool).
