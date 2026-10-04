# Contributing to BareStats

Thank you for your interest in contributing to **BareStats**!

BareStats is engineered to be an ultra-lightweight, high-performance, and non-invasive desktop telemetry overlay for Windows. To maintain these guarantees, all contributions must adhere to the foundational architectural principles outlined below.

---

## Architectural Principles & Non-Negotiables

Before submitting a feature request or code changes, ensure your contribution respects these non-negotiable design constraints:

1. **Zero Kernel Drivers:** BareStats will never load `.sys` ring-0 drivers or open handles to vulnerable third-party driver interfaces.
2. **Zero Process Injection & Hooking:** BareStats will never call `CreateRemoteThread`, inject foreign DLLs, or hook 3D graphics rendering swapchains (DirectX/Vulkan).
3. **Zero External Runtime Dependencies:** The release binary must compile as a single standalone executable using native Windows SDK interfaces and dynamic runtime linking.
4. **Zero Remote Telemetry (100% Offline):** BareStats must never initiate outbound network connections or collect user analytics.
5. **Ultra-Low Overhead:** Memory footprint must remain minimal (~4.5 MB working set) and idle CPU duty cycle must maintain 0.00% via dirty-frame rasterization bypasses.

---

## Development Setup & Building

### Prerequisites
* Windows 10 or Windows 11 (x64)
* Visual Studio 2022+ (Desktop Development with C++ workload)
* CMake 3.20 or newer
* Ninja build system

### Local Build Commands
```cmd
# Configure release build
cmake -B build -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Compile standalone executable
cmake --build build --config Release

# Run automated telemetry verification & benchmarks
build\BareStats.exe --test
```

---

## Contribution Workflow

1. **Check Existing Issues:** Before implementing a new feature, search existing issues and discussions to confirm alignment with the project roadmap.
2. **Branch from `main`:** Create a focused feature branch for your changes:
   ```cmd
   git checkout -b feat/my-enhancement
   ```
3. **Verify Locally:**
   * Ensure code compiles cleanly with `/W4 /utf-8 /permissive-` without warnings.
   * Run `build\BareStats.exe --test` to ensure all collector routines and fallback paths pass.
   * Test interactive overlay behavior (drag-to-reposition, hotkeys, screen resolution changes).
4. **Submit a Pull Request:**
   * Provide a clear description explaining the *why* and *how* of your changes.
   * Include verification logs or screenshots where applicable.
