#!/usr/bin/env bash
# ==============================================================================
# BareStats - Automated Linux Multi-Package Builder & Checksum Generator
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="/tmp/barestats_build"
OUTPUT_DIR="${REPO_ROOT}/build/linux"
PACKAGES_DIR="${BUILD_DIR}/packages"

echo "================================================================="
echo "  BareStats v1.3.0 - Linux Multi-Package Build & Checksum Suite  "
echo "================================================================="

# 1. Clean previous staging directories
echo "==> Cleaning old build and packages artifacts..."
rm -rf "${PACKAGES_DIR}"
mkdir -p "${PACKAGES_DIR}"
mkdir -p "${OUTPUT_DIR}"

# 2. Configure and Compile Release Binary
echo "==> Configuring CMake Release Build..."
cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release

# 3. Build .deb and .rpm via CPack
echo "==> Packaging Debian (.deb) and RPM (.rpm)..."
CPACK_GENERATORS="DEB"
if command -v rpmbuild >/dev/null 2>&1; then
    CPACK_GENERATORS="DEB;RPM"
fi
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -B "${PACKAGES_DIR}" -G "${CPACK_GENERATORS}"

# 4. Build Universal Tarball with Installer Scripts
echo "==> Packaging Universal Tarball (.tar.gz)..."
TARBALL_STAGE="${BUILD_DIR}/tarball-stage/barestats-1.3.0-linux-x86_64"
rm -rf "${BUILD_DIR}/tarball-stage"
mkdir -p "${TARBALL_STAGE}"
cp "${BUILD_DIR}/barestats" "${TARBALL_STAGE}/barestats"
cp "${REPO_ROOT}/resources/linux/barestats.desktop" "${TARBALL_STAGE}/barestats.desktop"
cp "${REPO_ROOT}/resources/linux/barestats.png" "${TARBALL_STAGE}/barestats.png"
cp "${REPO_ROOT}/resources/linux/barestats.svg" "${TARBALL_STAGE}/barestats.svg"
cp "${REPO_ROOT}/resources/linux/io.github.chakrank.barestats.metainfo.xml" "${TARBALL_STAGE}/io.github.chakrank.barestats.metainfo.xml"
cp "${REPO_ROOT}/config.example.ini" "${TARBALL_STAGE}/config.example.ini"
cp "${REPO_ROOT}/LICENSE" "${TARBALL_STAGE}/LICENSE"
cp "${REPO_ROOT}/README.md" "${TARBALL_STAGE}/README.md"
cp "${REPO_ROOT}/packaging/tarball/install.sh" "${TARBALL_STAGE}/install.sh"
cp "${REPO_ROOT}/packaging/tarball/uninstall.sh" "${TARBALL_STAGE}/uninstall.sh"
chmod +x "${TARBALL_STAGE}/barestats" "${TARBALL_STAGE}/install.sh" "${TARBALL_STAGE}/uninstall.sh"
tar -czf "${PACKAGES_DIR}/barestats-1.3.0-linux-x86_64.tar.gz" -C "${BUILD_DIR}/tarball-stage" barestats-1.3.0-linux-x86_64

# 5. Build Arch Linux Package (.pkg.tar.zst)
if command -v zstd >/dev/null 2>&1; then
    echo "==> Packaging Arch Linux (.pkg.tar.zst)..."
    ARCH_STAGE="${BUILD_DIR}/arch-pkg/root"
    rm -rf "${BUILD_DIR}/arch-pkg"
    mkdir -p "${ARCH_STAGE}"
    DESTDIR="${ARCH_STAGE}" cmake --install "${BUILD_DIR}" --prefix /usr
    strip --strip-unneeded "${ARCH_STAGE}/usr/bin/barestats" || true
    SIZE=$(du -sb "${ARCH_STAGE}/usr" | cut -f1)
    NOW=$(date +%s)
    cat << EOF > "${ARCH_STAGE}/.PKGINFO"
pkgname = barestats
pkgbase = barestats
pkgver = 1.3.0-1
pkgdesc = BareStats - Ultra-lightweight native telemetry overlay
url = https://github.com/chakrank/BareStats
builddate = ${NOW}
packager = Chakrank <chakrank@hotmail.com>
size = ${SIZE}
arch = x86_64
license = MIT
depend = cairo
depend = libx11
depend = libxfixes
optdepend = nvidia-utils: NVIDIA GPU hardware telemetry via NVML
EOF
    tar --zstd -cf "${PACKAGES_DIR}/barestats-1.3.0-1-x86_64.pkg.tar.zst" -C "${ARCH_STAGE}" .PKGINFO usr
fi

# 6. Build Universal AppImage
if [ -f "${SCRIPT_DIR}/appimage/build_appimage.sh" ]; then
    echo "==> Packaging Universal AppImage..."
    bash "${SCRIPT_DIR}/appimage/build_appimage.sh" || echo "Warning: AppImage packaging skipped (requires network/linuxdeploy)"
fi

# 7. Clear old binaries in output directory
echo "==> Clearing old binaries in ${OUTPUT_DIR}..."
find "${OUTPUT_DIR}" -mindepth 1 -delete

# 8. Copy new packages into build/linux
echo "==> Populating ${OUTPUT_DIR} with fresh release artifacts..."
# Copy standalone binary
if [ -f "${BUILD_DIR}/barestats" ]; then
    cp "${BUILD_DIR}/barestats" "${OUTPUT_DIR}/barestats-linux-x86_64"
    chmod +x "${OUTPUT_DIR}/barestats-linux-x86_64"
fi

# Copy all packages from packages/
if [ -d "${PACKAGES_DIR}" ]; then
    cp -f "${PACKAGES_DIR}"/* "${OUTPUT_DIR}/" 2>/dev/null || true
fi

# 7. Generate Individual and Unified Checksums
echo "==> Generating SHA-256 Checksums..."
cd "${OUTPUT_DIR}"

# Remove any old checksum files
rm -f *.sha256 SHA256SUMS SHA256SUMS.txt

for f in *; do
    if [ -f "$f" ]; then
        sha256sum "$f" | awk '{print $1}' > "${f}.sha256"
    fi
done

# Generate unified SHA256SUMS.txt
sha256sum $(ls -1 | grep -v '\.sha256$' | grep -v '^SHA256SUMS') > SHA256SUMS.txt
cp SHA256SUMS.txt SHA256SUMS

echo "================================================================="
echo "  Build & Checksums Completed Successfully! Artifacts in:        "
echo "  ${OUTPUT_DIR}                                                  "
echo "================================================================="
ls -lh "${OUTPUT_DIR}"
