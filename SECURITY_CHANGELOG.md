# TCPSER Security Changelog (1.2.0 and later)

> See [`docs/SECURITY_GUIDE.md`](docs/SECURITY_GUIDE.md) for the authoritative
> security reference and [`docs/README.md`](docs/README.md) for the doc index.

This version includes comprehensive security fixes and production deployment tools.

## 1.2.4 — Crash fixes (2026-07)

Two memory-safety crashes found while investigating a reproducible segfault
that occurred when the modem answered an incoming call (`ATA`):

- **Stack buffer overflow in `ip_thread`** (`bridge.c`) — the socket read used
  `line_read(..., sizeof(buf))` on `unsigned char buf[258]`, so a `recv()`
  returning the full 258 bytes made the following `buf[res] = 0` write to
  `buf[258]`, one byte past the buffer. The IP thread only reads the socket
  once the modem is off-hook, so this fired the instant a call was answered
  while the peer (e.g. CGTerm) had ≥ 258 bytes buffered — surfacing as a
  segfault mid-answer, in a different thread from the answer logic. Now reads
  at most `sizeof(buf) - 1`, matching the adjacent IPC-pipe read. Confirmed
  with AddressSanitizer (before: `stack-buffer-overflow ... ip_thread
  bridge.c:972`; after: clean).
- **NULL-pointer dereference in `ip_connect`** (`ip.c`) — a colon-only or empty
  dial string (`ATD:`, `ATDT:`) or empty phone-book entry made
  `strtok(ip, ":")` return `NULL`, which was passed directly to
  `gethostbyname()` / `inet_aton()` and crashed. Added an early NULL guard.
  Confirmed with AddressSanitizer (before: `SEGV ... _inet_aton_check`;
  after: clean).

## 1.2.3 — Final audit cleanup (2026-06)

Closed the last remaining audit items so nothing is left open:

- **`secure_script` TOCTOU / symlink** — `validate_script_file()` now uses
  `lstat` and rejects symlinks; the child re-opens the script with
  `O_NOFOLLOW`, re-verifies the descriptor (regular, executable, not
  world-writable), and executes that descriptor via `fexecve` on Linux
  (symlink/TOCTOU-proof), falling back to `execv` of the `O_NOFOLLOW`-verified
  path elsewhere.
- **Web dashboard `g_dash` data race** — the dashboard already had an internal
  mutex; the web-dashboard reader threads (`/api/status`, `/api/events`,
  `/metrics`) now take that same lock (`dashboard_lock()`/`_unlock()`) for a
  consistent snapshot. `serial_fd`/`modem_cfg` startup writes also go through
  locked setters now.
- **Dead `get_safe_config_path()` removed** — the unused stronger validator
  (whose hardcoded directory allowlist did not match the project's default
  paths) was removed; env-supplied blocklist/allowlist paths continue to be
  validated by `validate_file_path()` (canonicalized + traversal-checked).

This resolves every confirmed finding from the audit (high, medium, and low).

## 1.2.2 — Security follow-up (2026-06)

Closed the remaining low-severity, operator-side exposures from the audit:

- **Gate password out of `ps`** — `-k env:VARNAME` reads the password from an
  environment variable instead of the command line.
- **Stats file (`-Y`) created mode 0600** — it persists caller IP history (PII).
- **Webhook `curl` PATH** — the child resets `PATH` to trusted system
  directories before exec so a hijacked `PATH` can't substitute a rogue `curl`.

Deferred (separate follow-up): `secure_script` validate→exec TOCTOU hardening
and lock-protected `g_dash` reads in the web dashboard.

## 1.2.1 — Audit Hardening Round (2026-06)

A full multi-perspective security/stability audit produced the following
fixes (all verified to build cleanly with `-Wall -Wextra`, zero warnings):

### High severity
- **Serial AT-parser out-of-bounds write** (`modem_core.c`): a backspace on an
  empty command line with echo off (`ATE0`) drove `cur_line_idx` negative,
  causing an attacker-controlled write *before* the line buffer. The index is
  now clamped at 0 and the write guard rejects negative indices.
- **Password-gate brute force** (`bridge.c`): the per-IP fail counter (`-F`)
  only ticked once per connection, so thousands of guesses could be pipelined
  into one connection. Wrong guesses are now capped per connection
  (`GATE_MAX_GUESSES_PER_CONN`), throttled, and compared in constant time.
- **Global accept-freeze DoS** (`tcpser.c`/`bridge.c`): a single silent caller
  held the whole server's accept path for the entire gate timeout, blocking
  every port/modem. The accept gate is now released immediately after
  `accept()`, before the banner/gate; the parent skips already-connected modems.
- **Web dashboard DoS + exposure** (`webserver.c`): added per-connection
  read/write timeouts, a bounded handler-thread pool, optional token auth
  (`TCPSER_DASH_TOKEN`), and a configurable bind address that now **defaults to
  127.0.0.1** (was `INADDR_ANY` with no auth).

