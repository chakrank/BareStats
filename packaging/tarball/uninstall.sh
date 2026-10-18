#!/bin/sh
set -e

PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
DATADIR="$PREFIX/share"

echo "Uninstalling BareStats from $PREFIX..."

rm -f "$BINDIR/barestats"
rm -f "$DATADIR/applications/barestats.desktop"
rm -f "$DATADIR/icons/hicolor/scalable/apps/barestats.svg"
rm -f "$DATADIR/icons/hicolor/256x256/apps/barestats.png"
rm -f "$DATADIR/pixmaps/barestats.png"
rm -f "$DATADIR/metainfo/io.github.chakrank.barestats.metainfo.xml"
rm -rf "$DATADIR/doc/barestats"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "$DATADIR/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$DATADIR/icons/hicolor" || true
fi

echo "BareStats uninstalled successfully."
