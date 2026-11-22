#!/bin/sh
set -e

PREFIX="${PREFIX:-/usr/local}"
APP_DIR="${APP_DIR:-/Applications}"

echo "Uninstalling BareStats from macOS..."

# Check if BareStats process is running
if pgrep -x BareStats >/dev/null 2>&1 || pgrep -x barestats >/dev/null 2>&1; then
    if [ -t 0 ]; then
        printf "BareStats is currently running. Terminate running BareStats process before uninstalling? [Y/n]: "
        read -r answer
        case "$answer" in
            [nN][oO]|[nN])
                echo "Warning: Continuing uninstall without terminating BareStats."
                ;;
            *)
                echo "Terminating running BareStats process..."
                pkill -x BareStats 2>/dev/null || pkill -x barestats 2>/dev/null || killall BareStats 2>/dev/null || true
                ;;
        esac
    else
        pkill -x BareStats 2>/dev/null || pkill -x barestats 2>/dev/null || killall BareStats 2>/dev/null || true
    fi
fi

# Remove LaunchAgent if registered
if [ -n "$HOME" ] && [ -f "$HOME/Library/LaunchAgents/com.barestats.satellite.plist" ]; then
    launchctl bootout "gui/$(id -u)" "$HOME/Library/LaunchAgents/com.barestats.satellite.plist" 2>/dev/null || true
    rm -f "$HOME/Library/LaunchAgents/com.barestats.satellite.plist"
fi

# Remove binary and app bundle
rm -f "$PREFIX/bin/barestats"
rm -rf "$APP_DIR/BareStats.app"

echo "BareStats uninstalled successfully."