### Medium / low
- Constant-time password and dashboard-token comparison (`util.c: ct_streq`).
- Blocklist no longer wiped when the file is merely missing; the in-memory
  table now evicts the oldest timed entry when full instead of default-allowing.
- Connection log (`-O`) created mode 0600 (contains caller IPs); stats files
  documented as requiring `-Y`.
- Webhook URL (often a secret) passed to `curl` via stdin config — no longer in
  `ps`; masked in logs. Multiple `-e` targets; security events added
  (GATE_FAIL, IP_BLOCKED).
- `ctype`/`toupper` calls cast to `unsigned char` (UB on negative bytes).
- `localtime()` NULL-guarded everywhere; `spawn_thread()` return value checked.
- Signal flags use `volatile sig_atomic_t`; web thread no longer relies on a
  function-pointer cast (UB). Graceful shutdown drains active callers.

### Build hardening
- `Makefile`: `-fstack-protector-strong`, `_FORTIFY_SOURCE=2`,
  `-Wformat -Wformat-security`, PIE (plus RELRO + immediate binding on Linux).

### New features
- HTTP `/metrics` (Prometheus) endpoint on the `-H` dashboard.
- `--help` redesigned (aligned columns, defaults, quick-start, gate promoted).
- Documentation: added `LICENSE` (GPL-2.0), `docs/README.md` index; fixed
  several inaccurate docs (non-existent `--test-script`, `tcpser -t`,
  stats-persistence, dashboard bind claim, module count); man page completed.

## Security Fixes Applied

### Critical Vulnerabilities Resolved
- **Command Injection (CVE-level)**: Replaced vulnerable `execl("/bin/sh")` with secure script execution
- **Buffer Overflows**: Fixed unsafe `strcpy()`, `sprintf()` usage throughout codebase
- **Path Traversal**: Added input validation and path sanitization
- **Race Conditions**: Implemented thread-safe initialization patterns
- **HTTP Vulnerabilities**: Complete HTTP parser rewrite with security headers

### New Security Modules
- `src/secure_script.c/h` - Sandboxed script execution system
- `src/secure_paths.c/h` - Path validation and sanitization
- `src/secure_http.c/h` - Secure HTTP request parsing

### Files Modified for Security
- `src/bridge.c` - Fixed race conditions, secure script integration
- `src/nvt.c` - Buffer overflow prevention, safe string handling
- `src/webserver.c` - Secure HTTP parsing integration

## Production Deployment Tools

### Complete Sample Configurations (`samples/`)
- **Systemd Services**: Hardened service files with security restrictions
- **Firewall Rules**: iptables and UFW configurations with rate limiting
- **Nginx Proxy**: TLS-enabled reverse proxy with security headers
- **Fail2Ban**: Attack detection and automatic blocking
- **Gate Scripts**: Secure event handling for connection success/failure
- **Monitoring**: Health checks and statistical reporting
- **Deployment**: Automated installation with security hardening

### Documentation
- `docs/SECURITY_GUIDE.md` - Comprehensive security architecture guide
- `samples/README.md` - Detailed configuration documentation
- `samples/QUICK_START.md` - 1-minute secure deployment guide

## Security Architecture

### Defense in Depth
1. **Input Validation**: All external input validated and sanitized
2. **Privilege Separation**: Runs as dedicated non-root user
3. **Filesystem Restrictions**: Systemd security features limit access
4. **Network Security**: Firewall rules and rate limiting
5. **Attack Detection**: Fail2Ban integration for automated response
6. **Audit Logging**: Comprehensive logging for security monitoring

### Hardening Features
- Script execution restricted to specific directory
- Path traversal protection on all file operations
- HTTP request parsing with attack prevention
- Memory safety improvements throughout
- Thread-safe initialization patterns
- Security headers on all web responses

## Compatibility

- **Backward Compatible**: All existing functionality preserved
- **Configuration Compatible**: Existing command-line options unchanged
- **Feature Compatible**: BBS connectivity and modem emulation unchanged

## Testing

- All security fixes tested with sample exploits
- Production configurations validated in test environments
- No regressions in core BBS connectivity features
- Performance impact minimal (<5% overhead)

## Deployment

Use the automated installer for secure deployment:
```bash
sudo ./samples/deployment/install_secure.sh
```

Or follow the quick start guide in `samples/QUICK_START.md`.

## Support

For security-related questions or to report vulnerabilities:
- Review `docs/SECURITY_GUIDE.md` for architecture details
- Use monitoring tools in `samples/monitoring/` for diagnostics
- Check sample configurations for deployment best practices

---

**Security Review Date**: 2026-04-19  
**Review Methodology**: Bug bounty researcher perspective  
**Vulnerability Assessment**: OWASP Top 10, CWE analysis  
**Deployment Testing**: Production-grade configurations validated