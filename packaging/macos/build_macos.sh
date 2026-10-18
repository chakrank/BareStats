#!/usr/bin/env bash
set -euo pipefail

# BareStats macOS Build and Packaging Script
# Generates Universal (Apple Silicon arm64 + Intel x86_64) BareStats.app and Drag-and-Drop .dmg

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-macos"
DIST_DIR="${ROOT_DIR}/packages/macos"

echo "============================================================"
echo "BareStats macOS Native Build Pipeline (Universal arm64/x86_64)"
echo "============================================================"

mkdir -p "${BUILD_DIR}"
mkdir -p "${DIST_DIR}"

echo "[1/5] Generating native Apple iconset (.icns)..."
if command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
    mkdir -p "${ROOT_DIR}/barestats.iconset"
    sips -z 16 16     "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_16x16.png" >/dev/null
    sips -z 32 32     "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_16x16@2x.png" >/dev/null
    sips -z 32 32     "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_32x32.png" >/dev/null
    sips -z 64 64     "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_32x32@2x.png" >/dev/null
    sips -z 128 128   "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_128x128.png" >/dev/null
    sips -z 256 256   "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_128x128@2x.png" >/dev/null
    sips -z 256 256   "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_256x256.png" >/dev/null
    sips -z 512 512   "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_256x256@2x.png" >/dev/null
    sips -z 512 512   "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_512x512.png" >/dev/null
    sips -z 1024 1024 "${ROOT_DIR}/resources/linux/barestats.png" --out "${ROOT_DIR}/barestats.iconset/icon_512x512@2x.png" >/dev/null
    mkdir -p "${ROOT_DIR}/resources/macos"
    iconutil -c icns "${ROOT_DIR}/barestats.iconset" -o "${ROOT_DIR}/resources/macos/barestats.icns"
    rm -rf "${ROOT_DIR}/barestats.iconset"
fi

echo "[2/5] Configuring CMake with Universal Binary support..."
cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DBARESTATS_BUILD_TESTS=ON

echo "[3/5] Building BareStats.app bundle and test harness..."
cmake --build "${BUILD_DIR}" --config Release -j"$(sysctl -n hw.ncpu || echo 4)"

if [ -d "${BUILD_DIR}/BareStats.app" ]; then
    chmod +x "${BUILD_DIR}/BareStats.app/Contents/MacOS/BareStats"
    codesign --force --deep --sign - "${BUILD_DIR}/BareStats.app" 2>/dev/null || true
fi

echo "[4/5] Running automated modular verification test suite..."
if [ -x "${BUILD_DIR}/BareStatsTests" ]; then
    "${BUILD_DIR}/BareStatsTests"
elif [ -x "${BUILD_DIR}/Release/BareStatsTests" ]; then
    "${BUILD_DIR}/Release/BareStatsTests"
fi

echo "[5/5] Generating Drag-and-Drop .dmg distribution package..."
cd "${BUILD_DIR}"
cpack -G DragNDrop

echo "Moving generated artifacts to ${DIST_DIR}..."
mv -f *.dmg "${DIST_DIR}/" || true

if ls "${DIST_DIR}"/*.dmg >/dev/null 2>&1; then
    echo "Generated DMG checksums:"
    shasum -a 256 "${DIST_DIR}"/*.dmg
fi

echo "============================================================"
echo "Build complete! Package generated in ${DIST_DIR}"
echo "============================================================"
