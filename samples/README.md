# TCPSER Sample Configurations and Scripts

This directory contains sample configurations, scripts, and deployment tools for TCPSER with security hardening.

## Directory Structure

```
samples/
├── deployment/         # Automated installation scripts
├── fail2ban/          # Fail2Ban configurations
├── firewall/          # Firewall rules (iptables, UFW)
├── monitoring/        # Health checks and statistics
├── nginx/             # Web server configurations
├── scripts/           # Gate success/failure scripts
└── systemd/           # Systemd service files
```

## Quick Start

1. **Secure Installation:**
   ```bash
   sudo ./deployment/install_secure.sh
   ```

2. **Configure Firewall:**
   ```bash
   # For Ubuntu/Debian:
   sudo ./firewall/ufw-rules.sh
   
   # For other Linux:
   sudo ./firewall/iptables-rules.sh
   ```

3. **Set Up Web Dashboard (optional):**
   ```bash
   # Copy nginx config
   sudo cp nginx/tcpser.conf /etc/nginx/sites-available/
   sudo ln -s /etc/nginx/sites-available/tcpser.conf /etc/nginx/sites-enabled/
   sudo nginx -t && sudo systemctl reload nginx
   ```

## Systemd Service Files

### `systemd/tcpser.service`
Full-featured service with maximum security hardening:
- Systemd security features enabled
- Restricted filesystem access
- Limited system calls
- Runs as dedicated user

**Installation:**
```bash
sudo cp systemd/tcpser.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable tcpser
sudo systemctl start tcpser
```

### `systemd/tcpser-basic.service`
Simplified service for basic setups without advanced security features.

## Firewall Configurations

### `firewall/iptables-rules.sh`
Comprehensive iptables rules with:
- Rate limiting for SSH and TCPSER
- Web dashboard restricted to localhost
- Logging of dropped packets

**Usage:**
```bash
sudo ./firewall/iptables-rules.sh
# To make persistent:
sudo iptables-save > /etc/iptables/rules.v4
```

### `firewall/ufw-rules.sh`
UFW (Ubuntu Firewall) configuration with same security features.

**Usage:**
```bash
sudo ./firewall/ufw-rules.sh
sudo ufw status verbose
```

## Nginx Web Proxy

### `nginx/tcpser.conf`
Production-ready nginx configuration featuring:
- TLS termination with strong ciphers
- Security headers (HSTS, CSP, etc.)
- Rate limiting
- Basic authentication support
- Server-Sent Events support for dashboard

**Setup:**
1. Obtain TLS certificate (Let's Encrypt recommended)
2. Update server_name and certificate paths
3. Install and reload nginx

## Fail2Ban Protection

### `fail2ban/tcpser.conf`
Jail configuration for:
- Authentication failure protection
- Request flooding protection

### `fail2ban/tcpser-auth.conf` & `fail2ban/tcpser-flood.conf`
Filter definitions for detecting attack patterns.

**Installation:**
```bash
sudo cp fail2ban/*.conf /etc/fail2ban/jail.d/
sudo cp fail2ban/tcpser-*.conf /etc/fail2ban/filter.d/
sudo systemctl restart fail2ban
```

## Gate Scripts

These scripts are executed when connections pass or fail the security gate. Place in `/usr/local/tcpser/scripts/` and use with the `-E` option.

### `scripts/gate_success_log`
Basic logging of successful gate passes:
- Logs to `/var/log/tcpser/gate_success.log`
- Sends to syslog
- Input validation for IP addresses

### `scripts/gate_success_notify`
Enhanced notifications for successful connections:
- Slack webhook integration
- Email notifications
- Daily statistics tracking

**Configuration:**
Edit the script to set webhook URLs and email addresses.

### `scripts/gate_fail_security`
Security response to gate failures:
- Detailed failure logging
- Automatic temporary banning after repeated failures
- Administrator alerts
- Expired ban cleanup

## Monitoring Scripts

### `monitoring/health_check.sh`
Comprehensive health check script:
- Process and port status
- Connection testing
- Memory usage monitoring
- Serial device validation
- Log file status

**Usage:**
```bash
./monitoring/health_check.sh [serial_device]
```

**Exit Codes:**
- 0: Healthy
- 1: Warning (minor issues)
- 2: Critical (major issues)

**Cron Integration:**
```bash
# Add to crontab for automated monitoring
*/5 * * * * /usr/local/tcpser/monitoring/health_check.sh /dev/ttyUSB0
```

### `monitoring/stats_report.sh`
Statistical reporting from TCPSER logs:
- Connection statistics
- Top client IPs
- Hourly distribution analysis
- System health summary
- Recent error analysis

**Usage:**
```bash
# Daily report
./monitoring/stats_report.sh

# Weekly report  
./monitoring/stats_report.sh -w

# Monthly report
./monitoring/stats_report.sh -m

# Custom period
./monitoring/stats_report.sh -d 14
```

## Deployment

### `deployment/install_secure.sh`
Automated secure installation script:
- Dependency installation
- User/group creation
- Directory setup with proper permissions
- Systemd service installation
- Log rotation configuration
- Security hardening

**Usage:**
```bash
# Run from TCPSER source directory
sudo ./samples/deployment/install_secure.sh
```

## Security Considerations

### Default Configuration
- TCPSER runs as dedicated non-privileged user
- Web dashboard (`-H`) binds to 127.0.0.1 by default; set
  `TCPSER_DASH_BIND` to expose it and `TCPSER_DASH_TOKEN` to require auth
- Systemd security features enabled
- Logs rotated automatically

### Network Security
- Firewall rules limit access
- Rate limiting prevents abuse
- Fail2Ban protects against attacks
- TLS encryption for web dashboard

### Script Security
- Gate scripts validate input
- No shell injection vulnerabilities
- Restricted execution directory
- Timeout protection

### Monitoring
- Health checks detect issues
- Statistics track usage patterns
- Failed connections are logged
- Automated alerting available

## Customization

### Modifying Gate Scripts
1. Copy sample scripts to `/usr/local/tcpser/scripts/`
2. Customize logic for your environment
3. Ensure scripts remain executable
4. Test with sample IPs before deployment

### Firewall Rules
1. Review port configurations in scripts
2. Adjust rate limits for your usage
3. Add additional allowed IPs if needed
4. Test rules in non-production environment

### Monitoring Thresholds
1. Adjust health check timeouts
2. Modify alert thresholds
3. Customize notification methods
4. Set appropriate monitoring intervals

## Troubleshooting

### Service Won't Start
1. Check systemd logs: `journalctl -u tcpser`
2. Verify serial device permissions
3. Check port conflicts
4. Review configuration file syntax

### Firewall Issues
1. Check rule application: `iptables -L` or `ufw status`
2. Test connectivity: `nc -z localhost 6400`
3. Review fail2ban logs: `fail2ban-client status`

### Web Dashboard Problems
1. Check nginx configuration: `nginx -t`
2. Verify upstream connectivity: `curl localhost:8080`
3. Review nginx error logs
4. Check TLS certificate validity

### Performance Issues
1. Monitor memory usage: `ps aux | grep tcpser`
2. Check connection logs for patterns
3. Review rate limiting settings
4. Analyze statistics reports

## Support

For additional help:
- Review the main TCPSER documentation
- Check system logs for error messages
- Use the health check script for diagnostics
- Monitor connection logs for patterns

Remember to keep your system updated and review security configurations regularly.