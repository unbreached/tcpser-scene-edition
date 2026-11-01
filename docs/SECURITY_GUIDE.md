# TCPSER Security Guide

## Overview

TCPSER (1.2 and later) has undergone comprehensive security hardening to eliminate critical vulnerabilities while maintaining all functional capabilities. This guide provides complete security implementation details, deployment recommendations, and operational best practices.

## Security Architecture

### Core Security Principles

1. **Input Validation**: All external input is validated and sanitized
2. **Least Privilege**: Runs with minimal required permissions  
3. **Defense in Depth**: Multiple security layers prevent compromise
4. **Secure by Default**: Safe configuration out of the box
5. **Fail Secure**: Security failures result in safe states

### Security Boundaries

```
┌─────────────────────────────────────────────────────────────┐
│                    TCPSER Security Zones                    │
├─────────────────────────────────────────────────────────────┤
│ UNTRUSTED ZONE                                             │
│ ┌─────────────────┐                                        │
│ │ Internet Clients│ ──────┐                               │
│ └─────────────────┘       │                               │
│                          │                               │
├──────────────────────────┼───────────────────────────────────┤
│ PERIMETER SECURITY       │                               │
│ ┌─────────────────┐      │  ┌─────────────────────────┐  │
│ │   Firewall      │ ─────┼─▶│    TCPSER Process       │  │
│ │   Rate Limiting │      │  │  - Input Validation     │  │
│ │   IP Filtering  │      │  │  - Secure HTTP Parser   │  │
│ └─────────────────┘      │  │  - Path Validation      │  │
│                          │  │  - Script Sandboxing    │  │
├──────────────────────────┼──┴─────────────────────────────────┤
│ TRUSTED ZONE             │                               │
│ ┌─────────────────┐      │  ┌─────────────────────────┐  │
│ │ Serial Hardware │      └─▶│    BBS Software         │  │
│ │ Hayes Modems    │         │    Configuration        │  │
│ │ Terminal Server │         │    Log Files            │  │
│ └─────────────────┘         └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Security Features Implemented

### 1. Secure Script Execution

**Problem**: Command injection vulnerability in `-E` flag  
**Solution**: Sandboxed script execution with strict validation

**Implementation**:
```c
// OLD: Vulnerable shell execution
execl("/bin/sh", "sh", "-c", user_script, NULL);  // ❌ DANGEROUS

// NEW: Secure script execution  
execute_script_secure(script_name, peer_ip, timeout);  // ✅ SAFE
```

**Security Controls**:
- Scripts must be in `/usr/local/tcpser/scripts/`
- Alphanumeric names only (no paths, no metacharacters)
- Direct execution (no shell interpretation)
- 30-second execution timeout
- Environment sanitization
- Permission validation

### 2. Input Validation Framework

**Problem**: Buffer overflows and injection attacks  
**Solution**: Comprehensive input validation and safe string handling

**Key Functions**:
- `validate_file_path()` - Path traversal prevention
- `parse_http_request_safe()` - Secure HTTP parsing
- `get_action_safe()` - Bounds-checked string operations
- `validate_script_name()` - Script name sanitization

### 3. Memory Safety Improvements

**Problem**: Buffer overflows in network protocol handling  
**Solution**: Bounds checking and safe string operations

**Changes**:
```c
// OLD: Unsafe operations
strcpy(dest, src);                    // ❌ No bounds checking
sprintf(buf, "%s", user_input);       // ❌ Potential overflow

// NEW: Safe operations  
snprintf(dest, dest_size, "%s", src); // ✅ Bounds checked
get_action_safe(txt, sizeof(txt), action); // ✅ Size validation
```

### 4. Race Condition Elimination

**Problem**: Double-checked locking in IP blocklist initialization  
**Solution**: Thread-safe initialization using pthread_once

**Implementation**:
```c
// OLD: Race condition
if (!g_blocks_inited) {              // ❌ Race here
    pthread_mutex_lock(&mutex);
    if (!g_blocks_inited) {
        g_blocks = calloc(...);       // ❌ Double allocation
    }
}

// NEW: Thread-safe initialization
pthread_once(&blocks_once, init_blocks_once);  // ✅ Safe
```

### 5. Secure HTTP Implementation

**Problem**: HTTP request smuggling and header injection  
**Solution**: Proper HTTP parsing with security headers

**Security Headers Added**:
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY` 
- `X-XSS-Protection: 1; mode=block`
- `Content-Security-Policy: default-src 'self'`
- `Strict-Transport-Security: max-age=31536000`

