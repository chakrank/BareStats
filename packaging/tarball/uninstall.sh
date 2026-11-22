#!/bin/sh
set -e

PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
DATADIR="$PREFIX/share"

echo "Uninstalling BareStats from $PREFIX..."

# Terminate running BareStats process if active
if pgrep -x barestats >/dev/null 2>&1 || pidof barestats >/dev/null 2>&1; then
    if [ -t 0 ]; then
        printf "BareStats is currently running. Terminate running BareStats process before uninstalling? [Y/n]: "
        read -r answer
        case "$answer" in
            [nN][oO]|[nN])
                echo "Warning: Continuing uninstall without terminating BareStats."
                ;;
            *)
                echo "Terminating running BareStats process..."
                killall -q barestats 2>/dev/null || pkill -x barestats 2>/dev/null || true
                ;;
        esac
    else
        killall -q barestats 2>/dev/null || pkill -x barestats 2>/dev/null || true
    fi
fi

rm -f "$BINDIR/barestats"
rm -f "$DATADIR/applications/barestats.desktop"
rm -f "$DATADIR/applications/io.github.chakrank.barestats.desktop"
rm -f "$DATADIR/icons/hicolor/scalable/apps/barestats.svg"
rm -f "$DATADIR/icons/hicolor/scalable/apps/io.github.chakrank.barestats.svg"
rm -f "$DATADIR/icons/hicolor/256x256/apps/barestats.png"
rm -f "$DATADIR/icons/hicolor/256x256/apps/io.github.chakrank.barestats.png"
rm -f "$DATADIR/pixmaps/barestats.png"
rm -f "$DATADIR/pixmaps/io.github.chakrank.barestats.png"
rm -f "$DATADIR/metainfo/io.github.chakrank.barestats.metainfo.xml"
rm -rf "$DATADIR/doc/barestats"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "$DATADIR/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$DATADIR/icons/hicolor" || true
fi

echo "BareStats uninstalled successfully."
