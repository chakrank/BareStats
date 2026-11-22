#!/usr/bin/env bash
set -euo pipefail

# BareStats macOS Build and Packaging Script
# Generates Universal (Apple Silicon arm64 + Intel x86_64) BareStats.app,
# barestats-<version>-macOS.dmg (for Homebrew Cask), and
# barestats-<version>-macOS.tar.gz (for Homebrew Formula).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-macos"
DIST_DIR="${ROOT_DIR}/packages/macos"

# Extract version from CMakeLists.txt or fallback to 1.4.0
VERSION="${VERSION:-$(grep -E 'project\(BareStats VERSION' "${ROOT_DIR}/CMakeLists.txt" | awk '{print $3}' || echo "1.4.0")}"
VERSION="${VERSION:-1.4.0}"

echo "============================================================"
echo "BareStats macOS Native Build Pipeline v${VERSION}"
echo "Universal Architecture (arm64 + x86_64)"
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
cmake --build "${BUILD_DIR}" --config Release -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [ -d "${BUILD_DIR}/BareStats.app" ]; then
    chmod +x "${BUILD_DIR}/BareStats.app/Contents/MacOS/BareStats"
    codesign --force --deep --sign - "${BUILD_DIR}/BareStats.app" 2>/dev/null || true
    # Create standalone barestats CLI binary alias alongside the bundle
    cp -f "${BUILD_DIR}/BareStats.app/Contents/MacOS/BareStats" "${BUILD_DIR}/barestats"
    chmod +x "${BUILD_DIR}/barestats"
fi

echo "[4/5] Running automated modular verification test suite..."
if [ -x "${BUILD_DIR}/BareStatsTests" ]; then
    "${BUILD_DIR}/BareStatsTests"
elif [ -x "${BUILD_DIR}/Release/BareStatsTests" ]; then
    "${BUILD_DIR}/Release/BareStatsTests"
fi

echo "[5/5] Generating Drag-and-Drop .dmg and Application Tarball..."
cd "${BUILD_DIR}"
cpack -G DragNDrop || true

# Locate and normalize generated .dmg artifact
DMG_DEST="${DIST_DIR}/barestats-${VERSION}-macOS.dmg"
FOUND_DMG=$(find "${BUILD_DIR}" "${DIST_DIR}" -maxdepth 1 -name "*.dmg" ! -name "barestats-${VERSION}-macOS.dmg" 2>/dev/null | head -n 1 || true)

if [ -n "${FOUND_DMG}" ] && [ -f "${FOUND_DMG}" ]; then
    echo "Found CPack DMG: ${FOUND_DMG} -> copying to ${DMG_DEST}"
    cp -f "${FOUND_DMG}" "${DMG_DEST}"
elif [ ! -f "${DMG_DEST}" ] && command -v hdiutil >/dev/null 2>&1; then
    echo "Generating DMG directly via hdiutil: ${DMG_DEST}"
    hdiutil create -volname "BareStats" -srcfolder "${BUILD_DIR}/BareStats.app" -ov -format UDZO "${DMG_DEST}"
fi

# Generate barestats-<version>-macOS.tar.gz containing BareStats.app and barestats CLI
TAR_DEST="${DIST_DIR}/barestats-${VERSION}-macOS.tar.gz"
echo "Creating macOS distribution tarball: ${TAR_DEST}"
if [ -f "${BUILD_DIR}/barestats" ]; then
    tar -czf "${TAR_DEST}" -C "${BUILD_DIR}" BareStats.app barestats
else
    tar -czf "${TAR_DEST}" -C "${BUILD_DIR}" BareStats.app
fi

# Backward compatibility copy
cp -f "${TAR_DEST}" "${DIST_DIR}/BareStats-macOS-Universal.app.tar.gz"

# Compute individual SHA-256 manifests
cd "${DIST_DIR}"
for f in "barestats-${VERSION}-macOS.dmg" "barestats-${VERSION}-macOS.tar.gz" "BareStats-macOS-Universal.app.tar.gz"; do
    if [ -f "$f" ]; then
        if command -v shasum >/dev/null 2>&1; then
            shasum -a 256 "$f" | awk '{print $1}' > "${f}.sha256"
        elif command -v sha256sum >/dev/null 2>&1; then
            sha256sum "$f" | awk '{print $1}' > "${f}.sha256"
        fi
        echo "Manifest generated: ${f}.sha256 ($(cat "${f}.sha256"))"
    fi
done

# Generate aggregated SHA256SUMS.txt
if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 $(ls -1 | grep -v '\.sha256$' | grep -v '^SHA256SUMS') > SHA256SUMS.txt
elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum $(ls -1 | grep -v '\.sha256$' | grep -v '^SHA256SUMS') > SHA256SUMS.txt
fi
cp -f SHA256SUMS.txt SHA256SUMS 2>/dev/null || true

echo "============================================================"
echo "macOS build and packaging complete!"
echo "Distribution directory: ${DIST_DIR}"
ls -lh "${DIST_DIR}"
echo "============================================================"
