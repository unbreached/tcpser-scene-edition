#!/bin/bash
# Secure TCPSER Installation Script
# Automates installation with security hardening

set -e

# Configuration
TCPSER_USER="tcpser"
TCPSER_GROUP="tcpser"
INSTALL_PREFIX="/usr/local"
DATA_DIR="/usr/local/share/tcpser"
SCRIPT_DIR="/usr/local/tcpser/scripts"
LOG_DIR="/var/log/tcpser"
CONFIG_DIR="/etc/tcpser"
SERVICE_NAME="tcpser"

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
    useradd --system --gid "$TCPSER_GROUP" --shell /bin/false \
            --home-dir /nonexistent --create-home --no-user-group \
            "$TCPSER_USER"
fi

# Create directories
log "Creating directories..."
mkdir -p "$INSTALL_PREFIX/bin"
mkdir -p "$INSTALL_PREFIX/share/man/man1"
mkdir -p "$DATA_DIR"
mkdir -p "$SCRIPT_DIR"
mkdir -p "$LOG_DIR"
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
chown -R root:"$TCPSER_GROUP" "$DATA_DIR"
chown -R "$TCPSER_USER":"$TCPSER_GROUP" "$LOG_DIR"
chown -R "$TCPSER_USER":"$TCPSER_GROUP" "$SCRIPT_DIR"
chown -R root:"$TCPSER_GROUP" "$CONFIG_DIR"

chmod 755 "$DATA_DIR"
chmod 755 "$LOG_DIR"
chmod 755 "$SCRIPT_DIR"
chmod 755 "$CONFIG_DIR"

# Install sample scripts
log "Installing sample gate scripts..."
if [[ -d "samples/scripts" ]]; then
    cp samples/scripts/* "$SCRIPT_DIR/" 2>/dev/null || true
    chown "$TCPSER_USER":"$TCPSER_GROUP" "$SCRIPT_DIR"/*
    chmod 755 "$SCRIPT_DIR"/*
fi

# Create default configuration
log "Creating default configuration..."
cat > "$CONFIG_DIR/tcpser.conf" << 'EOF'
# TCPSER Configuration File
# Command line options can be specified here

# Serial device
device=/dev/ttyUSB0

# Serial speed
speed=2400

# TCP port
port=6400

# Enable extensions
extensions=yes

# Web dashboard port
web_port=8080

# Script directory
script_dir=/usr/local/tcpser/scripts

# Connection log
connection_log=/var/log/tcpser/connections.csv

# Blocklist file
blocklist=/usr/local/share/tcpser/tcpser.blocklist
EOF

chown root:"$TCPSER_GROUP" "$CONFIG_DIR/tcpser.conf"
chmod 640 "$CONFIG_DIR/tcpser.conf"

# Install systemd service
if command -v systemctl >/dev/null; then
    log "Installing systemd service..."

    if [[ -f "samples/systemd/tcpser.service" ]]; then
        cp samples/systemd/tcpser.service /etc/systemd/system/
    else
        # Create basic service file
        cat > /etc/systemd/system/tcpser.service << 'EOF'
[Unit]
Description=TCPSER TCP-to-Serial Bridge
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x -w 8080
User=tcpser
Group=tcpser
Restart=always
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes

[Install]
WantedBy=multi-user.target
EOF
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
echo "1. Configure your serial device in $CONFIG_DIR/tcpser.conf"
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
echo "  http://localhost:8080"
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