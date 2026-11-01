#!/bin/bash
# Secure TCPSER Installation Script
# Automates installation with security hardening

set -euo pipefail

# Configuration
TCPSER_USER="tcpser"
TCPSER_GROUP="tcpser"
INSTALL_PREFIX="/usr/local"
DATA_DIR="/usr/local/share/tcpser"
SCRIPT_DIR="/usr/local/tcpser/scripts"
LOG_DIR="/var/log/tcpser"
STATE_DIR="/var/lib/tcpser"
CONFIG_DIR="/etc/tcpser"
SERVICE_NAME="tcpser"
# Safe to re-run: existing /etc/tcpser/tcpser.env and an existing systemd unit
# are left alone (set FORCE=1 to overwrite them).
FORCE="${FORCE:-0}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Logging
log() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    error "This script must be run as root (use sudo)"
fi

echo -e "${BLUE}TCPSER Secure Installation${NC}"
echo "=========================="
echo

# Detect OS
if [[ -f /etc/os-release ]]; then
    source /etc/os-release
    OS=$ID
    VERSION=$VERSION_ID
else
    error "Cannot detect operating system"
fi

log "Detected OS: $OS $VERSION"

# Install dependencies
log "Installing dependencies..."
case $OS in
    ubuntu|debian)
        apt-get update
        apt-get install -y build-essential gcc make libc6-dev
        if command -v systemctl >/dev/null; then
            apt-get install -y systemd
        fi
        ;;
    centos|rhel|fedora)
        if command -v dnf >/dev/null; then
            dnf install -y gcc make glibc-devel
        else
            yum install -y gcc make glibc-devel
        fi
        ;;
    *)
        warn "Unknown OS. Please ensure gcc, make, and development tools are installed."
        ;;
esac

# Check if TCPSER source is available
if [[ ! -f "tcpser" && ! -f "src/tcpser.c" ]]; then
    error "TCPSER source not found. Run this script from the TCPSER source directory."
fi

# Build TCPSER if not already built
if [[ ! -f "tcpser" ]]; then
    log "Building TCPSER..."
    make clean || true
    make
fi

# Create user and group
log "Creating TCPSER user and group..."
if ! getent group "$TCPSER_GROUP" >/dev/null; then
    groupadd --system "$TCPSER_GROUP"
fi

if ! getent passwd "$TCPSER_USER" >/dev/null; then
    useradd --system --gid "$TCPSER_GROUP" --shell /usr/sbin/nologin \
            --home-dir /nonexistent --no-create-home --no-user-group \
            "$TCPSER_USER"
fi
# Serial adapters are group dialout (Debian/Ubuntu/Raspberry Pi OS) or uucp.
for g in dialout uucp; do
    if getent group "$g" >/dev/null; then usermod -a -G "$g" "$TCPSER_USER"; fi
done

# Create directories
log "Creating directories..."
mkdir -p "$INSTALL_PREFIX/bin"
mkdir -p "$INSTALL_PREFIX/share/man/man1"
mkdir -p "$DATA_DIR"
mkdir -p "$SCRIPT_DIR"
mkdir -p "$LOG_DIR"
mkdir -p "$STATE_DIR"
mkdir -p "$CONFIG_DIR"

# Install TCPSER binary
log "Installing TCPSER binary..."
cp tcpser "$INSTALL_PREFIX/bin/"
chmod 755 "$INSTALL_PREFIX/bin/tcpser"

# Install man page
if [[ -f "man/tcpser.1" ]]; then
    log "Installing man page..."
    cp man/tcpser.1 "$INSTALL_PREFIX/share/man/man1/"
    chmod 644 "$INSTALL_PREFIX/share/man/man1/tcpser.1"
fi

