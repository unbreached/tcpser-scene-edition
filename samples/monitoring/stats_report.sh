#!/bin/bash
# TCPSER Statistics Report Generator
# Generates daily/weekly reports from TCPSER logs

# Configuration
LOG_DIR="/var/log/tcpser"
CONNECTIONS_LOG="$LOG_DIR/connections.csv"
SUCCESS_LOG="$LOG_DIR/gate_success.log"
FAILURES_LOG="$LOG_DIR/gate_failures.log"
WEB_PORT=8080

# Parse command line arguments
DAYS=1
REPORT_TYPE="daily"

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--days)
            DAYS="$2"
            shift 2
            ;;
        -w|--weekly)
            DAYS=7
            REPORT_TYPE="weekly"
            shift
            ;;
        -m|--monthly)
            DAYS=30
            REPORT_TYPE="monthly"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [-d days] [-w] [-m]"
            echo "  -d days    Report for last N days (default: 1)"
            echo "  -w         Weekly report (last 7 days)"
            echo "  -m         Monthly report (last 30 days)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "TCPSER $REPORT_TYPE Report (Last $DAYS days)"
echo "Generated: $(date)"
echo "========================================"

# Calculate date range
if command -v gdate >/dev/null 2>&1; then
    # macOS with GNU coreutils
    START_DATE=$(gdate -d "$DAYS days ago" '+%Y-%m-%d')
else
    # Linux
    START_DATE=$(date -d "$DAYS days ago" '+%Y-%m-%d')
fi

echo "Period: $START_DATE to $(date '+%Y-%m-%d')"
echo

# Connection Statistics
if [[ -f "$CONNECTIONS_LOG" ]]; then
    echo "Connection Statistics:"
    echo "----------------------"

    # Total connections in period
    TOTAL_CONNECTIONS=$(awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start { count++ }
        END { print count+0 }
    ' "$CONNECTIONS_LOG" 2>/dev/null)

    # Successful connections
    SUCCESSFUL=$(awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start && $5 == "CONNECTED" { count++ }
        END { print count+0 }
    ' "$CONNECTIONS_LOG" 2>/dev/null)

    # Blocked connections
    BLOCKED=$(awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start && $5 == "BLOCKED" { count++ }
        END { print count+0 }
    ' "$CONNECTIONS_LOG" 2>/dev/null)

    # Gate failures
    GATE_FAIL=$(awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start && ($5 == "GATE-FAIL" || $5 == "GATE-PASS") { count++ }
        END { print count+0 }
    ' "$CONNECTIONS_LOG" 2>/dev/null)

    echo "  Total Connections: $TOTAL_CONNECTIONS"
    echo "  Successful:        $SUCCESSFUL"
    echo "  Blocked:          $BLOCKED"
    echo "  Gate Issues:      $GATE_FAIL"

    if [[ $TOTAL_CONNECTIONS -gt 0 ]]; then
        SUCCESS_RATE=$(( (SUCCESSFUL * 100) / TOTAL_CONNECTIONS ))
        echo "  Success Rate:     ${SUCCESS_RATE}%"
    fi

    echo
    echo "Top Client IPs:"
    awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start { count[$2]++ }
        END { for (ip in count) print count[ip], ip }
    ' "$CONNECTIONS_LOG" 2>/dev/null | sort -nr | head -10 | while read count ip; do
        echo "  $ip: $count connections"
    done

    echo
    echo "Hourly Distribution:"
    awk -F, -v start="$START_DATE" '
        NR>1 && $1 >= start {
            split($3, time, ":")
            hour = time[1]
            count[hour]++
        }
        END {
            for (h=0; h<24; h++) {
                printf "  %02d:00-%02d:59: %d\n", h, h, count[sprintf("%02d", h)]+0
            }
        }
    ' "$CONNECTIONS_LOG" 2>/dev/null

else
    echo "No connection log found at $CONNECTIONS_LOG"
fi

echo

# System Health
echo "System Health:"
echo "--------------"

# Check if TCPSER is running
if pgrep -x "tcpser" > /dev/null; then
    PID=$(pgrep -x "tcpser")
    echo "  Status: Running (PID: $PID)"

    # Uptime
    if command -v ps >/dev/null 2>&1; then
        UPTIME=$(ps -o etime= -p "$PID" | tr -d ' ')
        echo "  Uptime: $UPTIME"
    fi

    # Memory usage
    if command -v ps >/dev/null 2>&1; then
        MEM_MB=$(ps -o rss= -p "$PID" | awk '{print int($1/1024)}')
        echo "  Memory: ${MEM_MB}MB"
    fi
else
    echo "  Status: Not running"
fi

# Port status
if netstat -tlnp 2>/dev/null | grep -q ":6400 "; then
    echo "  Modem Port: Listening"
else
    echo "  Modem Port: Not listening"
fi

if netstat -tlnp 2>/dev/null | grep -q ":$WEB_PORT "; then
    echo "  Web Port: Listening"
else
    echo "  Web Port: Not listening"
fi

echo

# Recent Errors
echo "Recent Issues:"
echo "--------------"

# Check for recent failures
if [[ -f "$FAILURES_LOG" ]]; then
    RECENT_FAILURES=$(grep "$(date '+%Y-%m-%d')" "$FAILURES_LOG" 2>/dev/null | wc -l)
    echo "  Gate failures today: $RECENT_FAILURES"

    if [[ $RECENT_FAILURES -gt 0 ]]; then
        echo "  Recent failure IPs:"
        grep "$(date '+%Y-%m-%d')" "$FAILURES_LOG" 2>/dev/null | \
        awk '{print $4}' | sort | uniq -c | sort -nr | head -5 | \
        while read count ip; do
            echo "    $ip: $count failures"
        done
    fi
else
    echo "  No failure log found"
fi

# Check system log for TCPSER errors
if command -v journalctl >/dev/null 2>&1; then
    ERROR_COUNT=$(journalctl -u tcpser --since="$START_DATE" --no-pager -q 2>/dev/null | grep -i error | wc -l)
    echo "  System log errors: $ERROR_COUNT"
fi

echo
echo "Report generated at $(date)"