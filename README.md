# BareStats

> **An ultra-lightweight, zero-overhead native system telemetry overlay.**  
> *BareStats is not another hardware-monitoring suite. Rather than bundling background services, kernel drivers, and real-time graphing engines, it relies on the operating system's native telemetry facilities first, querying vendor-specific hardware APIs strictly when necessary. Small, transparent, native, and predictable.*

---

## Overview

**BareStats** is a minimalist, privacy-focused native system telemetry overlay engineered to present vital hardware performance metrics as a floating, transparent, always-on-top single-line status bar. It eliminates the overhead, resident memory footprint, and background services typical of traditional monitoring utilities, providing high-fidelity visibility into CPU, GPU, and RAM telemetry with negligible system impact.

```text
│ CPU 12% │ GPU 45% 4.2/16GB 52°C 85W 40% │ RAM 12.3/16GB │ NET 1.2M↓ 0.4M↑ │
```

### Platform Architecture & Roadmap
- **Windows (v0.9 Active / v1.0 Target):** Pure Win32 layered window implementation for Windows 10 and Windows 11 (x64), supporting Intel, AMD, ARM64 / Snapdragon, and multi-GPU configurations.
- **Linux (Planned):** Standalone Wayland (`wlr-layer-shell`) and X11 overlay utilizing `/proc` and `sysfs` telemetry interfaces.
- **macOS (Planned):** Native floating menu panel rendered via AppKit and Metal, leveraging Mach kernel and IOKit frameworks.

---

## Technical Highlights

- **Minimal Memory Footprint:** Trims runtime working set down to **~3.9 MB** resident memory (WS) in Task Manager.
- **Decoupled Sampling Pipeline:** Telemetry polling runs on a dedicated background worker thread with conditional sleep; the Win32 UI message pump remains purely event-driven and unblocked.
- **Sub-Microsecond Dirty-Frame Bypass:** Completely skips GDI+ rasterization and layered window surface updates when telemetry values are unchanged, yielding true **0.00%** CPU usage at idle.
- **Native Network Throughput Telemetry:** Optional real-time auto-scaling network throughput (`NET 1.2M↓ 0.4M↑`) queried via Windows native `GetIfTable2` (`iphlpapi.dll`). Real-time heuristic filtering permanently discards virtual adapters, WSL bridges, Hyper-V switches, and VPN tunnels to monitor true physical LAN/WAN internet traffic.
- **Zero-Restart Live Configuration Hot-Reload:** Background file-watcher using non-blocking `ReadDirectoryChangesW` with 200ms write debouncing. Any changes to fonts, colors, margins, opacity, metric order, or poll rates saved in external editors apply instantly without restarting.
- **Hardware Opacity & Alpha Blending:** Configurable global window opacity (10% to 100%) composited directly by the Windows Desktop Window Manager (DWM).
- **Multi-GPU & Hybrid Laptop Battery Guard:** Configurable GPU adapter targeting (`gpu_index`) across discrete and integrated GPUs. Selecting an iGPU (Intel UHD/Iris, AMD Radeon) completely bypasses vendor NVML calls, keeping mobile discrete GPUs in ultra-low-power D3-cold sleep to preserve laptop battery life.
- **Display Power State Awareness:** Subscribes to display power notifications (`GUID_CONSOLE_DISPLAY_STATE`), suspending polling when monitors enter standby to conserve battery.
- **Per-Monitor V2 DPI Scaling:** Native vector and typography rendering that dynamically scales across high-DPI displays (125%, 150%, 200%) and heterogeneous multi-monitor setups without bitmap blur.
- **Hardware Driver Resilience:**
  - Auto-reconnects hardware telemetry on system resume from sleep (`PBT_APMRESUMEAUTOMATIC`).
  - Seamlessly bridges GPU driver crashes or resets (TDR) via DXGI until vendor APIs recover.
