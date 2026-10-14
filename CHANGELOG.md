# Changelog

All notable changes to **BareStats** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

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
