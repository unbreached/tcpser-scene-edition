#!/bin/bash
#
# TCPSER Secure Script Example: Simple Connection Logger
#
# Minimal script that just logs connections to a file.
# Good for basic monitoring without external dependencies.

# Configuration
LOG_FILE="/tmp/tcpser-connections.log"

# Get connection info
CLIENT_IP="${TCPSER_PEER_IP:-unknown}"
DATE_TIME=$(date '+%Y-%m-%d %H:%M:%S')

# Create simple log entry
echo "${DATE_TIME} - Connection from ${CLIENT_IP}" >> "${LOG_FILE}"

# Optional: limit log file size (keep last 1000 lines)
tail -n 1000 "${LOG_FILE}" > "${LOG_FILE}.tmp" 2>/dev/null && mv "${LOG_FILE}.tmp" "${LOG_FILE}"

exit 0