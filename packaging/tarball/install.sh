#!/bin/sh
set -e

PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
DATADIR="$PREFIX/share"

echo "Installing BareStats to $PREFIX..."

mkdir -p "$BINDIR"
mkdir -p "$DATADIR/applications"
mkdir -p "$DATADIR/icons/hicolor/scalable/apps"
mkdir -p "$DATADIR/icons/hicolor/256x256/apps"
mkdir -p "$DATADIR/pixmaps"
mkdir -p "$DATADIR/metainfo"
mkdir -p "$DATADIR/doc/barestats"

install -m 755 barestats "$BINDIR/barestats"
install -m 644 barestats.desktop "$DATADIR/applications/barestats.desktop"
install -m 644 barestats.svg "$DATADIR/icons/hicolor/scalable/apps/barestats.svg"
install -m 644 barestats.png "$DATADIR/icons/hicolor/256x256/apps/barestats.png"
install -m 644 barestats.png "$DATADIR/pixmaps/barestats.png"
install -m 644 io.github.chakrank.barestats.metainfo.xml "$DATADIR/metainfo/io.github.chakrank.barestats.metainfo.xml"
install -m 644 config.example.ini "$DATADIR/doc/barestats/config.example.ini"
install -m 644 LICENSE "$DATADIR/doc/barestats/copyright"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "$DATADIR/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$DATADIR/icons/hicolor" || true
fi

echo "BareStats installed successfully!"
echo "Run 'barestats' from your terminal or launch from your Application Menu."
