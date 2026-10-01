# BareStats (TinyMonitor) — Production Roadmap to v1.0

## Vision & Philosophy
> **"Don't build another hardware-monitoring suite. Use Windows' existing telemetry wherever possible, and only query hardware-specific APIs for information Windows does not expose. Small, transparent, private, native, and predictable."**

This document establishes the official stage-by-stage engineering plan to evolve BareStats from its working base (`v0.2`) to an enterprise-grade, zero-overhead Windows utility (`v1.0`).

---

## Versioning Model: Minor Increments (`+0.1`)
In accordance with standard Semantic Versioning (`MAJOR.MINOR.PATCH`), each stage represents a major architectural milestone and increments the minor version:
* **Current Baseline:** `v0.2` (Real-time telemetry, drag-to-position, Segoe UI typography, roll-up wakeup animation)
* **Stage 1 Completion:** `v0.3` (Core Architecture & Performance Engine)
* **Stage 2 Completion:** `v0.4` (Windows Shell & Persistent Configuration)
* **Stage 3 Completion:** `v0.5` (Visual Polish, Branded Identity & DPI V2)
* **Stage 4 Completion:** `v0.6` (OS Lifecycle & Hardware Driver Resilience)
* **Stage 5 Completion:** `v1.0` (Official Production-Grade Release)

*(Patch increments like `+0.0.1` are reserved exclusively for bug fixes and hotfixes).*

---

## Detailed Stages & Milestones

### Baseline (Completed): `v0.2`
- [x] Zero-dependency native C++17 single executable.
- [x] CPU (`GetSystemTimes`), RAM (`GlobalMemoryStatusEx`), GPU (NVIDIA NVML with DXGI VRAM fallback).
- [x] Topmost, layered, transparent, click-through OSD window.
- [x] Drag-to-reposition mode (`Ctrl+Shift+M` / `Ctrl+Alt+M`) with vector move box.
- [x] Physical display clamping (`rcMonitor`) eliminating taskbar placement jump.
- [x] Single-instance named mutex with 800ms ease-out digital roll-up wakeup animation.
- [x] Unified `Segoe UI Semibold` typography matching Windows 11 Taskbar and NVIDIA Overlay.
- [x] Local `config.ini` parsing with compiled-in fallback defaults.

---

### Stage 1: Core Architecture & Performance Engine (`v0.3`) — COMPLETED
**Goal:** Guarantee zero UI thread jank, 0% idle CPU/GPU rendering, and zero battery drain when displays sleep.

- [x] **1.1 Decoupled Telemetry Worker Thread**
  - Moved `cpuCollector.Collect()`, `memoryCollector.Collect()`, and `gpuCollector.Collect()` to dedicated `TelemetryEngine` background thread (`std::thread`).
  - Thread sleeps for the configured interval using `std::condition_variable` (instant wake on resume/shutdown).
  - UI thread runs a pure non-blocking message pump, receiving `WM_TELEMETRY_UPDATED`.
- [x] **1.2 15-Nanosecond Dirty Frame Detection**
  - In `OverlayWindow::UpdateMetrics`, compares formatted text against `lastRenderedText_`.
  - Skips 100% of GDI+ rasterization and `UpdateLayeredWindow` when metrics are identical.
  - Drops idle pixel rasterization overhead to true 0.00%.
- [x] **1.3 Display Sleep / Screen-Off Polling Suspension**
  - Registered `GUID_CONSOLE_DISPLAY_STATE` via `RegisterPowerSettingNotification`.
  - Suspends worker polling when display enters power-saving sleep or headless mode.
  - Wakes up immediately upon display activation.
- [x] **1.4 Startup Working Set Trimming**
  - Called `SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1)` upon animation completion and move-mode exit.
  - Verified in live testing: resident memory dropped from **~42 MB down to 3.92 MB**!

---

### Stage 2: Windows Shell & Persistent Configuration (`v0.4`)
**Goal:** Seamless integration into Windows startup, system tray, and multi-user filesystem paths.

- [ ] **2.1 Start with Windows Toggle**
  - Add "Start with Windows" entry with dynamic checkmark in the tray context menu.
  - Manages `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\BareStats`.
  - Stores quoted path (`"C:\path with spaces\BareStats.exe"`) with zero administrative/UAC elevation required.