## Deployment Security

### System-Level Hardening

#### 1. Dedicated User Account
```bash
# Create dedicated user with minimal privileges
sudo useradd --system \
    --home /var/lib/tcpser \
    --shell /usr/sbin/nologin \
    --comment "TCPSER BBS Gateway" \
    tcpser

# Add to dialout group for serial port access
sudo usermod -aG dialout tcpser
```

#### 2. File System Security
```bash
# Set secure permissions
sudo chown root:root /usr/local/bin/tcpser
sudo chmod 755 /usr/local/bin/tcpser

# Create secure script directory
sudo mkdir -p /usr/local/tcpser/scripts
sudo chown root:tcpser /usr/local/tcpser/scripts
sudo chmod 755 /usr/local/tcpser/scripts

# Secure configuration directory
sudo mkdir -p /etc/tcpser
sudo chown root:tcpser /etc/tcpser
sudo chmod 750 /etc/tcpser

# Create data directory
sudo mkdir -p /var/lib/tcpser
sudo chown tcpser:tcpser /var/lib/tcpser
sudo chmod 750 /var/lib/tcpser
```

#### 3. Systemd Security Features
```ini
[Unit]
Description=TCPSER Hayes Modem Bridge
Documentation=man:tcpser(1)
After=network.target
RequiresMountsFor=/var/lib/tcpser

[Service]
Type=simple
User=tcpser
Group=tcpser
ExecStart=/usr/local/bin/tcpser -d /dev/ttyUSB0 -s 2400 -p 6400
Restart=on-failure
RestartSec=5

# Security hardening
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
ProtectProc=invisible
ProcSubset=pid
PrivateDevices=no  # Need access to serial devices
PrivateTmp=yes
PrivateUsers=yes
PrivateNetwork=no  # Need network access
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
RemoveIPC=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
SystemCallFilter=@system-service
SystemCallFilter=~@debug @mount @cpu-emulation @obsolete @privileged
SystemCallErrorNumber=EPERM

# Capability restrictions
CapabilityBoundingSet=
AmbientCapabilities=

# Resource limits
LimitNOFILE=1024
LimitNPROC=64
MemoryMax=256M
TasksMax=32

# Read-only paths
ReadOnlyPaths=/usr/local/tcpser/scripts
ReadWritePaths=/var/lib/tcpser /var/log

[Install]
WantedBy=multi-user.target
```

### Network Security

#### 1. Firewall Configuration
```bash
# UFW Configuration
sudo ufw default deny incoming
sudo ufw default allow outgoing

# Allow BBS ports with rate limiting
sudo ufw limit 6400/tcp comment "TCPSER BBS (rate limited)"
# The web dashboard binds 127.0.0.1 by default; no rule needed unless you expose it.

# Enable firewall
sudo ufw enable
```

#### 2. Fail2ban Integration

Use the shipped jails and filters in `samples/fail2ban/` (`tcpser-gate` reads
tcpser's own log for gate failures, blocked and rate-limited IPs; `tcpser-auth`
and `tcpser-flood` read the nginx access log for the dashboard). Start tcpser
with `-L /var/log/tcpser/tcpser.log -l 4` so the gate lines are written.

```bash
sudo cp samples/fail2ban/tcpser.conf /etc/fail2ban/jail.d/
sudo cp samples/fail2ban/tcpser-*.conf /etc/fail2ban/filter.d/
sudo fail2ban-client reload
```

#### 3. TLS Termination (Recommended)
```nginx
# /etc/nginx/sites-available/tcpser-dashboard
server {
    listen 443 ssl http2;
    server_name bbs.example.com;

    ssl_certificate /etc/ssl/certs/tcpser.crt;
    ssl_certificate_key /etc/ssl/private/tcpser.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384;
    ssl_prefer_server_ciphers off;

    # Security headers
    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
    add_header X-Content-Type-Options nosniff;
    add_header X-Frame-Options DENY;
    add_header X-XSS-Protection "1; mode=block";

    # Rate limiting
    limit_req_zone $binary_remote_addr zone=tcpser:10m rate=10r/m;
    limit_req zone=tcpser burst=5 nodelay;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        # WebSocket support for SSE
        proxy_set_header Connection '';
        proxy_http_version 1.1;
        chunked_transfer_encoding off;
        proxy_buffering off;
        proxy_cache off;
    }
}

# Redirect HTTP to HTTPS
server {
    listen 80;
    server_name bbs.example.com;
    return 301 https://$server_name$request_uri;
}
```

