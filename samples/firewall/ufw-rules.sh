#!/bin/bash
# TCPSER Firewall Rules (UFW - Ubuntu)
# Run with: sudo bash ufw-rules.sh

set -e

# Configuration
TCPSER_PORT=6400
WEB_PORT=8080
SSH_PORT=22

echo "Setting up UFW rules for TCPSER..."

# Reset UFW
ufw --force reset

# Default policies
ufw default deny incoming
ufw default allow outgoing

# Allow SSH
ufw allow $SSH_PORT/tcp comment 'SSH'

# Allow TCPSER modem port
ufw allow $TCPSER_PORT/tcp comment 'TCPSER Modem'

# Allow web dashboard only from localhost (modify if needed)
ufw allow from 127.0.0.1 to any port $WEB_PORT comment 'TCPSER Dashboard'

# Allow HTTP/HTTPS if using nginx proxy (uncomment if needed)
# ufw allow 80/tcp comment 'HTTP'
# ufw allow 443/tcp comment 'HTTPS'

# Rate limiting for SSH (built into UFW)
ufw limit ssh comment 'SSH rate limit'

# Enable UFW
ufw --force enable

echo "UFW rules applied successfully."
echo "Check status with: ufw status verbose"