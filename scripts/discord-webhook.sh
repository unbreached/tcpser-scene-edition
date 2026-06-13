#!/bin/bash
#
# TCPSER Secure Script Example: Discord Webhook Notification
#
# Sends a Discord notification when someone connects to the BBS.
# Configure your Discord webhook URL below.

# Configuration
DISCORD_WEBHOOK_URL=""  # Set your Discord webhook URL here

# Get client information
CLIENT_IP="${TCPSER_PEER_IP:-unknown}"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# Only proceed if webhook URL is configured
if [[ -z "${DISCORD_WEBHOOK_URL}" ]]; then
    echo "Discord webhook URL not configured in script"
    exit 1
fi

# Validate IP format (basic check)
if [[ ! "${CLIENT_IP}" =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]] && [[ "${CLIENT_IP}" != "unknown" ]]; then
    CLIENT_IP="invalid"
fi

# Create Discord message payload
JSON_PAYLOAD=$(cat <<EOF
{
    "embeds": [
        {
            "title": "🖥️ BBS Connection",
            "description": "New connection to your retro BBS!",
            "color": 65280,
            "fields": [
                {
                    "name": "IP Address",
                    "value": "${CLIENT_IP}",
                    "inline": true
                },
                {
                    "name": "Timestamp",
                    "value": "${TIMESTAMP}",
                    "inline": true
                }
            ],
            "footer": {
                "text": "TCPSER BBS Gateway"
            }
        }
    ]
}
EOF
)

# Send notification to Discord (background, with timeout)
curl -H "Content-Type: application/json" \
     -X POST \
     -d "${JSON_PAYLOAD}" \
     --max-time 15 \
     --silent \
     "${DISCORD_WEBHOOK_URL}" &

exit 0