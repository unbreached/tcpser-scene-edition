# Gate hook scripts (`-E NAME`)

tcpser can run ONE hook after a caller passes the bot gate: `-E NAME`, where
NAME is a script in `/usr/local/tcpser/scripts/` (letters, digits, `-`, `_`,
`.` only; no path). There is no gate-*failure* hook: failed callers are
counted, rate-limited and blocked by tcpser itself (`-F`, blocklist).

How the hook is invoked:

| What                | Value                                               |
|---------------------|-----------------------------------------------------|
| Arguments           | none                                                |
| `TCPSER_PEER_IP`    | caller IP (dotted quad)                             |
| `TCPSER_SCRIPT_DIR` | the script directory                                |
| `PATH`              | reset to `/usr/local/bin:/usr/bin:/bin`             |
| Execution           | detached from the connection thread; killed after 30 s |
| Result              | logged as `tcpser[hook]: NAME: ...` in the tcpser log |

Requirements enforced by tcpser: regular file, not a symlink, executable,
not world-writable, under 1 MB. Keep hooks owned by root (`root:tcpser`,
mode 750) so a compromised service account cannot edit them.

Samples here: `gate_success_log` (append to a log + syslog) and
`gate_success_notify` (Slack/Discord/email via env vars).
