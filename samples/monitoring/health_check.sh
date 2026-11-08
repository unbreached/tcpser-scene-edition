#!/bin/bash
# TCPSER Health Check Script
# Use with cron, monitoring systems, or manually

set -u   # not -e: a failed check must set a flag, not abort the script

# Configuration
TCPSER_PORT=6400
WEB_PORT=8080
TIMEOUT=5

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Exit codes
EXIT_OK=0
EXIT_WARNING=1
EXIT_CRITICAL=2

# Flags
CRITICAL=0
WARNING=0

echo "TCPSER Health Check $(date)"
echo "================================"

# Check if TCPSER process is running
if pgrep -x "tcpser" > /dev/null; then
    echo -e "${GREEN}✓${NC} TCPSER process is running"
    PID=$(pgrep -x "tcpser" | head -n 1)     # several instances: report the first
    echo "  PID(s): $(pgrep -x tcpser | tr '\n' ' ')"

    # Check memory usage
    MEM_USAGE=$(ps -o pid,vsz,rss,pmem -p "$PID" | tail -n 1)
    echo "  Memory: $MEM_USAGE"
else
    echo -e "${RED}✗${NC} TCPSER process is NOT running"
    CRITICAL=1
fi

# Check if ports are listening
echo
echo "Port Status:"
# listening ports: ss (Linux), netstat, or lsof (macOS) - whichever exists
listening() {
    if command -v ss >/dev/null; then ss -tln 2>/dev/null | grep -q ":$1 "
    elif command -v netstat >/dev/null; then netstat -tln 2>/dev/null | grep -q "[.:]$1 "
    else lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; fi
}
if listening "$TCPSER_PORT"; then
    echo -e "${GREEN}✓${NC} Port $TCPSER_PORT (modem) is listening"
else
    echo -e "${RED}✗${NC} Port $TCPSER_PORT (modem) is NOT listening"
    CRITICAL=1
fi

if listening "$WEB_PORT"; then
    echo -e "${GREEN}✓${NC} Port $WEB_PORT (web) is listening"
else
    echo -e "${YELLOW}⚠${NC} Port $WEB_PORT (web) is NOT listening"
    WARNING=1
fi

# Test TCP connection
echo
echo "Connection Tests:"
if timeout "$TIMEOUT" nc -z localhost "$TCPSER_PORT" 2>/dev/null; then
    echo -e "${GREEN}✓${NC} TCP connection to port $TCPSER_PORT successful"
else
    echo -e "${RED}✗${NC} TCP connection to port $TCPSER_PORT failed"
    CRITICAL=1
fi

# Test web dashboard
if timeout "$TIMEOUT" curl -s -o /dev/null "http://localhost:$WEB_PORT/" 2>/dev/null; then
    echo -e "${GREEN}✓${NC} Web dashboard responding"
else
    echo -e "${YELLOW}⚠${NC} Web dashboard not responding"
    WARNING=1
fi

# Check serial device (if specified)
if [[ -n "$1" ]]; then
    SERIAL_DEV="$1"
    echo
    echo "Serial Device Check:"
    if [[ -e "$SERIAL_DEV" ]]; then
        if [[ -r "$SERIAL_DEV" && -w "$SERIAL_DEV" ]]; then
            echo -e "${GREEN}✓${NC} Serial device $SERIAL_DEV is accessible"
        else
            echo -e "${YELLOW}⚠${NC} Serial device $SERIAL_DEV exists but may not have proper permissions"
            WARNING=1
        fi
    else
        echo -e "${RED}✗${NC} Serial device $SERIAL_DEV does not exist"
        CRITICAL=1
    fi
fi

# Check log files
echo
echo "Log Status:"
LOG_FILES=("/var/log/tcpser/connections.csv" "/var/log/tcpser/gate_success.log" "/var/log/tcpser/gate_failures.log")
for LOG_FILE in "${LOG_FILES[@]}"; do
    if [[ -f "$LOG_FILE" ]]; then
        SIZE=$(du -h "$LOG_FILE" 2>/dev/null | cut -f1)
        LINES=$(wc -l < "$LOG_FILE" 2>/dev/null || echo "0")
        echo -e "${GREEN}✓${NC} $LOG_FILE ($SIZE, $LINES lines)"
    else
        echo -e "${YELLOW}⚠${NC} $LOG_FILE does not exist"
    fi
done

# Summary
echo
echo "Summary:"
if [[ $CRITICAL -eq 1 ]]; then
    echo -e "${RED}CRITICAL${NC} - TCPSER has critical issues"
    exit $EXIT_CRITICAL
elif [[ $WARNING -eq 1 ]]; then
    echo -e "${YELLOW}WARNING${NC} - TCPSER has minor issues"
    exit $EXIT_WARNING
else
    echo -e "${GREEN}OK${NC} - TCPSER is healthy"
    exit $EXIT_OK
fi