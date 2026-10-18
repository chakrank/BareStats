#!/usr/bin/env python3
"""
BareStats Cross-Platform Test Runner and Variant Verification Process
Usage:
    python scripts/run_tests.py [--build-type Release|Debug] [--skip-build] [--build-dir DIR]
"""

import os
import sys
import subprocess
import argparse
import time
import re
from pathlib import Path

def print_separator(char="=", width=64):
    print(char * width)

def run_command(cmd, cwd=None, env=None, shell=None):
    if shell is None:
        shell = isinstance(cmd, str)
    start_time = time.time()
    try:
        proc = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            shell=shell
        )
        elapsed = time.time() - start_time
        return proc.returncode, proc.stdout, elapsed
    except Exception as e:
        elapsed = time.time() - start_time
        return 1, str(e), elapsed

def check_packaging_and_manifests(repo_root):
    print("[STEP 1/4] Verifying cross-platform manifests and packaging consistency...")
    errors = []

    # 1. Project version from CMakeLists.txt
    cmakelists_path = repo_root / "CMakeLists.txt"
    if not cmakelists_path.exists():
        errors.append("CMakeLists.txt not found in repository root")
        return False, errors

    cmake_content = cmakelists_path.read_text(encoding="utf-8")
    m_cmake = re.search(r'project\s*\(\s*BareStats\s+VERSION\s+([0-9\.]+)', cmake_content, re.IGNORECASE)
    if not m_cmake:
        errors.append("Could not extract project VERSION from CMakeLists.txt")
        return False, errors

    project_version = m_cmake.group(1)
    print(f"  Target Version: v{project_version}")

    # 2. Windows Manifest
    win_manifest = repo_root / "resources" / "windows" / "BareStats.manifest"
    if win_manifest.exists():
        content = win_manifest.read_text(encoding="utf-8")
        if f'version="{project_version}.0"' not in content and f'version="{project_version}"' not in content:
            errors.append(f"BareStats.manifest version mismatch (expected {project_version}.0)")
    else:
        errors.append("resources/windows/BareStats.manifest missing")

    # 3. Linux Desktop Entry
    linux_desktop = repo_root / "resources" / "linux" / "barestats.desktop"
    if linux_desktop.exists():
        content = linux_desktop.read_text(encoding="utf-8")
        if not re.search(r'Version=(1\.[0-9]+|' + re.escape(project_version) + r')', content):
            errors.append(f"barestats.desktop version mismatch (expected Desktop Spec Version=1.5 or {project_version})")
    else:
        errors.append("resources/linux/barestats.desktop missing")

    # 4. Icon assets existence
    win_icon = repo_root / "resources" / "windows" / "bare_stats.ico"
    linux_svg = repo_root / "resources" / "linux" / "barestats.svg"
    if not win_icon.exists():
        errors.append("resources/windows/bare_stats.ico missing")
    if not linux_svg.exists():
        errors.append("resources/linux/barestats.svg missing")

    # 5. Packaging recipes check
    pkg_checks = [
        (repo_root / "packaging" / "debian" / "control", "Debian control"),
        (repo_root / "packaging" / "arch" / "PKGBUILD", "Arch PKGBUILD"),
        (repo_root / "packaging" / "rpm" / "barestats.spec", "RPM spec"),
        (repo_root / "packaging" / "windows" / "package_windows.ps1", "Windows package script"),
    ]
    for p_path, p_name in pkg_checks:
        if not p_path.exists():
            errors.append(f"Packaging recipe missing: {p_name} ({p_path})")

    if errors:
        for err in errors:
            print(f"  [FAIL] {err}")
        return False, errors

    print("  [PASS] All manifests, icons, and packaging recipes aligned.")
    return True, []

