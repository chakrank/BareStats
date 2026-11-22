#!/usr/bin/env python3
"""
BareStats Native RPM Package Builder
Uses librpm/librpmbuild to generate standalone .rpm packages without requiring
external rpmbuild binary installations.
"""

import os
import sys
import shutil
import ctypes
import rpm
from pathlib import Path

def build_rpm(repo_root: Path, output_dir: Path):
    stage_dir = Path("/tmp/barestats_rpm_stage")
    shutil.rmtree(stage_dir, ignore_errors=True)

    for sub in ["SPECS", "RPMS", "BUILD", "BUILDROOT", "SOURCES", "SRPMS"]:
        (stage_dir / sub).mkdir(parents=True, exist_ok=True)

    binary_path = output_dir / "barestats-linux-x86_64"
    if not binary_path.exists():
        binary_path = repo_root / "build" / "barestats"
    if not binary_path.exists():
        binary_path = Path("/tmp/barestats_build/barestats")
    if not binary_path.exists():
        print(f"Error: Could not locate compiled Linux binary at {binary_path}", file=sys.stderr)
        sys.exit(1)

    spec_content = f"""Name:           barestats
Version:        1.4.0
Release:        1
Summary:        Ultra-lightweight native telemetry overlay
Group:          Applications/System
License:        MIT
URL:            https://github.com/chakrank/BareStats
Vendor:         Chakrank
Packager:       Chakrank <chakrank@outlook.com>

Requires:       cairo
Requires:       libX11
Requires:       libXfixes
Requires:       libXcomposite
Requires:       libXrender

Provides:       barestats = 1.4.0-1
Provides:       barestats(x86-64) = 1.4.0-1
Provides:       application()
Provides:       application(barestats.desktop)
Provides:       appdata()
Provides:       appdata(io.github.chakrank.barestats.metainfo.xml)
Provides:       metainfo()
Provides:       metainfo(io.github.chakrank.barestats.metainfo.xml)

%description
BareStats is an unprivileged, ultra-lightweight telemetry overlay delivering
real-time CPU, GPU, VRAM, RAM, and network throughput at 0.00% idle duty cycle.
Engineered for local AI/ML workloads, competitive gaming, and workstation productivity.
Developed by Chakrank.

%install
mkdir -p %{{buildroot}}%{{_bindir}}
mkdir -p %{{buildroot}}%{{_datadir}}/applications
mkdir -p %{{buildroot}}%{{_datadir}}/pixmaps
mkdir -p %{{buildroot}}%{{_datadir}}/icons
mkdir -p %{{buildroot}}%{{_datadir}}/metainfo
mkdir -p %{{buildroot}}%{{_datadir}}/doc/barestats

cp {binary_path} %{{buildroot}}%{{_bindir}}//barestats
chmod 755 %{{buildroot}}%{{_bindir}}/barestats

cp {repo_root}/resources/linux/barestats.desktop %{{buildroot}}%{{_datadir}}/applications/barestats.desktop
chmod 644 %{{buildroot}}%{{_datadir}}/applications/barestats.desktop

cp -r {repo_root}/resources/linux/icons/hicolor %{{buildroot}}%{{_datadir}}/icons/

cp {repo_root}/resources/linux/barestats.png %{{buildroot}}%{{_datadir}}/pixmaps/barestats.png
cp {repo_root}/resources/linux/io.github.chakrank.barestats.png %{{buildroot}}%{{_datadir}}/pixmaps/io.github.chakrank.barestats.png

cp {repo_root}/resources/linux/io.github.chakrank.barestats.metainfo.xml %{{buildroot}}%{{_datadir}}/metainfo/io.github.chakrank.barestats.metainfo.xml

cp {repo_root}/config.example.ini %{{buildroot}}%{{_datadir}}/doc/barestats/config.example.ini
cp {repo_root}/LICENSE %{{buildroot}}%{{_datadir}}/doc/barestats/copyright
cp {repo_root}/README.md %{{buildroot}}%{{_datadir}}/doc/barestats/README.md
cp {repo_root}/CHANGELOG.md %{{buildroot}}%{{_datadir}}/doc/barestats/CHANGELOG.md

%post
rm -f %{{_datadir}}/applications/io.github.chakrank.barestats.desktop
if [ -x %{{_bindir}}/update-desktop-database ]; then
    %{{_bindir}}/update-desktop-database -q %{{_datadir}}/applications || true
fi
if [ -x %{{_bindir}}/gtk-update-icon-cache ]; then
    %{{_bindir}}/gtk-update-icon-cache -q -t -f %{{_datadir}}/icons/hicolor || true
fi

%preun
if [ $1 -eq 0 ]; then
    killall -q barestats 2>/dev/null || pkill -x barestats 2>/dev/null || true
fi

%postun
if [ -x %{{_bindir}}/update-desktop-database ]; then
    %{{_bindir}}/update-desktop-database -q %{{_datadir}}/applications || true
fi
if [ -x %{{_bindir}}/gtk-update-icon-cache ]; then
    %{{_bindir}}/gtk-update-icon-cache -q -t -f %{{_datadir}}/icons/hicolor || true
fi

%files
%{{_bindir}}/barestats
%{{_datadir}}/applications/barestats.desktop
%{{_datadir}}/pixmaps/*
%{{_datadir}}/icons/hicolor/*/apps/*
%{{_datadir}}/metainfo/io.github.chakrank.barestats.metainfo.xml
%{{_datadir}}/doc/barestats/*
"""

    spec_path = stage_dir / "SPECS" / "barestats.spec"
    with open(spec_path, "w") as f:
        f.write(spec_content)

    librpm = ctypes.CDLL("librpm.so.10")
    librpmbuild = ctypes.CDLL("librpmbuild.so.10")

    librpm.rpmReadConfigFiles(None, None)

    rpm.addMacro("_topdir", str(stage_dir))
    rpm.addMacro("_tmppath", "/tmp")
    rpm.addMacro("___build_pre", "umask 022\n")
    rpm.addMacro("__os_install_post", "%{nil}")
    rpm.addMacro("__check_files", "%{nil}")

    librpmbuild.rpmSpecParse.restype = ctypes.c_void_p
    librpmbuild.rpmSpecParse.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p]

    librpmbuild.rpmSpecPkgIterInit.restype = ctypes.c_void_p
    librpmbuild.rpmSpecPkgIterInit.argtypes = [ctypes.c_void_p]

    librpmbuild.rpmSpecPkgIterNext.restype = ctypes.c_void_p
    librpmbuild.rpmSpecPkgIterNext.argtypes = [ctypes.c_void_p]

    librpmbuild.rpmSpecPkgIterFree.restype = None
    librpmbuild.rpmSpecPkgIterFree.argtypes = [ctypes.c_void_p]

    librpmbuild.rpmSpecPkgHeader.restype = ctypes.c_void_p
    librpmbuild.rpmSpecPkgHeader.argtypes = [ctypes.c_void_p]

    librpm.headerDel.restype = ctypes.c_int
    librpm.headerDel.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    spec = librpmbuild.rpmSpecParse(str(spec_path).encode(), 0, None)
    if not spec:
        print("Failed to parse spec file", file=sys.stderr)
        sys.exit(1)

    # Remove duplicated tags introduced during parse to prevent RPM build abort
    piter = librpmbuild.rpmSpecPkgIterInit(spec)
    while True:
        pkg = librpmbuild.rpmSpecPkgIterNext(piter)
        if not pkg:
            break
        hdr = librpmbuild.rpmSpecPkgHeader(pkg)
        for tag in [1021, 1132, 1122, 1044]:
            librpm.headerDel(hdr, tag)
    librpmbuild.rpmSpecPkgIterFree(piter)

    librpm.rpmtsCreate.restype = ctypes.c_void_p
    librpm.rpmtsCreate.argtypes = []

    librpmbuild.rpmSpecBuild.restype = ctypes.c_int
    librpmbuild.rpmSpecBuild.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

    class RpmBuildArguments(ctypes.Structure):
        _fields_ = [
            ("pkgFlags", ctypes.c_int),
            ("buildAmount", ctypes.c_int),
            ("buildRootOverride", ctypes.c_char_p),
            ("cookie", ctypes.c_char_p),
            ("rootdir", ctypes.c_char_p),
        ]

    ts = librpm.rpmtsCreate()
    build_amount = rpm.RPMBUILD_INSTALL | rpm.RPMBUILD_PACKAGEBINARY
    args = RpmBuildArguments(0, build_amount, None, None, None)

    rc = librpmbuild.rpmSpecBuild(ts, spec, ctypes.byref(args))
    if rc != 0:
        print(f"Error: rpmSpecBuild returned non-zero code: {rc}", file=sys.stderr)
        sys.exit(rc)

    rpm_candidates = list((stage_dir / "RPMS" / "x86_64").glob("*.rpm"))
    if not rpm_candidates:
        print("Error: No RPM artifact found in RPMS/x86_64", file=sys.stderr)
        sys.exit(1)

    src_rpm = rpm_candidates[0]
    dest_rpm = output_dir / "barestats-1.4.0-1.x86_64.rpm"
    shutil.copy2(src_rpm, dest_rpm)
    print(f"==> Successfully built and deployed RPM: {dest_rpm}")

if __name__ == "__main__":
    repo = Path(__file__).resolve().parent.parent.parent
    if len(sys.argv) > 1:
        out = Path(sys.argv[1]).resolve()
    else:
        out = repo / "build" / "linux"
    out.mkdir(parents=True, exist_ok=True)
    build_rpm(repo, out)