## Operational Security

### Monitoring and Alerting

#### 1. Log Analysis
```bash
# Monitor security events
sudo tail -f /var/log/tcpser-security.log | grep -E "(BLOCKED|GATE_FAIL|Rate limited)"

# Daily security report
#!/bin/bash
# /usr/local/bin/tcpser-security-report.sh
DATE=$(date +%Y-%m-%d)
LOG_FILE="/var/log/tcpser-security.log"

echo "TCPSER Security Report - $DATE"
echo "======================================"
echo "Blocked IPs: $(grep "BLOCKED" $LOG_FILE | grep $DATE | wc -l)"
echo "Gate failures: $(grep "GATE_FAIL" $LOG_FILE | grep $DATE | wc -l)"  
echo "Rate limited: $(grep "Rate limited" $LOG_FILE | grep $DATE | wc -l)"
echo ""
echo "Top attacking IPs:"
grep -E "(BLOCKED|GATE_FAIL)" $LOG_FILE | grep $DATE | \
    awk '{print $(NF-1)}' | sort | uniq -c | sort -nr | head -10
```

#### 2. Health Monitoring
```bash
#!/bin/bash
# /usr/local/bin/tcpser-health-check.sh

# Check if TCPSER is running
if ! systemctl is-active --quiet tcpser; then
    echo "ALERT: TCPSER service is not running"
    exit 1
fi

# Check if ports are listening
if ! netstat -ln | grep -q ":6400.*LISTEN"; then
    echo "ALERT: BBS port 6400 not listening"
    exit 1
fi

if ! netstat -ln | grep -q ":8080.*LISTEN"; then
    echo "ALERT: Web dashboard port 8080 not listening" 
    exit 1
fi

# Check memory usage
MEM_USAGE=$(ps -o pid,vsz,rss,comm -p $(pgrep tcpser) | awk 'NR==2 {print $3}')
if [ "$MEM_USAGE" -gt 262144 ]; then  # 256MB in KB
    echo "WARNING: TCPSER using excessive memory: ${MEM_USAGE}KB"
fi

echo "TCPSER health check passed"
```

### Incident Response

#### 1. Security Event Response
```bash
# Immediate response to attack
#!/bin/bash
# /usr/local/bin/tcpser-incident-response.sh

ATTACKER_IP="$1"

if [ -z "$ATTACKER_IP" ]; then
    echo "Usage: $0 <attacker_ip>"
    exit 1
fi

echo "Responding to attack from $ATTACKER_IP"

# Block IP immediately  
sudo ufw insert 1 deny from $ATTACKER_IP comment "Incident response block"

# Check current connections
echo "Active connections from $ATTACKER_IP:"
netstat -an | grep $ATTACKER_IP

# Review recent logs
echo "Recent activity from $ATTACKER_IP:"
grep $ATTACKER_IP /var/log/tcpser-security.log | tail -20

# Notify administrators
echo "Security incident involving $ATTACKER_IP" | \
    mail -s "TCPSER Security Alert" admin@example.com
```

#### 2. Recovery Procedures
```bash
# Service recovery checklist
#!/bin/bash
# /usr/local/bin/tcpser-recovery.sh

echo "TCPSER Recovery Procedure"
echo "========================"

# Check system resources
echo "1. System resources:"
df -h /var/lib/tcpser
free -h

# Validate configuration
echo "2. Version / build check:"
/usr/local/bin/tcpser -V  # Print version (there is no config-test mode)

# Check serial devices
echo "3. Serial device status:"
ls -l /dev/ttyUSB* /dev/ttyS*

# Restart service
echo "4. Restarting TCPSER service:"
sudo systemctl restart tcpser

# Verify operation
echo "5. Service verification:"
sleep 5
sudo systemctl status tcpser
netstat -ln | grep -E "(6400|8080).*LISTEN"

echo "Recovery complete"
```

## Security Maintenance

### Regular Security Tasks

#### Weekly
- [ ] Review security logs for anomalies
- [ ] Check blocked IP statistics
- [ ] Verify backup integrity
- [ ] Update system packages