- **Clean OS Shutdown Integration:** Intercepts `WM_QUERYENDSESSION` and `WM_ENDSESSION` to guarantee the process never delays, blocks, or halts system reboots.
- **Desktop Shell Integration:**
  - Embedded multi-resolution semiconductor telemetry icon in Windows Explorer and the notification tray.
  - Non-elevated "Start with Windows" registry toggle.
  - Automatic notification tray recovery on Explorer restarts (`TaskbarCreated`).
  - Immediate topmost re-assertion when Windows shell popups (Start Menu, Action Center) dismiss.
- **Portable & Multi-User Configuration:**
  - Portable mode: loads from the application directory when writable.
  - Installed mode: automatically falls back to `%LOCALAPPDATA%\BareStats\config.ini` in write-restricted environments.

---

## Controls & Hotkeys

| Action | Control / Hotkey | Description |
| :--- | :--- | :--- |
| **Move Mode** | <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> *(or <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>M</kbd>)* | Toggles interactive repositioning mode with a 4-way vector drag box. Click and drag anywhere on the overlay. |
| **Toggle Visibility** | <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd> *(or <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>H</kbd>)* | Instantly hides or reveals the overlay window without interrupting background telemetry collection. |
| **Lock Position** | Left-click outside or re-press hotkey | Anchors position to configuration and restores click-through transparency. |
| **System Tray** | Left-Click icon | Toggles repositioning mode. |
| **System Tray** | Right-Click icon | Context menu: Reposition, Reset Position, Hide/Show Overlay, Reload Config, Start with Windows, and Exit. |

---

## Configuration Reference

BareStats automatically provisions a default `config.ini` on first run if none is present:

```ini
[Position]
monitor_index = 0
preset = bottom-center
margin = 12

[Appearance]
font_name = auto
font_weight = semibold
font_size = 13
text_color = #FFFFFF
label_color = #A0A0A0
separator_color = #6E6E6E
color_high = #FFD700
color_critical = #FF4D4D
enable_dynamic_colors = 1
drop_shadow = 1
separator = │
opacity = 100

[Telemetry]
refresh_interval_ms = 1000
gpu_index = 0
order = CPU, GPU, RAM, NET
show_cpu = 1
show_gpu = 1
show_ram = 1
show_net = 0
show_gpu_temp = 1
show_gpu_power = 1
show_gpu_fan = 1
show_max_memory = 1
hide_offline_metrics = 0
```

Consult [`config.example.ini`](config.example.ini) for full documentation of each configuration directive.

---

## Anti-Cheat Compatibility & Safety Certification

BareStats is engineered specifically to run safely alongside strict kernel-level anti-cheat solutions, including **Riot Vanguard**, **Easy Anti-Cheat (EAC)**, **BattlEye**, and **Call of Duty Ricochet**.

- **Zero Kernel Drivers:** BareStats loads no `.sys` drivers and communicates with zero ring-0 interfaces (avoiding blacklisted or vulnerable driver IOCTLs like `WinRing0` or `RTCore64`).
- **Zero DLL Injection & Hooking:** BareStats never calls `CreateRemoteThread`, `SetWindowsHookEx`, or hooks game rendering swapchains (unlike RivaTuner/MSI Afterburner).
- **Zero Memory Inspection:** BareStats never queries foreign process descriptors, scans thread contexts, or inspects process virtual memory (`PROCESS_VM_READ`).
- **Pure Layered Desktop Window:** The overlay is a standard Win32 top-level layered window (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`) composited directly by the Windows Desktop Window Manager (DWM). All user input passes through freely to fullscreen borderless games without interception.

---

## Building from Source

### Prerequisites
- Windows 10 or Windows 11 (x64)
- Microsoft Visual Studio 2022+ with C++ Desktop Development tools
- CMake 3.20 or newer

### Build Commands
```cmd
# Configure build
cmake -B build -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Compile release binary
cmake --build build --config Release
```

The build produces a single standalone executable (`BareStats.exe`, ~163 KB) with zero external runtime dependencies.

### Verification Suite
Execute from the command prompt to verify collector pipelines and exit:
```cmd
BareStats.exe --test
```

---

## License

BareStats is licensed under the [MIT License](LICENSE).
