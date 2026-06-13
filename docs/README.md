# TCPSER Documentation Index

Start here to find the right document.

| Document | What it covers |
|----------|----------------|
| [`../README.md`](../README.md) | Main manual: features, all options, examples, AT commands, dashboard, recovery, troubleshooting. **Read this first.** |
| [`SECURITY_GUIDE.md`](SECURITY_GUIDE.md) | **Canonical security reference** — threat model, hardening, the gate, blocklist/allowlist, `-E` script execution, systemd/firewall/fail2ban, web dashboard exposure. |
| [`../SECURITY_SETUP.md`](../SECURITY_SETUP.md) | Quick "secure install" walkthrough (a condensed companion to the guide). |
| [`../SECURITY_CHANGELOG.md`](../SECURITY_CHANGELOG.md) | History of security-relevant changes. |
| [`../samples/README.md`](../samples/README.md) | systemd / fail2ban / firewall / nginx sample configs and how to use them. |
| [`../samples/QUICK_START.md`](../samples/QUICK_START.md) | Fastest path to a hardened deployment. |
| [`../man/tcpser.1`](../man/tcpser.1) | Man page (`man tcpser` after `make install`). Mirrors `tcpser -h`. |
| [`../LICENSE`](../LICENSE) | GPL-2.0. |

## Quick links

- **Just want to run it?** → main README ["Quick Start"](../README.md#quick-start)
- **Hosting on the public Internet?** → [`SECURITY_GUIDE.md`](SECURITY_GUIDE.md) and enable the gate (`-k`/`-x`).
- **Exposing the web dashboard (`-H`)?** → set `TCPSER_DASH_BIND` and `TCPSER_DASH_TOKEN` (see main README, "Web Dashboard").

> Note: `SECURITY_SETUP.md` and the `samples/` guides intentionally repeat a
> few steps from `SECURITY_GUIDE.md` so each can be followed standalone. When
> they disagree, **`docs/SECURITY_GUIDE.md` is authoritative.**