def detect_windows_msvc_env():
    if sys.platform != "win32":
        return None
    if "VCINSTALLDIR" in os.environ:
        return os.environ.copy()

    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    vcvars = None
    if vswhere.exists():
        code, out, _ = run_command([str(vswhere), "-latest", "-property", "installationPath"])
        if code == 0 and out.strip():
            candidate = Path(out.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                vcvars = candidate

    # Common hardcoded fallbacks
    if not vcvars:
        for drive in ["D:", "C:"]:
            for ver in ["18", "2022", "2019"]:
                for ed in ["Community", "Professional", "Enterprise", "BuildTools"]:
                    p = Path(f"{drive}/Program Files/Microsoft Visual Studio/{ver}/{ed}/VC/Auxiliary/Build/vcvars64.bat")
                    if p.exists():
                        vcvars = p
                        break
                if vcvars:
                    break
            if vcvars:
                break

    if not vcvars:
        return None

    cmd = f'call "{vcvars}" && set'
    code, out, _ = run_command(cmd, shell=True)
    if code != 0:
        return None

    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env

def main():
    parser = argparse.ArgumentParser(description="BareStats Automated Test and Variant Verification Suite")
    parser.add_argument("--build-type", default="Release", choices=["Release", "Debug", "RelWithDebInfo"], help="CMake build configuration")
    parser.add_argument("--build-dir", default="build", help="Build directory path")
    parser.add_argument("--skip-build", action="store_true", help="Skip CMake build and run tests directly")
    parser.add_argument("--no-cli-test", action="store_true", help="Skip CLI --test mode execution")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    build_dir = repo_root / args.build_dir

    print_separator()
    print("BareStats Automated Cross-Platform Test & Variant Runner (v1.3.0)")
    print(f"Platform: {sys.platform} | Build Type: {args.build_type} | Root: {repo_root}")
    print_separator()

    total_start = time.time()
    steps_passed = 0
    total_steps = 4 if not args.no_cli_test else 3

    # Step 1: Manifest & Packaging Checks
    pkg_ok, pkg_errs = check_packaging_and_manifests(repo_root)
    if not pkg_ok:
        print("\nAborting: Manifest or packaging discrepancies detected.")
        sys.exit(1)
    steps_passed += 1

    # Prepare Environment
    env = os.environ.copy()
    if sys.platform == "win32":
        msvc_env = detect_windows_msvc_env()
        if msvc_env:
            env = msvc_env
        # Terminate lingering instances to release executable lock before linking
        run_command(["powershell", "-NoProfile", "-Command", "Get-Process -Name 'BareStats*' -ErrorAction SilentlyContinue | Stop-Process -Force"])

    # Step 2: Build
    if not args.skip_build:
        print(f"\n[STEP 2/4] Configuring and Building ({args.build_type})...")
        cmake_configure_cmd = [
            "cmake",
            "-B", str(build_dir),
            f"-DCMAKE_BUILD_TYPE={args.build_type}",
            "-DBARESTATS_BUILD_TESTS=ON"
        ]
        code, out, elapsed = run_command(cmake_configure_cmd, cwd=str(repo_root), env=env)
        if code != 0:
            print(f"  [FAIL] CMake configuration failed ({elapsed:.2f}s):\n{out}")
            sys.exit(1)

        cmake_build_cmd = [
            "cmake",
            "--build", str(build_dir),
            "--config", args.build_type
        ]
        code, out, elapsed = run_command(cmake_build_cmd, cwd=str(repo_root), env=env)
        if code != 0:
            print(f"  [FAIL] CMake build failed ({elapsed:.2f}s):\n{out}")
            sys.exit(1)

        print(f"  [PASS] Build completed in {elapsed:.2f}s.")
    else:
        print("\n[STEP 2/4] Build skipped via --skip-build flag.")
    steps_passed += 1

    # Locate Test Executable
    test_exe_name = "BareStatsTests.exe" if sys.platform == "win32" else "BareStatsTests"
    test_exe = build_dir / test_exe_name
    if not test_exe.exists():
        test_exe_cfg = build_dir / args.build_type / test_exe_name
        if test_exe_cfg.exists():
            test_exe = test_exe_cfg

    if not test_exe.exists():
        print(f"  [FAIL] Test executable not found at {test_exe}")
        sys.exit(1)

    # Step 3: Run Unit Test Suite
    print(f"\n[STEP 3/4] Running Modular Test Suite ({test_exe.name})...")
    code, out, elapsed = run_command([str(test_exe)], cwd=str(test_exe.parent), env=env)
    print(out)
    if code != 0:
        print(f"  [FAIL] Unit tests failed with return code {code} ({elapsed:.2f}s)")
        sys.exit(code)
    print(f"  [PASS] All unit tests completed successfully in {elapsed:.2f}s.")
    steps_passed += 1

    # Step 4: CLI Mode Verification
    if not args.no_cli_test:
        print("\n[STEP 4/4] Verifying CLI Mode (--test)...")
        app_exe_name = "BareStats.exe" if sys.platform == "win32" else "barestats"
        app_exe = build_dir / app_exe_name
        if not app_exe.exists():
            app_exe_cfg = build_dir / args.build_type / app_exe_name
            if app_exe_cfg.exists():
                app_exe = app_exe_cfg

        if not app_exe.exists():
            print(f"  [FAIL] Main executable not found at {app_exe}")
            sys.exit(1)

        code, out, elapsed = run_command([str(app_exe), "--test"], cwd=str(app_exe.parent), env=env)
        if code != 0:
            print(f"  [FAIL] CLI mode --test failed with return code {code}:\n{out}")
            sys.exit(code)
        print(f"  [PASS] CLI mode verified (exit code 0 in {elapsed:.2f}s).")
        steps_passed += 1

    # Final Summary
    total_time = time.time() - total_start
    print_separator()
    print(f"Test and Verification Pipeline Succeeded: {steps_passed}/{total_steps} steps passed in {total_time:.2f}s")
    print_separator()

if __name__ == "__main__":
    main()
