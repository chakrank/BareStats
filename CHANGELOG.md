# Changelog

All notable changes to **BareStats** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

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
