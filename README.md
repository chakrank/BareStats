# BareStats

> **An ultra-lightweight, zero-overhead native system telemetry overlay.**  
> *"Don't build another hardware-monitoring suite. Use the operating system's existing telemetry wherever possible, and only query hardware-specific APIs for information the OS does not expose. Small, transparent, private, native, and predictable."*

---

## Overview

**BareStats** (TinyMonitor) is a minimalist, privacy-focused native system telemetry overlay engineered to present critical hardware performance metrics as a floating, transparent, always-on-top single-line status bar. It eliminates the heavy footprint, background services, and bloat of traditional monitoring suites while providing real-time visibility into CPU, GPU, and RAM telemetry.

```text
│ CPU 12% │ GPU 45% 4.2/16GB 52°C 85W 40% │ RAM 12.3/16GB │
```

### Platform Support & Roadmap
- **Windows (v0.6 Active / v1.0 Target):** Native Win32 implementation (Windows 10 / 11 x64).
- **Linux (Planned):** Native Wayland/X11 overlay with `/proc` and `sysfs` telemetry in the roadmap.
- **macOS (Planned):** Native floating menu/bar overlay via Metal and IOKit in the roadmap.

---

## Key Highlights

- **Near-Zero Memory Footprint:** Trims resident memory down to **~3.9 MB** working set (WS).
- **Decoupled Telemetry Worker Architecture:** Hardware polling runs on a dedicated background worker thread with conditional sleep; the UI rendering loop runs purely event-driven and unblocked.
- **15-Nanosecond Dirty-Frame Detection:** Completely skips rasterization and layered window updates when metrics are identical, dropping idle overhead to true **0.00%**.
- **Display Sleep / Screen-Off Suspension:** Monitors display power states and automatically suspends telemetry sampling when screens sleep or enter headless mode to prevent battery drain.
- **Per-Monitor High-DPI Scaling:** Crystal-clear vector graphics and typography scaling across high-DPI displays (125%, 150%, 200%) and multi-monitor setups.
- **Hardware Driver Resilience:**
  - Auto-reconnects hardware telemetry upon system resume from sleep/standby.
  - Gracefully recovers from GPU driver resets (TDR) with fallback bridging until the driver recovers.
- **Graceful OS Shutdown:** Clean lifecycle hooks ensure the application never delays or blocks system restarts or shutdowns.
- **Branded & Shell Integrated:**
  - Embedded multi-resolution hardware telemetry icon (`.ico`) in application manager and system tray.
  - "Start with System" autostart toggle.
  - Shell crash recovery (auto-restores tray icon if the desktop shell restarts).
  - Zero-latency pop-down recovery when system flyouts dismiss.
- **Portable & Multi-User Ready:**
  - Portable mode: loads from the local application directory if writable.
  - Multi-user / installed mode: seamlessly stores configuration in user app data when deployed to system directories.

---

## Controls & Hotkeys

| Action | Control / Hotkey | Description |
| :--- | :--- | :--- |
| **Move Mode** | <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> *(or <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>M</kbd>)* | Activates interactive drag box with 4-way vector move handle. Click and drag anywhere on the overlay. |
| **Lock Position** | Left-click outside or toggle hotkey | Saves position to config and restores click-through transparency. |
| **System Tray** | Left-Click icon | Toggles Move Mode on/off. |
| **System Tray** | Right-Click icon | Opens context menu: Reposition, Reset to Bottom-Center, Start with Windows toggle, and Exit. |

---

## Configuration Reference

BareStats automatically generates a `config.ini` file if none exists. Example configuration:

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

[Telemetry]
refresh_interval_ms = 1000
show_cpu = 1
show_gpu = 1
show_ram = 1
show_gpu_temp = 1
show_gpu_power = 1
show_gpu_fan = 1
show_max_memory = 1
hide_offline_metrics = 0
```

Refer to [`config.example.ini`](config.example.ini) for full documentation of every option.

---

## Building from Source (Windows v0.6)

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

The resulting `BareStats.exe` is a single standalone executable (~163 KB) with zero third-party runtime dependencies.

### Command-Line Verification
Run in command prompt to test telemetry collectors and exit:
```cmd
BareStats.exe --test
```

---

## License

BareStats is released under the [MIT License](LICENSE).
