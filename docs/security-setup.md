# TCPSER Security Setup Guide

> Quick secure-install companion. The authoritative security reference is
> [`SECURITY_GUIDE.md`](SECURITY_GUIDE.md); see
> [`README.md`](README.md) for the full documentation index.

## Secure Script Execution

TCPSER now implements **secure script execution** to prevent command injection vulnerabilities while still allowing automation after successful gate authentication.

### 🔒 Security Features

1. **Restricted Directory**: Scripts must be in `/usr/local/tcpser/scripts/`
2. **Name Validation**: Only alphanumeric, dash, underscore, and dot allowed
3. **No Shell Execution**: Scripts run directly (no `sh -c`)
4. **Permission Checks**: Scripts must be executable but not world-writable
5. **Timeout Protection**: 30-second execution limit
6. **Size Limits**: Scripts limited to 1MB
7. **Environment Sanitization**: Dangerous variables removed

### 📁 Setup Script Directory

```bash
# Create script directory
sudo mkdir -p /usr/local/tcpser/scripts
sudo chown root:root /usr/local/tcpser/scripts
sudo chmod 755 /usr/local/tcpser/scripts
```

### 📝 Example Scripts

#### Notification Script (`notify-connection.sh`)
```bash
#!/bin/bash
# Script runs when gate is passed successfully
# Environment: TCPSER_PEER_IP contains client IP

logger "TCPSER: Successful connection from ${TCPSER_PEER_IP:-unknown}"
echo "Connection from ${TCPSER_PEER_IP}" >> /var/log/tcpser-connections.log

# Optional: Send notification
# curl -X POST "https://ntfy.sh/your-topic" -d "BBS connection from ${TCPSER_PEER_IP}"
```

#### Sound Alert Script (`play-sound.sh`)
```bash
#!/bin/bash
# Play connection sound
/usr/bin/aplay /usr/local/tcpser/sounds/connection.wav 2>/dev/null &
```

#### Discord Webhook Script (`discord-notify.sh`)
```bash
#!/bin/bash
# Send Discord notification
WEBHOOK_URL="https://discord.com/api/webhooks/YOUR_WEBHOOK_URL"
IP="${TCPSER_PEER_IP:-unknown}"

curl -H "Content-Type: application/json" \
     -X POST \
     -d "{\"content\":\"🖥️ BBS connection from \`${IP}\`\"}" \
     "${WEBHOOK_URL}" 2>/dev/null &
```

### 🔧 Installation

```bash
# Make scripts executable
sudo chmod +x /usr/local/tcpser/scripts/*.sh

# Production usage: -E runs the named script (from the script dir) after a
# successful gate. Pass the script's basename only; tcpser validates the name
# (alphanumeric, dash, underscore, dot) and the location for you.
./tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -k "secret" -E "notify-connection.sh"
```

### ⚠️ Security Guidelines

#### ✅ SAFE Script Names
- `notify.sh`
- `connection-alert.py`
- `sound_player`
- `webhook-2.0.sh`

#### ❌ UNSAFE Script Names
- `../../../etc/passwd` (path traversal)
- `script; rm -rf /` (command injection)
- `.hidden-script` (hidden files)
- `script with spaces` (contains spaces)

#### ✅ Safe Script Content
```bash
#!/bin/bash
# Use full paths
/usr/bin/logger "Connection from ${TCPSER_PEER_IP}"

# Validate input if processing IP
if [[ "${TCPSER_PEER_IP}" =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]]; then
    echo "Valid IP: ${TCPSER_PEER_IP}" >> /var/log/tcpser.log
fi
```

#### ❌ Unsafe Script Content
```bash
#!/bin/bash
# DON'T DO THIS - vulnerable to injection
eval "echo Connection from ${TCPSER_PEER_IP}"

# DON'T DO THIS - uses unvalidated input
curl "http://api.example.com/log?ip=${TCPSER_PEER_IP}"
```

### 🚨 Migration from Old -E Flag

**Old vulnerable usage:**
```bash
# OLD: DANGEROUS - allows command injection
tcpser -E "logger 'Connection'; touch /tmp/pwned"
```

**New secure usage:**
```bash
# NEW: SECURE - script name only
tcpser -E "log-connection.sh"
```

### 🛡️ Additional Hardening

1. **Run as dedicated user:**
```bash
sudo useradd --system --home /var/lib/tcpser tcpser
sudo chown tcpser:tcpser /usr/local/tcpser/scripts
```

2. **Use systemd restrictions:**
```ini
[Service]
User=tcpser
ProtectSystem=strict
ProtectHome=true
NoNewPrivileges=true
ReadOnlyPaths=/usr/local/tcpser/scripts
```

3. **Firewall protection:**
```bash
# Allow only necessary ports
iptables -A INPUT -p tcp --dport 6400 -j ACCEPT
iptables -A INPUT -p tcp --dport 8080 -j ACCEPT  # Web dashboard
```

### 🔍 Troubleshooting

**Script not found:**
```
ERROR: Script not found or not executable: myscript.sh
```
- Check script exists in `/usr/local/tcpser/scripts/`
- Verify script is executable: `chmod +x script.sh`

**Invalid script name:**
```
ERROR: Invalid character in script name: '/'
```
- Use only alphanumeric, dash, underscore
- No paths allowed - filename only

**Permission denied:**
```
ERROR: Script is world-writable (insecure): script.sh
```
- Remove world-write: `chmod o-w script.sh`

### 📊 Security Comparison

| Feature | Old -E Flag | New Secure -E |
|---------|-------------|---------------|
| Script location | Anywhere | `/usr/local/tcpser/scripts/` only |
| Shell execution | Yes (vulnerable) | No (direct exec) |
| Path traversal | Possible | Prevented |
| Command injection | Possible | Prevented |
| Input validation | None | Strict |
| Timeout protection | None | 30 seconds |
| Permission checks | None | Enforced |

This secure implementation maintains the functionality you need while eliminating the critical security vulnerabilities.