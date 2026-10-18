# Changelog

All notable changes to **BareStats** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.3.0] - 2026-10-18

### Added
- **macOS Native Telemetry & Overlay Engine:**
  - Full native port supporting both **Apple Silicon (`arm64`)** and **Intel (`x86_64`)** via Universal Binaries.
  - **Unprivileged Native Telemetry Pipeline (`src/metrics/macos/`):**
    - **CPU:** Mach kernel `host_statistics64(HOST_CPU_LOAD_INFO)` computing aggregate kernel, user, and idle tick deltas across all cores.
    - **RAM:** Mach virtual memory subsystem `host_statistics64(HOST_VM_INFO64)` with precise App + Wired + Compressed memory calculations.
    - **GPU & Unified Memory:** Direct Metal device query (`MTLCopyAllDevices`) for real-time allocated VRAM tracking (`currentAllocatedSize`) combined with unprivileged IOKit `IOAccelerator` `PerformanceStatistics` for device compute load percentage. Native discrete GPU support for Intel/AMD Macs.
    - **Network:** BSD sockets `getifaddrs` with automatic loopback, virtual, and tunnel interface filtering, and `sysctl(NET_RT_DUMP)` default gateway routing lookup.
  - **Native Cocoa/AppKit Floating Overlay Window:**
    - Custom borderless `NSPanel` configured at `kCGFloatingWindowLevel` with `.ignoresMouseEvents = YES` for true OS-level click-through transparency.
    - Hardware-accelerated 2D rasterization using CoreGraphics and CoreText with dirty-frame hash bypasses (FNV-1a).
    - Mathematical Ease-Out Quadratic rolling number startup animation at 60 Hz.
    - Multi-display Component Island Mode (`grid_mode = islands`) distributed seamlessly across connected `NSScreen`s.
  - **macOS Menu Bar Status Item (`NSStatusItem`):**
    - Custom vector template icon dynamically adapting to macOS light and dark menu bar themes.
    - Comprehensive control menu: Toggle Reposition Mode, Presets (Bottom Center, Top Right, Top Left, etc.), Toggle Visibility, Reload Config, and Exit.
    - Accessory process mode (`LSUIElement = true` / `NSApplicationActivationPolicyAccessory`) running unobtrusively in the menu bar without occupying Dock or `Cmd+Tab` switcher space.
  - **Interactive Reposition Mode:**
    - Live mouse drag repositioning with golden bounding guide, click-outside monitor (`NSEvent` global/local monitoring), and `Esc` key cancellation.
  - **Single-Instance Enforcement & Local IPC:**
    - POSIX `flock` file locking on `barestats.lock` with local UNIX domain socket IPC (`barestats.sock`).
    - Seamless CLI command forwarding (`--toggle`, `--move`, `--preset`, `--reload`, `--quit`, `--test`).
  - **Application Bundle & Packaging:**
    - Native standalone `BareStats.app` bundle generation with embedded `Info.plist`.
    - Drag-and-Drop `.dmg` packaging via CPack (`DragNDrop` generator) and standalone build script (`packaging/macos/build_macos.sh`).
    - CI build and verification matrix running on `macos-latest` (Apple Silicon) and `macos-13` (Intel x86_64).
- **Linux Cross-Platform Parity & Multi-Distribution Packaging:**
  - Native Linux overlay using X11 / Cairo 32-bit ARGB 100% transparent surface with XFixes shape input passthrough.
  - Unprivileged telemetry engine via `/proc/stat`, `/proc/meminfo`, `/proc/net/dev`, `/sys/class/drm`, and `/sys/class/hwmon`.
  - Single-instance POSIX PID mutex and file-locking enforcement.
  - Interactive Move Mode with amber bounding guide and click-outside exit.
  - Mathematical Ease-Out Quadratic rolling number startup animation.
  - Multi-window Component Island Mode (`grid_mode = islands`) across multi-monitor topologies.
  - Comprehensive Linux packaging pipeline: `.deb` (Debian/Ubuntu), `.rpm` (Fedora/openSUSE), `.pkg.tar.zst` (Arch Linux), universal AppImage, portable `.tar.gz`, and stripped standalone ELF binaries.
  - Automated Docker smoke test matrix verifying package installation, execution, and cleanup across Ubuntu 24.04, Debian 12, Fedora, openSUSE Leap, and Arch Linux.
