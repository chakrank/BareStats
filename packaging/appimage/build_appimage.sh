#!/usr/bin/env bash
# ==============================================================================
# BareStats - Universal Linux AppImage Build Script
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="/tmp/barestats_build"
APPDIR="${BUILD_DIR}/AppDir"
OUTPUT_DIR="${BUILD_DIR}/packages"

echo "=== Building BareStats Universal AppImage ==="

# 1. Ensure binary is compiled
if [ ! -f "${BUILD_DIR}/barestats" ]; then
    echo "Compiling Release binary..."
    cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --config Release
fi

# 2. Prepare AppDir layout
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/share/applications"
mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${APPDIR}/usr/share/metainfo"
mkdir -p "${OUTPUT_DIR}"

cp "${BUILD_DIR}/barestats" "${APPDIR}/usr/bin/barestats"
cp "${REPO_ROOT}/resources/linux/barestats.desktop" "${APPDIR}/usr/share/applications/barestats.desktop"
cp "${REPO_ROOT}/resources/linux/barestats.desktop" "${APPDIR}/barestats.desktop"
cp "${REPO_ROOT}/resources/linux/barestats.png" "${APPDIR}/usr/share/icons/hicolor/256x256/apps/barestats.png"
cp "${REPO_ROOT}/resources/linux/barestats.png" "${APPDIR}/barestats.png"
cp "${REPO_ROOT}/resources/linux/io.github.chakrank.barestats.metainfo.xml" "${APPDIR}/usr/share/metainfo/"
cp "${REPO_ROOT}/config.example.ini" "${APPDIR}/usr/bin/config.example.ini"

# AppRun launcher
cat << 'EOF' > "${APPDIR}/AppRun"
#!/bin/sh
HERE="$(dirname "$(readlink -f "${0}")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH:-}"
export XDG_DATA_DIRS="${HERE}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "${HERE}/usr/bin/barestats" "$@"
EOF
chmod +x "${APPDIR}/AppRun"

# 3. Download and cryptographically verify linuxdeploy if not present
LINUXDEPLOY="${BUILD_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"

if [ ! -f "${LINUXDEPLOY}" ]; then
    echo "Downloading linuxdeploy (${LINUXDEPLOY_URL})..."
    if command -v curl >/dev/null 2>&1; then
        curl -sLo "${LINUXDEPLOY}" "${LINUXDEPLOY_URL}"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "${LINUXDEPLOY}" "${LINUXDEPLOY_URL}"
    else
        echo "Error: curl or wget required to fetch linuxdeploy" >&2
        exit 1
    fi

    echo "Verifying linuxdeploy cryptographic checksum..."
    ACTUAL_SHA256="$(sha256sum "${LINUXDEPLOY}" | awk '{print $1}')"
    if [ "${ACTUAL_SHA256}" != "${LINUXDEPLOY_SHA256}" ]; then
        echo "Error: linuxdeploy SHA-256 checksum mismatch!" >&2
        echo "Expected: ${LINUXDEPLOY_SHA256}" >&2
        echo "Actual:   ${ACTUAL_SHA256}" >&2
        rm -f "${LINUXDEPLOY}"
        exit 1
    fi
    chmod +x "${LINUXDEPLOY}"
fi

# 4. Generate AppImage
echo "Packaging AppImage..."
export VERSION="1.4.0"
export APPIMAGE_EXTRACT_AND_RUN=1
(cd "${BUILD_DIR}" && "${LINUXDEPLOY}" --appdir "${APPDIR}" --output appimage)

mv "${BUILD_DIR}"/BareStats*.AppImage "${OUTPUT_DIR}/BareStats-1.4.0-x86_64.AppImage" 2>/dev/null || \
mv "${REPO_ROOT}"/BareStats*.AppImage "${OUTPUT_DIR}/BareStats-1.4.0-x86_64.AppImage" 2>/dev/null || true

echo "=== Universal AppImage packaged successfully in: ${OUTPUT_DIR}/ ==="