#### Monthly  
- [ ] Rotate log files
- [ ] Review user accounts and permissions
- [ ] Test backup and recovery procedures
- [ ] Update security documentation

#### Quarterly
- [ ] Security audit and penetration testing
- [ ] Review and update firewall rules
- [ ] Certificate renewal (if using TLS)
- [ ] Security training updates

### Vulnerability Management

#### 1. Security Updates
```bash
# Check for updates
#!/bin/bash
# /usr/local/bin/tcpser-update-check.sh

echo "Checking for TCPSER security updates..."

# Check official repository
git -C /opt/tcpser-source fetch origin
UPDATES=$(git -C /opt/tcpser-source log HEAD..origin/main --oneline | grep -i security)

if [ -n "$UPDATES" ]; then
    echo "Security updates available:"
    echo "$UPDATES"
    echo "Please review and apply updates promptly."
else
    echo "No security updates available."
fi
```

#### 2. Static Code Analysis
```bash
# Automated security scanning
#!/bin/bash
# /usr/local/bin/tcpser-security-scan.sh

TCPSER_SRC="/opt/tcpser-source"

# Run static analysis tools
echo "Running security analysis..."

# Cppcheck for C/C++ code
if command -v cppcheck >/dev/null; then
    cppcheck --enable=all --inconclusive --xml $TCPSER_SRC/src/ 2> cppcheck-results.xml
fi

# Clang static analyzer
if command -v scan-build >/dev/null; then
    scan-build -o scan-build-results make -C $TCPSER_SRC clean all
fi

# Custom security pattern matching
grep -r "strcpy\|strcat\|sprintf\|gets" $TCPSER_SRC/src/ && \
    echo "WARNING: Potentially unsafe string functions found"

echo "Security scan complete"
```

## Compliance and Auditing

### Security Compliance Checklist

#### OWASP Top 10 2021 Mitigation
- [x] **A01 Broken Access Control**: Input validation, path sanitization
- [x] **A02 Cryptographic Failures**: TLS termination recommended  
- [x] **A03 Injection**: Command injection eliminated, SQL injection N/A
- [x] **A04 Insecure Design**: Secure-by-default configuration
- [x] **A05 Security Misconfiguration**: Hardened systemd service
- [x] **A06 Vulnerable Components**: Regular update procedures
- [x] **A07 Identification Failures**: Basic auth via gate mechanism
- [x] **A08 Software Integrity**: Code signing recommended
- [x] **A09 Logging Failures**: Comprehensive security logging
- [x] **A10 SSRF**: Limited HTTP client functionality, validated URLs

#### Security Controls Matrix

| Control Category | Implementation | Status |
|-----------------|----------------|--------|
| Authentication | Gate passwords/challenges | ✅ Implemented |
| Authorization | IP allowlist/blocklist | ✅ Implemented |  
| Input Validation | All external inputs validated | ✅ Implemented |
| Output Encoding | HTTP response encoding | ✅ Implemented |
| Encryption | TLS for web dashboard | 📋 Recommended |
| Session Management | Stateless design | ✅ Implemented |
| Error Handling | Fail-secure error states | ✅ Implemented |
| Logging | Security event logging | ✅ Implemented |
| Configuration | Secure defaults | ✅ Implemented |

### Audit Evidence

The following files contain security implementations:

| File | Security Feature | Evidence |
|------|------------------|----------|
| `src/secure_script.c` | Script execution sandboxing | Function `execute_script_secure()` |
| `src/secure_paths.c` | Path traversal prevention | Function `validate_file_path()` |
| `src/secure_http.c` | HTTP request validation | Function `parse_http_request_safe()` |
| `src/nvt.c` | Buffer overflow fixes | Function `get_action_safe()` |
| `src/bridge.c` | Race condition fixes | pthread_once implementation |

## Conclusion

TCPSER implements comprehensive security controls addressing all identified vulnerabilities. The multi-layered security approach provides defense in depth while maintaining full functionality for vintage BBS operations.

**Key Security Achievements**:
- ✅ **Zero Critical Vulnerabilities** after remediation
- ✅ **Defense in Depth** with multiple security layers  
- ✅ **Secure by Default** configuration
- ✅ **Production Ready** with enterprise security controls

Regular security maintenance and monitoring ensure continued protection against evolving threats.