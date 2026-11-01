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

# SSH: rate-limited (4 new connections / 60 s per source), then accept.
# NOTE: the DROP rule must come BEFORE the accepting --set rule; an earlier
# version had an unconditional ACCEPT first, which made the limits dead code.
iptables -A INPUT -p tcp --dport $SSH_PORT -m state --state NEW -m recent --name ssh --update --seconds 60 --hitcount 4 -j DROP
iptables -A INPUT -p tcp --dport $SSH_PORT -m state --state NEW -m recent --name ssh --set -j ACCEPT

# TCPSER modem port: rate-limited (10 new connections / 60 s per source), then accept.
iptables -A INPUT -p tcp --dport $TCPSER_PORT -m state --state NEW -m recent --name tcpser --update --seconds 60 --hitcount 10 -j DROP
iptables -A INPUT -p tcp --dport $TCPSER_PORT -m state --state NEW -m recent --name tcpser --set -j ACCEPT

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

# IPv6: without this the v6 INPUT policy stays ACCEPT and every port is open
# over IPv6. Mirror the essentials (ip6tables may be absent on some systems).
if command -v ip6tables >/dev/null 2>&1; then
    ip6tables -F
    ip6tables -P INPUT DROP
    ip6tables -P FORWARD DROP
    ip6tables -P OUTPUT ACCEPT
    ip6tables -A INPUT -i lo -j ACCEPT
    ip6tables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT
    ip6tables -A INPUT -p ipv6-icmp -j ACCEPT
    ip6tables -A INPUT -p tcp --dport $SSH_PORT -m state --state NEW -m recent --name ssh6 --update --seconds 60 --hitcount 4 -j DROP
    ip6tables -A INPUT -p tcp --dport $SSH_PORT -m state --state NEW -m recent --name ssh6 --set -j ACCEPT
    ip6tables -A INPUT -p tcp --dport $TCPSER_PORT -m state --state NEW -m recent --name tcpser6 --update --seconds 60 --hitcount 10 -j DROP
    ip6tables -A INPUT -p tcp --dport $TCPSER_PORT -m state --state NEW -m recent --name tcpser6 --set -j ACCEPT
    ip6tables -A INPUT -j DROP
fi

echo "Firewall rules applied successfully."
echo "To make persistent, save with: iptables-save > /etc/iptables/rules.v4"
echo "                           and: ip6tables-save > /etc/iptables/rules.v6"