- **Windows & Cross-Platform Engine Enhancements:**
  - Expanded automated verification test suite (`BareStatsTests`) to 35 comprehensive test cases covering hardware collectors, synthetic driver mocks (NVML, ADL, IGCL), synthetic Linux procfs/sysfs parsers, formatting math, true matrix grid layouts, component island positioning, and configuration migration.
  - Hardened cross-platform compilation barriers, ensuring zero-warning compilation across MSVC (Windows), GCC/Clang (Linux), and Apple Clang (macOS).
  - Added Windows automated release packaging script (`packaging/windows/package_windows.ps1`) generating cryptographically verified SHA-256 manifests.
  - Synchronized Windows release archive (`BareStats-v1.3.0-Windows-x64.zip`) and executable (`BareStats.exe`) with updated documentation and example configurations.

## [1.2.0] - 2026-10-14

### Added
- **Mathematical Matrix Grid Layout Engine (`layout = custom_grid`):**
  - Fully customizable row-by-row matrix layout definitions (`grid_row_1 = cpu | . | . | ram`, etc.) allowing arbitrary spatial positioning of system stats across rows and columns.
  - Column cell merging: Adjacent identical cell identifiers in a row automatically merge into a single wide contiguous cell block.
  - Empty spacer tokens (`.` or `null` or empty `| |`) for freeform coordinate spacing and blank slots.
  - Fallback protection: Matrices consisting entirely of spacers safely fall back to the synthesized layout template.
  - Arbitrary static text tokens (`text:Label` or `label:Label`) rendered directly without telemetry square brackets.
- **Component Island Micro-HWND Architecture (`grid_mode = islands`):**
  - Distributed micro-HWND rendering surfaces for each non-empty matrix cell, calculating screen coordinates from a global anchor via configurable `cell_gap` and `row_gap`.
  - Eliminates fullscreen transparent layered windows across multi-monitor or wide-screen coordinate spans, preventing desktop window manager alpha-compositing bloat and preserving low memory usage.
  - Win+D (Show Desktop) resilience: Island windows actively intercept `WM_WINDOWPOSCHANGING` and re-assert `HWND_TOPMOST` Z-order via dedicated `IslandWndProc` and `ReassertTopmost()`.
  - Unified rendering mode (`grid_mode = unified`): Single compact layered window surface for dense status clusters with zero inter-cell padding.
- **Multi-GPU Grid Wrapping & Asymmetric GPU Customization (`[GPU0]`, `[GPU1]`):**
  - Automatic multi-row grid wrapping in synthesized layouts (`layout = grid`) controlled by `max_gpus_per_row` to cleanly display high-density multi-GPU workstation or server arrays.
  - Dedicated hierarchical adapter sections (`[GPU0]`, `[GPU1]`, etc.) supporting per-GPU sensor overrides (`show_temp`, `show_power`, `show_fan`, `vram_mode`) for asymmetric monitoring setups (e.g. detailed gaming sensors on primary dGPU and minimal VRAM-only monitoring on secondary AI compute cards).
  - Explicit multi-GPU cell tokens (`gpu0`, `gpu1`, `gpu0_vram`, `gpu1_vram`, `gpu0_temp`, etc.) with automatic background adapter target registration.
- **Hardware Introspection & In-Place Schema Migration:**
  - Dynamic GPU hardware introspection during initial configuration generation automatically writes detected GPU models, vendor names, and adapter indices into `config.ini` comments.
  - Non-destructive in-place configuration schema migration from v1.0/v1.1 to v1.2 (`schema_version = 120`), safely injecting new `[BareStats]`, `[Position]`, and `[GPU*]` sections while preserving existing user preferences.
- **Automated Verification Test Suite Expansion:**
  - Expanded test runner (`BareStatsTests.exe`) to 18 automated verification steps covering configuration schema migration, multi-GPU grid synthesis, custom matrix parsing, cell merging, island micro-HWND math, and driver fault tolerance.

## [1.1.0] - 2026-10-10

### Added
- **NVML v2 Driver Reservation Decoupling:** Dynamically queries `nvmlDeviceGetMemoryInfo_v2` to isolate firmware and driver static reservations from active application allocations, delivering a true 0.0 GB idle baseline.
- **Configurable VRAM Reporting Modes (`vram_mode`):**
  - `physical`: Gross hardware silicon capacity (e.g. 16.0 GB) matching retail packaging and Task Manager.
  - `usable`: Net allocatable capacity (total minus driver reservations, e.g. 15.6 GB), preventing unexpected CUDA Out of Memory (OOM) errors during local LLM model loading (Ollama, PyTorch, vLLM).
