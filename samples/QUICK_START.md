# TCPSER Quick Setup Guide

This guide gets you up and running with secure TCPSER in minutes.

## Prerequisites

- Linux system with systemd
- Serial device (e.g., `/dev/ttyUSB0`)
- Root/sudo access

## 1-Minute Setup

```bash
# 1. Install TCPSER with security hardening
sudo ./deployment/install_secure.sh

# 2. Set up firewall (choose one)
sudo ./firewall/ufw-rules.sh        # Ubuntu/Debian
sudo ./firewall/iptables-rules.sh   # Other Linux

# 3. Start the service
sudo systemctl start tcpser
sudo systemctl status tcpser

# 4. Test connection
telnet localhost 6400

# 5. View web dashboard
curl http://localhost:8080
```

## Configuration Locations

- **Service Config:** `/etc/tcpser/tcpser.conf`
- **Scripts:** `/usr/local/tcpser/scripts/`
- **Logs:** `/var/log/tcpser/`
- **Data Files:** `/usr/local/share/tcpser/`

## Common Tasks

### Change Serial Device
```bash
sudo nano /etc/systemd/system/tcpser.service
# Edit ExecStart line: -d /dev/ttyUSB0
sudo systemctl daemon-reload
sudo systemctl restart tcpser
```

### Enable Web Dashboard Security
```bash
# Set up nginx proxy with TLS
sudo cp nginx/tcpser.conf /etc/nginx/sites-available/
# Edit server_name and certificate paths
sudo ln -s /etc/nginx/sites-available/tcpser.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

### Add Fail2Ban Protection
```bash
sudo cp fail2ban/*.conf /etc/fail2ban/jail.d/
sudo cp fail2ban/tcpser-*.conf /etc/fail2ban/filter.d/
sudo systemctl restart fail2ban
```

### Monitor System Health
```bash
# Run health check
./monitoring/health_check.sh /dev/ttyUSB0

# Generate statistics report
./monitoring/stats_report.sh -d 7  # Last 7 days
```

## Troubleshooting

### Service Won't Start
```bash
# Check status and logs
sudo systemctl status tcpser
sudo journalctl -u tcpser -f

# Check serial device
ls -la /dev/ttyUSB0
sudo usermod -a -G dialout tcpser
```

### Can't Connect
```bash
# Check if port is listening
sudo netstat -tlnp | grep 6400

# Test firewall
sudo ufw status
telnet localhost 6400
```

### Permission Issues
```bash
# Fix ownership
sudo chown -R tcpser:tcpser /var/log/tcpser
sudo chown -R tcpser:tcpser /usr/local/tcpser/scripts
```

## Security Checklist

- ✅ Service runs as non-root user
- ✅ Firewall rules applied
- ✅ Web dashboard binds localhost by default (set TCPSER_DASH_TOKEN if exposing)
- ✅ Log rotation configured
- ✅ Gate scripts installed
- ⬜ TLS certificate for web dashboard
- ⬜ Fail2Ban protection enabled
- ⬜ Regular monitoring scheduled

## Next Steps

1. Customize gate scripts for your environment
2. Set up automated monitoring with cron
3. Configure notifications for gate events
4. Review and adjust firewall rules
5. Set up TLS for production web dashboard

For detailed information, see `README.md` in this directory.