# Install data files
log "Installing data files..."
if [[ -d "data" ]]; then
    cp data/*.seq "$DATA_DIR/" 2>/dev/null || true
    cp data/tcpser.blocklist "$DATA_DIR/" 2>/dev/null || true
    chmod 644 "$DATA_DIR"/* 2>/dev/null || true
fi

# Set up permissions
log "Setting up permissions..."
chown root:root "$INSTALL_PREFIX/bin/tcpser"
chown -R root:"$TCPSER_GROUP" "$DATA_DIR"          # read-only data (banners, blocklist template)
chown -R "$TCPSER_USER":"$TCPSER_GROUP" "$LOG_DIR"  # logs: caller IPs = PII, keep private
chown -R "$TCPSER_USER":"$TCPSER_GROUP" "$STATE_DIR" # persistent blocklist + stats (writable)
chown -R root:"$TCPSER_GROUP" "$SCRIPT_DIR"        # hooks: NOT writable by the service user
chown -R root:"$TCPSER_GROUP" "$CONFIG_DIR"

chmod 755 "$DATA_DIR"
chmod 750 "$LOG_DIR"
chmod 750 "$STATE_DIR"
chmod 750 "$SCRIPT_DIR"
chmod 750 "$CONFIG_DIR"

# Seed the writable blocklist from the shipped template (never overwrite)
if [[ ! -f "$STATE_DIR/tcpser.blocklist" ]]; then
    if [[ -f "$DATA_DIR/tcpser.blocklist" ]]; then
        cp "$DATA_DIR/tcpser.blocklist" "$STATE_DIR/tcpser.blocklist"
    else
        : > "$STATE_DIR/tcpser.blocklist"
    fi
    chown "$TCPSER_USER":"$TCPSER_GROUP" "$STATE_DIR/tcpser.blocklist"
    chmod 640 "$STATE_DIR/tcpser.blocklist"
fi

# Install sample gate hooks (root-owned, group-executable; tcpser refuses
# world-writable hooks and the unit mounts this directory read-only)
log "Installing sample gate scripts..."
if [[ -d "samples/scripts" ]]; then
    for f in samples/scripts/*; do
        [[ -f "$f" && -x "$f" ]] || continue
        install -o root -g "$TCPSER_GROUP" -m 750 "$f" "$SCRIPT_DIR/"
    done
fi

# Environment file for secrets (dashboard token, gate password). tcpser has
# no config file: everything else is set on the ExecStart line of the unit.
if [[ ! -f "$CONFIG_DIR/tcpser.env" || "$FORCE" == "1" ]]; then
    log "Writing $CONFIG_DIR/tcpser.env (secrets; edit it, then: systemctl restart tcpser)"
    cat > "$CONFIG_DIR/tcpser.env" << 'ENVEOF'
# Environment for the tcpser systemd service (EnvironmentFile=).
# Uncomment and set what you need. Mode 0640 root:tcpser.

# Protect the web dashboard (http://127.0.0.1:8080/?token=...):
#TCPSER_DASH_TOKEN=change-me

# Expose the dashboard beyond localhost ONLY behind nginx+TLS (see samples/nginx):
#TCPSER_DASH_BIND=127.0.0.1

# Password gate: use `-k env:TCPSER_GATE_PASSWORD` on the ExecStart line.
#TCPSER_GATE_PASSWORD=change-me

# Persistent blocklist (already set in the unit; override here if needed)
#TCPSER_BLOCKLIST=/var/lib/tcpser/tcpser.blocklist
ENVEOF
else
    log "Keeping existing $CONFIG_DIR/tcpser.env"
fi
chown root:"$TCPSER_GROUP" "$CONFIG_DIR/tcpser.env"
chmod 640 "$CONFIG_DIR/tcpser.env"

# Install systemd service (never clobber a customised unit unless FORCE=1)
if command -v systemctl >/dev/null; then
    UNIT=/etc/systemd/system/$SERVICE_NAME.service
    if [[ -f "$UNIT" && "$FORCE" != "1" ]]; then
        log "Keeping existing $UNIT (edit with: systemctl edit --full $SERVICE_NAME)"
    elif [[ -f "samples/systemd/tcpser.service" ]]; then
        log "Installing systemd service..."
        install -m 644 samples/systemd/tcpser.service "$UNIT"
    else
        error "samples/systemd/tcpser.service not found; run from the source tree"
    fi
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"
    log "Service installed and enabled"
else
    warn "systemd not available. Manual service setup required."
fi

# Set up log rotation
log "Setting up log rotation..."
cat > /etc/logrotate.d/tcpser << 'EOF'
/var/log/tcpser/*.log /var/log/tcpser/*.csv {
    daily
    missingok
    rotate 30
    compress
    delaycompress
    notifempty
    copytruncate
    su tcpser tcpser
}
EOF

# Security recommendations
echo
echo -e "${BLUE}Installation Complete!${NC}"
echo "======================"
echo
log "TCPSER has been installed with security hardening"
echo
echo -e "${YELLOW}Next Steps:${NC}"
echo "1. Set your serial device/speed/port on the ExecStart line:"
echo "     sudo systemctl edit --full $SERVICE_NAME"
echo "   Secrets (dashboard token, gate password) go in $CONFIG_DIR/tcpser.env"
echo "2. Review and customize gate scripts in $SCRIPT_DIR/"
echo "3. Set up firewall rules (see samples/firewall/)"
echo "4. Configure nginx proxy for web dashboard (see samples/nginx/)"
echo "5. Set up fail2ban protection (see samples/fail2ban/)"
echo
echo -e "${YELLOW}Start TCPSER:${NC}"
echo "  sudo systemctl start $SERVICE_NAME"
echo
echo -e "${YELLOW}Check status:${NC}"
echo "  sudo systemctl status $SERVICE_NAME"
echo "  sudo journalctl -u $SERVICE_NAME -f"
echo
echo -e "${YELLOW}Web Dashboard:${NC}"
echo "  http://127.0.0.1:8080/   (add ?token=... if TCPSER_DASH_TOKEN is set)"
echo
echo -e "${YELLOW}Logs:${NC}"
echo "  $LOG_DIR/"
echo

# Final security check
warn "Security Reminders:"
echo "- Change default ports if exposed to internet"
echo "- Set up TLS for web dashboard in production"
echo "- Review and customize blocklist file"
echo "- Monitor logs regularly"
echo "- Keep system and dependencies updated"