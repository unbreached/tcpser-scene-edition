#!/bin/bash
#
# TCPSER Secure Script Example: Connection Notification
#
# This script is executed when a user successfully passes the BBS gate.
# Environment variables provided:
#   TCPSER_PEER_IP - IP address of the connecting client
#   TCPSER_SCRIPT_DIR - Directory containing this script
#
# Security: This script runs with restricted environment and 30-second timeout.

# Get client IP (validated by tcpser)
CLIENT_IP="${TCPSER_PEER_IP:-unknown}"

# Log the connection
/usr/bin/logger "TCPSER: Successful BBS connection from ${CLIENT_IP}"

# Write to connection log file (create if needed)
LOG_FILE="/var/log/tcpser-connections.log"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# Create log entry
echo "${TIMESTAMP} - Connection from ${CLIENT_IP}" >> "${LOG_FILE}" 2>/dev/null

# Optional: Send notification to monitoring system
# Uncomment and configure as needed:

# Send to syslog
#logger -t tcpser "BBS connection established from ${CLIENT_IP}"

# Send to ntfy.sh notification service
#curl -X POST "https://ntfy.sh/your-bbs-topic" \
#     -H "Title: BBS Connection" \
#     -d "New connection from ${CLIENT_IP}" \
#     --max-time 10 2>/dev/null &

# Send email notification
#echo "BBS connection from ${CLIENT_IP} at ${TIMESTAMP}" | \
#  /usr/bin/mail -s "BBS Connection Alert" admin@example.com 2>/dev/null &

# Exit successfully
exit 0