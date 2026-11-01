#!/bin/bash
# TCPSER Firewall Rules (UFW - Ubuntu)
# Run with: sudo bash ufw-rules.sh            (adds rules to the current set)
#           sudo bash ufw-rules.sh --reset    (wipes ALL existing ufw rules first)

set -eu

# Configuration
TCPSER_PORT=6400
WEB_PORT=8080
SSH_PORT=22

echo "Setting up UFW rules for TCPSER..."

# Reset only on explicit request: 'ufw --force reset' used to run
# unconditionally and destroyed every other service's rules on the host.
if [[ "${1:-}" == "--reset" ]]; then
    echo "Resetting all existing UFW rules (--reset given)..."
    ufw --force reset
fi

# Default policies
ufw default deny incoming
ufw default allow outgoing

# Allow SSH
ufw allow $SSH_PORT/tcp comment 'SSH'

# Allow TCPSER modem port
ufw allow $TCPSER_PORT/tcp comment 'TCPSER Modem'

# Web dashboard: tcpser binds it to 127.0.0.1 by default, so no rule is needed
# and loopback is implicitly allowed by ufw. If you expose it with
# TCPSER_DASH_BIND, restrict it to your LAN instead, e.g.:
# ufw allow from 192.168.1.0/24 to any port $WEB_PORT proto tcp comment 'TCPSER Dashboard (LAN)'

# Allow HTTP/HTTPS if using nginx proxy (uncomment if needed)
# ufw allow 80/tcp comment 'HTTP'
# ufw allow 443/tcp comment 'HTTPS'

# Rate limiting for SSH (built into UFW)
ufw limit ssh comment 'SSH rate limit'

# Enable UFW
ufw --force enable

echo "UFW rules applied successfully."
echo "Check status with: ufw status verbose"