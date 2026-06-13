#!/bin/bash
# TCPSER Firewall Rules (iptables)
# Run with: sudo bash iptables-rules.sh

set -e

# Configuration
TCPSER_PORT=6400
WEB_PORT=8080
SSH_PORT=22

echo "Setting up iptables rules for TCPSER..."

# Flush existing rules
iptables -F
iptables -X
iptables -t nat -F
iptables -t nat -X

# Default policies
iptables -P INPUT DROP
iptables -P FORWARD DROP
iptables -P OUTPUT ACCEPT

# Allow loopback
iptables -A INPUT -i lo -j ACCEPT

# Allow established connections
iptables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT

# Allow SSH (change port if needed)
iptables -A INPUT -p tcp --dport $SSH_PORT -m state --state NEW -j ACCEPT

# Rate limit SSH to prevent brute force
iptables -A INPUT -p tcp --dport $SSH_PORT -m recent --name ssh --update --seconds 60 --hitcount 4 -j DROP
iptables -A INPUT -p tcp --dport $SSH_PORT -m recent --name ssh --set -j ACCEPT

# Allow TCPSER modem port
iptables -A INPUT -p tcp --dport $TCPSER_PORT -m state --state NEW -j ACCEPT

# Rate limit TCPSER connections (10 new connections per minute per IP)
iptables -A INPUT -p tcp --dport $TCPSER_PORT -m recent --name tcpser --update --seconds 60 --hitcount 10 -j DROP
iptables -A INPUT -p tcp --dport $TCPSER_PORT -m recent --name tcpser --set -j ACCEPT

# Allow web dashboard only from localhost (modify if needed)
iptables -A INPUT -p tcp -s 127.0.0.1 --dport $WEB_PORT -j ACCEPT

# Allow HTTP/HTTPS if using nginx proxy
# iptables -A INPUT -p tcp --dport 80 -m state --state NEW -j ACCEPT
# iptables -A INPUT -p tcp --dport 443 -m state --state NEW -j ACCEPT

# Allow ping
iptables -A INPUT -p icmp --icmp-type echo-request -j ACCEPT

# Log dropped packets (optional)
iptables -A INPUT -m limit --limit 5/min -j LOG --log-prefix "iptables denied: " --log-level 7

# Drop everything else
iptables -A INPUT -j DROP

echo "Firewall rules applied successfully."
echo "To make persistent, save with: iptables-save > /etc/iptables/rules.v4"