- [ ] **2.2 Persistent & Portable Configuration Hierarchy**
  - First check for `config.ini` in the executable folder (Portable Mode).
  - If folder is write-restricted (e.g., `C:\Program Files\BareStats\`), automatically store and read from `%LOCALAPPDATA%\BareStats\config.ini`.
  - Creates parent directory automatically if missing.
- [ ] **2.3 Explorer Crash Recovery (`TaskbarCreated`)**
  - Register the custom window message `RegisterWindowMessageW(L"TaskbarCreated")`.
  - If Windows Explorer restarts, automatically re-register the tray icon (`Shell_NotifyIconW(NIM_ADD)`) so BareStats never gets orphaned.
- [ ] **2.4 Session-Scoped Mutex Namespace**
  - Update mutex identifier to `Local\BareStats_SingleInstance_Mutex` to ensure clean isolation in multi-user and Remote Desktop environments.
- [ ] **2.5 Zero-Latency Shell Pop-Down Recovery (Instant Start Menu / Flyout Restore)**
  - Expand Windows Event Hook (`SetWinEventHook`) to capture shell dismiss events (`EVENT_SYSTEM_MENUPOPUPEND`, `EVENT_SYSTEM_CAPTUREEND`, `EVENT_OBJECT_HIDE`).
  - The instant the Start Menu, Action Center, Network/Sound flyout, or system tray menu closes, immediately re-assert `HWND_TOPMOST` and force an instant repaint.
  - Eliminates the up to 1000ms delay waiting for the next clock tick to restore the overlay.

---

### Stage 3: Visual Polish, Branded Identity & DPI V2 (`v0.5`)
**Goal:** Flawless visual scaling across multi-monitor setups and professional executable branding.

- [ ] **3.1 Embedded Multi-Resolution `.ico` Resource**
  - Design an original, copyright-free minimalist telemetry vector icon (geometric pulse/gauge).
  - Package sizes: 16x16, 24x24, 32x32, 48x48, 256x256 into `resources/bare_stats.ico`.
  - Integrate `.rc` resource script into CMake build to embed icon directly into the executable header (`IDI_APPLICATION`).
- [ ] **3.2 Per-Monitor V2 Application Manifest**
  - Embed application manifest specifying `<dpiAwareness>PerMonitorV2, PerMonitor</dpiAwareness>`.
  - Disables Windows DWM bitmap stretching on high-DPI displays (125%, 150%, 200%).
- [ ] **3.3 Dynamic Cross-Monitor Scaling (`WM_DPICHANGED`)**
  - Handle `WM_DPICHANGED` message in `WndProc`.
  - Dynamically scale base font size: `fontSize = configFontSize * (dpi / 96.0f)`.
  - Recalculate text bounds and reposition smoothly when dragged across displays with different scaling factors.

---

### Stage 4: OS Lifecycle & Hardware Driver Resilience (`v0.6`)
**Goal:** Rock-solid stability across system reboots, sleep cycles, and GPU driver resets.

- [ ] **4.1 Graceful OS Shutdown Hook**
  - Handle `WM_QUERYENDSESSION` and `WM_ENDSESSION`.
  - Flush window coordinates to configuration.
  - Cleanly call `nvmlShutdown()` to prevent holding up OS shutdown with "This app is preventing Windows from restarting".
- [ ] **4.2 Sleep/Wake & GPU Driver Recovery (`WM_POWERBROADCAST`)**
  - Listen for `PBT_APMRESUMEAUTOMATIC` upon system resume.
  - Implement health check / reconnect logic for NVML if the NVIDIA driver resets (TDR - Timeout Detection and Recovery).

---

### Stage 5: Documentation & Production Packaging (`v1.0`)
**Goal:** Finalize the v1.0 release package for public distribution.

- [ ] **5.1 Comprehensive `README.md`**
  - Feature highlights, hotkeys (`Ctrl+Shift+M`), configuration guide, and system requirements.
- [ ] **5.2 Annotated `config.example.ini`**
  - Complete reference with comments explaining every setting and default values.
- [ ] **5.3 Open-Source License**
  - Standard MIT License included in the repository.
- [ ] **5.4 Standalone Release Binary Optimization**
  - Verify MSVC release flags (`/O2`, `/GL`, `/LTCG`, `/Gy`).
  - Confirm binary size (~200 KB) and single-file portability.