- **Native AMD ADL Telemetry Engine (`AdlCollector`):** Zero-driver runtime loading of `atiadlxx.dll` / `atiadlxy.dll` for AMD Radeon core thermals, board power wattage, and fan RPM/percentage.
- **Native Intel IGCL Telemetry Engine (`IgclCollector`):** Dynamic runtime loading of `ControlLib.dll` for Intel Arc and Iris Xe silicon to query package thermals, power draw, and fan speeds in user mode.
- **Simultaneous Side-by-Side Multi-GPU Monitoring:** Flexible `gpu_index` targeting supporting single adapters (`gpu_index = 0`), comma-separated adapter lists (`gpu_index = 0, 1`), and wildcard selection (`gpu_index = all`) with multi-GPU segment formatting (e.g. `│ GPU0 ... │ GPU1 ... │`).
- **Universal Windows D3DKMT Engine Utilization Fallback (`D3dKmtCollector`):** Unprivileged kernel scheduling query (`D3DKMTQueryStatistics` from `gdi32.dll`) providing real-time GPU engine utilization % for integrated graphics (Intel UHD / Iris Xe, AMD APUs) and Qualcomm Snapdragon X Elite (ARM64) where discrete vendor SDKs are unavailable.
- **Hardware-Agnostic Driver Mock Testing Harness (`BareStatsTests.exe`):** Implemented synthetic vendor mock libraries (`mock_atiadlxx.dll`, `mock_ControlLib.dll`, `mock_nvml.dll`) and an automated 11-step verification suite integrated with CTest and GitHub Actions CI to deterministically test driver telemetry, VRAM math, multi-GPU layouts, and fault recovery without physical hardware dependencies.
- **Fortified Driver Lifecycle & Reset Recovery:** Added double-unload protection and dynamic function pointer nullification across ADL, IGCL, and NVML recovery pipelines, preventing access violations during GPU reset and modern standby wake transitions.

## [1.0.0] - 2026-10-04

### Added
- **Native CPU Telemetry:** Multi-core kernel time slicing across all physical and logical cores without ring-0 drivers.
- **Dedicated GPU Telemetry:** Universal DXGI hardware infrastructure query with dynamic NVML runtime linking for NVIDIA core thermals, board power draw, and fan speeds.
- **Optimus Battery Guard:** Respects mobile dGPU D3-cold low-power sleep states when targeting integrated graphics.
- **Physical Network Throughput:** Zero-driver gateway throughput telemetry (`NET X.XM↓ X.XM↑`) using Windows IP Helper (`GetIfTable2`) with automatic filtering of virtual, WSL, bridge, and VPN adapters.
- **Desktop Window Manager Overlay:** Top-level click-through layered window (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`) rendered via hardware alpha compositing without hooking 3D swapchains or injecting DLLs.
- **Zero-Driver Anti-Cheat Safety:** Fully compliant alongside strict kernel anti-cheat engines (Riot Vanguard, Easy Anti-Cheat, BattlEye, Ricochet).
- **Interactive Move Mode:** Hotkey-driven (`Ctrl + Shift + M` / `Ctrl + Alt + M`) and tray-accessible drag-to-repositioning with an obsidian bounding box.
- **Instant Visibility Toggle:** Global hotkey (`Ctrl + Shift + H` / `Ctrl + Alt + H`) to instantly show or hide the overlay.
- **System Tray Management:** Right-click tray menu with repositioning toggle, reset to default, config reload, startup toggle, and exit.
- **Configurable Visual Pipeline:** Full customization via `config.ini` for font family, weight, size, palette, custom separators, dynamic threshold highlighting, drop shadows, and opacity.
- **Live Config Hot-Reload:** Non-blocking background filesystem watcher with write debouncing for seamless configuration updates.
- **High-DPI Awareness:** Native Per-Monitor V2 manifest and multi-resolution icon resources (16px to 256px).
- **Diagnostic Verification Mode:** CLI `--test` suite for hardware adapter probing, collector validation, and microsecond timing traces.
- **Multi-Runner CI Matrix:** Automated GitHub Actions workflows targeting `windows-latest` and `windows-2022` with SHA-256 release artifact generation.
