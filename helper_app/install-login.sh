#!/bin/bash
# Install Browservice Helper as a Login Item on OS X Leopard
# Run this on the iMac G4 after copying the .app bundle

APP_NAME="BrowserviceHelper"
APP_PATH="/Applications/${APP_NAME}.app"

if [ ! -d "$APP_PATH" ]; then
    echo "Error: $APP_PATH not found"
    echo "Copy BrowserviceHelper.app to /Applications/ first"
    exit 1
fi

# Add to Login Items using osascript (Leopard compatible)
osascript <<EOF
tell application "System Events"
    make login item at end with properties {path:"$APP_PATH", hidden:true}
end tell
EOF

echo "Added $APP_NAME to Login Items (will auto-launch at login)"
echo "To remove: System Preferences → Accounts → Login Items"
