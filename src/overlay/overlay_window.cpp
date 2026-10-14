#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay_window.h"
#include "monitor_helper.h"
#include "../util/formatter.h"
#include "../../resources/resource.h"
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <unknwn.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

namespace BareStats {

UINT OverlayWindow::s_uTaskbarRestartMsg = 0;
static HHOOK s_hMouseHook = nullptr;
static HWINEVENTHOOK s_hWinEventHook = nullptr;
static OverlayWindow* s_pOverlayInstance = nullptr;

static LRESULT CALLBACK IslandWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_WINDOWPOSCHANGING: {
            auto* pwp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (s_pOverlayInstance && !s_pOverlayInstance->IsPositioningMode()) {
                pwp->hwndInsertAfter = HWND_TOPMOST;
            }
            pwp->flags |= SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (s_pOverlayInstance && !s_pOverlayInstance->IsPositioningMode()) {
        s_pOverlayInstance->ReassertTopmost();
    }
}

using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
static UINT FetchDpiForWindow(HWND hwnd) {
    static auto pfn = reinterpret_cast<PFN_GetDpiForWindow>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pfn && hwnd) {
        UINT dpi = pfn(hwnd);
        if (dpi > 0) return dpi;
    }
    return 96;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) {
            auto* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (s_pOverlayInstance && s_pOverlayInstance->IsPositioningMode()) {
                HWND hwnd = s_pOverlayInstance->GetHwnd();
                RECT rc{};
                if (hwnd && GetWindowRect(hwnd, &rc)) {
                    POINT pt = pMouse->pt;
                    if (!PtInRect(&rc, pt)) {
                        PostMessageW(hwnd, WM_COMMAND, OverlayWindow::ID_EXIT_MOVE_MODE, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
}

OverlayWindow::OverlayWindow(AppConfig& config)
    : config_(config) {
}

OverlayWindow::~OverlayWindow() {
    if (s_hWinEventHook) {
        UnhookWinEvent(s_hWinEventHook);
        s_hWinEventHook = nullptr;
    }
    if (s_hMouseHook) {
        UnhookWindowsHookEx(s_hMouseHook);
        s_hMouseHook = nullptr;
    }
    s_pOverlayInstance = nullptr;

    RemoveTrayIcon();

    if (hPowerNotify_) {
        UnregisterPowerSettingNotification(hPowerNotify_);
        hPowerNotify_ = nullptr;
    }

    DestroyIslandWindows();

    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_M);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_ALT_M);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_ALT_P);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_H);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_ALT_H);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (gdiplusToken_) {
        GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<OverlayWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (s_uTaskbarRestartMsg != 0 && msg == s_uTaskbarRestartMsg) {
        SetupTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_CTRL_SHIFT_M || wParam == HOTKEY_CTRL_ALT_M || wParam == HOTKEY_CTRL_ALT_P) {
                SetPositioningMode(!isPositioningMode_);
                return 0;
            } else if (wParam == HOTKEY_CTRL_SHIFT_H || wParam == HOTKEY_CTRL_ALT_H) {
                ToggleVisibility();
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
            if (isPositioningMode_) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                OnMoved();
                return 0;
            }
            break;

        case WM_EXITSIZEMOVE:
            OnMoved();
            return 0;

        case WM_SETCURSOR:
            if (isPositioningMode_) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                SetPositioningMode(!isPositioningMode_);
                return 0;
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu();
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT_MOVE_MODE) {
                SetPositioningMode(false);
                return 0;
            } else if (LOWORD(wParam) == ID_TRAY_TOGGLE_VISIBILITY) {
                ToggleVisibility();
                return 0;
            } else if (LOWORD(wParam) == ID_TRAY_RELOAD_CONFIG) {
                PostMessageW(hwnd, WM_CONFIG_RELOADED, 0, 0);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TELEMETRY_UPDATED:
            if (onTelemetryReady_) {
                onTelemetryReady_();
            }
            return 0;

        case WM_CONFIG_RELOADED: {
            std::string cfgPath = ConfigManager::GetDefaultConfigPath();
            AppConfig newConfig = ConfigManager::Load(cfgPath);
            ApplyConfig(newConfig);
            if (onConfigReload_) {
                onConfigReload_();
            }
            return 0;
        }

        case WM_TRIGGER_LAUNCH_ANIM:
            TriggerLaunchAnimation(lastMetrics_);
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_ANIM_ID) {
                animTicksRemaining_--;
                if (animTicksRemaining_ <= 0) {
                    KillTimer(hwnd_, TIMER_ANIM_ID);
                    animTicksRemaining_ = 0;
                    isAnimatingRollUp_ = false;
                    lastRenderedText_.clear();
                    if (config_.layout == OverlayLayout::CustomGrid) {
                        RenderCustomGrid(animTargetMetrics_);
                    } else {
                        lastRows_ = Formatter::FormatRows(animTargetMetrics_, &config_);
                        RenderOverlay(lastRows_);
                    }

                    // Startup Working Set Trimming: Release initialization pages
                    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                    return 0;
                }

                float t = 1.0f - (static_cast<float>(animTicksRemaining_) / 20.0f);
                // Ease-Out Quadratic: f(t) = 1 - (1 - t)^2
                float progress = 1.0f - (1.0f - t) * (1.0f - t);

                SystemMetrics cur = animTargetMetrics_;
                cur.cpu.utilizationPercent = animTargetMetrics_.cpu.utilizationPercent * progress;
                if (animTargetMetrics_.cpu.temperatureC.has_value()) {
                    cur.cpu.temperatureC = *animTargetMetrics_.cpu.temperatureC * progress;
                }
                if (animTargetMetrics_.cpu.powerWatts.has_value()) {
                    cur.cpu.powerWatts = *animTargetMetrics_.cpu.powerWatts * progress;
                }
                if (animTargetMetrics_.cpu.fanRpm.has_value()) {
                    cur.cpu.fanRpm = static_cast<uint32_t>(*animTargetMetrics_.cpu.fanRpm * progress);
                }

                if (animTargetMetrics_.gpu.utilizationPercent.has_value()) {
                    cur.gpu.utilizationPercent = *animTargetMetrics_.gpu.utilizationPercent * progress;
                }
                cur.gpu.memoryUsedBytes = static_cast<uint64_t>(animTargetMetrics_.gpu.memoryUsedBytes * progress);
                cur.gpu.memoryTotalBytes = animTargetMetrics_.gpu.memoryTotalBytes; // Keep max static!
                if (animTargetMetrics_.gpu.temperatureC.has_value()) {
                    cur.gpu.temperatureC = static_cast<uint32_t>(*animTargetMetrics_.gpu.temperatureC * progress);
                }
                if (animTargetMetrics_.gpu.powerWatts.has_value()) {
                    cur.gpu.powerWatts = static_cast<uint32_t>(*animTargetMetrics_.gpu.powerWatts * progress);
                }
                if (animTargetMetrics_.gpu.fanRpm.has_value()) {
                    cur.gpu.fanRpm = static_cast<uint32_t>(*animTargetMetrics_.gpu.fanRpm * progress);
                } else if (animTargetMetrics_.gpu.fanSpeedPercent.has_value()) {
                    cur.gpu.fanSpeedPercent = static_cast<uint32_t>(*animTargetMetrics_.gpu.fanSpeedPercent * progress);
                }

                for (size_t gi = 0; gi < cur.gpus.size(); ++gi) {
                    const auto& targetGpu = animTargetMetrics_.gpus[gi];
                    auto& g = cur.gpus[gi];

                    if (targetGpu.utilizationPercent.has_value()) {
                        g.utilizationPercent = *targetGpu.utilizationPercent * progress;
                    }
                    g.memoryUsedBytes = static_cast<uint64_t>(targetGpu.memoryUsedBytes * progress);
                    g.memoryTotalBytes = targetGpu.memoryTotalBytes; // Keep max static!
                    if (targetGpu.temperatureC.has_value()) {
                        g.temperatureC = static_cast<uint32_t>(*targetGpu.temperatureC * progress);
                    }
                    if (targetGpu.powerWatts.has_value()) {
                        g.powerWatts = static_cast<uint32_t>(*targetGpu.powerWatts * progress);
                    }
                    if (targetGpu.fanRpm.has_value()) {
                        g.fanRpm = static_cast<uint32_t>(*targetGpu.fanRpm * progress);
                    } else if (targetGpu.fanSpeedPercent.has_value()) {
                        g.fanSpeedPercent = static_cast<uint32_t>(*targetGpu.fanSpeedPercent * progress);
                    }
                }

                cur.memory.usedBytes = static_cast<uint64_t>(animTargetMetrics_.memory.usedBytes * progress);
                cur.memory.totalBytes = animTargetMetrics_.memory.totalBytes; // Keep max static!

                cur.network.rxBytesPerSec = static_cast<uint64_t>(animTargetMetrics_.network.rxBytesPerSec * progress);
                cur.network.txBytesPerSec = static_cast<uint64_t>(animTargetMetrics_.network.txBytesPerSec * progress);

                if (config_.layout == OverlayLayout::CustomGrid) {
                    RenderCustomGrid(cur);
                } else {
                    lastRows_ = Formatter::FormatRows(cur, &config_);
                    RenderOverlay(lastRows_);
                }
                return 0;
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE) {
                auto* pSetting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                if (pSetting && IsEqualGUID(pSetting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)) {
                    DWORD state = *reinterpret_cast<const DWORD*>(pSetting->Data);
                    // 0 = Off, 1 = On, 2 = Dimmed
                    bool screenActive = (state != 0);
                    if (onDisplayStateChanged_) {
                        onDisplayStateChanged_(screenActive);
                    }
                }
            } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                // System waking from sleep / standby: reconnect hardware APIs and re-assert topmost
                if (onSystemResume_) {
                    onSystemResume_();
                }
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ForceRepaint();
            }
            return TRUE;

        case WM_QUERYENDSESSION:
            // Agree to OS shutdown immediately (never block shutdown/restart)
            return TRUE;

        case WM_ENDSESSION:
            if (wParam == TRUE) {
                // Session is ending: cleanly persist state and shut down hardware collectors
                if (onEndSession_) {
                    onEndSession_();
                }
                ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);
                RemoveTrayIcon();
            }
            return 0;

        case WM_DPICHANGED: {
            currentDpi_ = LOWORD(wParam);
            auto* lprc = reinterpret_cast<RECT*>(lParam);
            if (lprc) {
                windowX_ = lprc->left;
                windowY_ = lprc->top;
            }
            lastRenderedText_.clear();
            if (lastRows_.empty()) {
                lastRows_ = Formatter::FormatRows(lastMetrics_, &config_);
            }
            if (!lastRows_.empty()) {
                RenderOverlay(lastRows_);
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            RecalculateWindowPosition(windowWidth_, windowHeight_);
            SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            if (lastRows_.empty()) {
                lastRows_ = Formatter::FormatRows(lastMetrics_, &config_);
            }
            if (!lastRows_.empty()) {
                RenderOverlay(lastRows_);
            }
            return 0;
        }

        case WM_WINDOWPOSCHANGING: {
            auto* pwp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (!isPositioningMode_) {
                pwp->hwndInsertAfter = HWND_TOPMOST;
            }
            pwp->flags |= SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool OverlayWindow::Initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr) != Status::Ok) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"BareStatsOverlayClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    WNDCLASSEXW wci{};
    wci.cbSize = sizeof(WNDCLASSEXW);
    wci.lpfnWndProc = IslandWndProc;
    wci.hInstance = hInstance;
    wci.lpszClassName = L"BareStatsIslandClass";
    wci.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wci);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    RecalculateWindowPosition(windowWidth_, windowHeight_);

    hwnd_ = CreateWindowExW(exStyle, L"BareStatsOverlayClass", L"BareStats Overlay", style,
                            windowX_, windowY_, windowWidth_, windowHeight_,
                            nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    currentDpi_ = FetchDpiForWindow(hwnd_);

    s_pOverlayInstance = this;

    // Track foreground window changes and shell popup dismiss events to immediately re-assert HWND_TOPMOST
    s_hWinEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_CAPTUREEND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Register hotkeys with fallbacks
    // Primary Reposition: Ctrl + Shift + M (most reliable across Windows setups)
    RegisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_M, MOD_CONTROL | MOD_SHIFT, 'M');
    // Secondary Reposition: Ctrl + Alt + M
    RegisterHotKey(hwnd_, HOTKEY_CTRL_ALT_M, MOD_CONTROL | MOD_ALT, 'M');
    // Tertiary Reposition: Ctrl + Alt + P
    RegisterHotKey(hwnd_, HOTKEY_CTRL_ALT_P, MOD_CONTROL | MOD_ALT, 'P');

    // Primary Visibility Toggle: Ctrl + Shift + H
    RegisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_H, MOD_CONTROL | MOD_SHIFT, 'H');
    // Secondary Visibility Toggle: Ctrl + Alt + H
    RegisterHotKey(hwnd_, HOTKEY_CTRL_ALT_H, MOD_CONTROL | MOD_ALT, 'H');

    SetupTrayIcon();
    s_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    hPowerNotify_ = RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    return true;
}

void OverlayWindow::SetupTrayIcon() {
    memset(&nid_, 0, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    int smX = GetSystemMetrics(SM_CXSMICON);
    int smY = GetSystemMetrics(SM_CYSMICON);
    if (smX <= 0) smX = 16;
    if (smY <= 0) smY = 16;
    nid_.hIcon = reinterpret_cast<HICON>(LoadImageW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, smX, smY, LR_DEFAULTCOLOR));
    if (!nid_.hIcon) {
        nid_.hIcon = LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    }
    wcscpy_s(nid_.szTip, L"BareStats - Click to Reposition | Right-click for options");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    trayIconActive_ = true;
}

void OverlayWindow::RemoveTrayIcon() {
    if (trayIconActive_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayIconActive_ = false;
    }
}

void OverlayWindow::ShowTrayContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    UINT moveFlags = MF_STRING | (isPositioningMode_ ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, moveFlags, ID_TRAY_TOGGLE_MOVE, L"Reposition Mode (Ctrl+Shift+M or Ctrl+Alt+M)");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RESET_POS, L"Reset to Bottom-Center");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    std::wstring visLabel = isVisible_ ? L"Hide Overlay (Ctrl+Shift+H)" : L"Show Overlay (Ctrl+Shift+H)";
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_VISIBILITY, visLabel.c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RELOAD_CONFIG, L"Reload Configuration");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT autoStartFlags = MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, L"Start with Windows");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit BareStats");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == ID_TRAY_TOGGLE_MOVE) {
        SetPositioningMode(!isPositioningMode_);
    } else if (cmd == ID_TRAY_RESET_POS) {
        ResetToPreset(PositionPreset::BottomCenter);
    } else if (cmd == ID_TRAY_TOGGLE_VISIBILITY) {
        ToggleVisibility();
    } else if (cmd == ID_TRAY_RELOAD_CONFIG) {
        PostMessageW(hwnd_, WM_CONFIG_RELOADED, 0, 0);
    } else if (cmd == ID_TRAY_AUTOSTART) {
        SetAutoStart(!IsAutoStartEnabled());
    } else if (cmd == ID_TRAY_EXIT) {
        DestroyWindow(hwnd_);
    }
}

void OverlayWindow::ForceRepaint() {
    lastRenderedText_.clear();
    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(lastMetrics_);
        return;
    }
    if (lastRows_.empty()) {
        lastRows_ = Formatter::FormatRows(lastMetrics_, &config_);
    }
    if (!lastRows_.empty()) {
        RenderOverlay(lastRows_);
    }
}

void OverlayWindow::ReassertTopmost() {
    if (isPositioningMode_) return;
    if (config_.layout == OverlayLayout::CustomGrid && config_.gridMode == GridMode::Islands) {
        for (const auto& slot : islandSlots_) {
            if (slot.hwnd && IsWindow(slot.hwnd)) {
                HWND prev = GetWindow(slot.hwnd, GW_HWNDPREV);
                if (prev != nullptr) {
                    SetWindowPos(slot.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
                }
            }
        }
    } else {
        if (hwnd_ && IsWindow(hwnd_)) {
            HWND prev = GetWindow(hwnd_, GW_HWNDPREV);
            if (prev != nullptr) {
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
            }
        }
    }
}

bool OverlayWindow::IsAutoStartEnabled() const {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        LSTATUS status = RegQueryValueExW(hKey, L"BareStats", nullptr, &type, nullptr, &size);
        RegCloseKey(hKey);
        return (status == ERROR_SUCCESS && type == REG_SZ);
    }
    return false;
}

bool OverlayWindow::SetAutoStart(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enable) {
        wchar_t exePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            std::wstring quotedPath = L"\"" + std::wstring(exePath) + L"\"";
            DWORD bytes = static_cast<DWORD>((quotedPath.length() + 1) * sizeof(wchar_t));
            success = (RegSetValueExW(hKey, L"BareStats", 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(quotedPath.c_str()), bytes) == ERROR_SUCCESS);
        }
    } else {
        success = (RegDeleteValueW(hKey, L"BareStats") == ERROR_SUCCESS);
    }

    RegCloseKey(hKey);
    return success;
}

void OverlayWindow::Show() {
    isVisible_ = true;
    if (hwnd_) {
        if (config_.layout != OverlayLayout::CustomGrid || config_.gridMode != GridMode::Islands || isPositioningMode_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
    }
    for (auto& slot : islandSlots_) {
        if (slot.hwnd) {
            ShowWindow(slot.hwnd, SW_SHOWNOACTIVATE);
        }
    }
}

void OverlayWindow::Hide() {
    isVisible_ = false;
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    for (auto& slot : islandSlots_) {
        if (slot.hwnd) {
            ShowWindow(slot.hwnd, SW_HIDE);
        }
    }
}

void OverlayWindow::ToggleVisibility() {
    if (isVisible_) {
        Hide();
    } else {
        Show();
        ForceRepaint();
    }
}

void OverlayWindow::ApplyConfig(const AppConfig& newConfig) {
    config_ = newConfig;
    if (config_.opacity < 10) config_.opacity = 10;
    if (config_.opacity > 100) config_.opacity = 100;

    DestroyIslandWindows();
    lastRenderedText_.clear();
    RecalculateWindowPosition(windowWidth_, windowHeight_);
    SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (config_.layout != OverlayLayout::CustomGrid || config_.gridMode != GridMode::Islands || isPositioningMode_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(lastMetrics_);
    } else {
        lastRows_ = Formatter::FormatRows(lastMetrics_, &config_);
        if (!lastRows_.empty()) {
            RenderOverlay(lastRows_);
        }
    }
}

void OverlayWindow::SetPositioningMode(bool enable) {
    if (isPositioningMode_ == enable) return;
    isPositioningMode_ = enable;

    RECT rc{};
    if (hwnd_) GetWindowRect(hwnd_, &rc);

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        exStyle &= ~WS_EX_TRANSPARENT;
        windowX_ = rc.left;
        windowY_ = rc.top - headerHeight_;
        s_pOverlayInstance = this;
        if (!s_hMouseHook) {
            s_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(nullptr), 0);
        }
    } else {
        if (s_hMouseHook) {
            UnhookWindowsHookEx(s_hMouseHook);
            s_hMouseHook = nullptr;
        }
        s_pOverlayInstance = nullptr;

        exStyle |= WS_EX_TRANSPARENT;
        windowX_ = rc.left;
        windowY_ = rc.top + headerHeight_;
        config_.preset = PositionPreset::Custom;
        config_.customX = windowX_;
        config_.customY = windowY_;
        ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);

        // Working Set Trimming: Release repositioning pages
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    lastRenderedText_.clear();
    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(lastMetrics_);
    } else if (!lastRows_.empty()) {
        RenderOverlay(lastRows_);
    }
}

void OverlayWindow::TriggerLaunchAnimation(const SystemMetrics& targetMetrics) {
    animTargetMetrics_ = targetMetrics;
    lastMetrics_ = targetMetrics;
    animTicksRemaining_ = 20; // 20 frames * 40ms = 800ms
    isAnimatingRollUp_ = true;
    if (hwnd_) {
        SetTimer(hwnd_, TIMER_ANIM_ID, 40, nullptr);
    }

    // Render initial 0-state frame immediately
    SystemMetrics zeroMetrics = animTargetMetrics_;
    zeroMetrics.cpu.utilizationPercent = 0.0;
    if (zeroMetrics.cpu.temperatureC.has_value()) zeroMetrics.cpu.temperatureC = 0.0;
    if (zeroMetrics.cpu.powerWatts.has_value()) zeroMetrics.cpu.powerWatts = 0.0;
    if (zeroMetrics.cpu.fanRpm.has_value()) zeroMetrics.cpu.fanRpm = 0;

    if (zeroMetrics.gpu.utilizationPercent.has_value()) zeroMetrics.gpu.utilizationPercent = 0.0;
    zeroMetrics.gpu.memoryUsedBytes = 0;
    // Keep max static! (zeroMetrics.gpu.memoryTotalBytes untouched)
    if (zeroMetrics.gpu.temperatureC.has_value()) zeroMetrics.gpu.temperatureC = 0;
    if (zeroMetrics.gpu.powerWatts.has_value()) zeroMetrics.gpu.powerWatts = 0;
    if (zeroMetrics.gpu.fanRpm.has_value()) zeroMetrics.gpu.fanRpm = 0;
    if (zeroMetrics.gpu.fanSpeedPercent.has_value()) zeroMetrics.gpu.fanSpeedPercent = 0;

    for (auto& g : zeroMetrics.gpus) {
        if (g.utilizationPercent.has_value()) g.utilizationPercent = 0.0;
        g.memoryUsedBytes = 0;
        if (g.temperatureC.has_value()) g.temperatureC = 0;
        if (g.powerWatts.has_value()) g.powerWatts = 0;
        if (g.fanRpm.has_value()) g.fanRpm = 0;
        if (g.fanSpeedPercent.has_value()) g.fanSpeedPercent = 0;
    }

    zeroMetrics.memory.usedBytes = 0;
    // Keep max static! (zeroMetrics.memory.totalBytes untouched)

    zeroMetrics.network.rxBytesPerSec = 0;
    zeroMetrics.network.txBytesPerSec = 0;

    lastRenderedText_.clear();
    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(zeroMetrics);
    } else {
        lastRows_ = Formatter::FormatRows(zeroMetrics, &config_);
        RenderOverlay(lastRows_);
    }
}

void OverlayWindow::OnMoved() {
    if (!hwnd_) return;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    windowX_ = rc.left;

    int actualY = isPositioningMode_ ? rc.top + headerHeight_ : rc.top;
    windowY_ = isPositioningMode_ ? rc.top : rc.top;

    config_.preset = PositionPreset::Custom;
    config_.customX = rc.left;
    config_.customY = actualY;

    HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(hMon, &mi)) {
        config_.customRelX = rc.left - mi.rcMonitor.left;
        config_.customRelY = actualY - mi.rcMonitor.top;
    }

    auto allMonitors = MonitorHelper::EnumerateMonitors();
    for (size_t i = 0; i < allMonitors.size(); ++i) {
        if (allMonitors[i].handle == hMon) {
            config_.monitorIndex = static_cast<int>(i);
            break;
        }
    }

    UpdateIslandPositions();
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);
}

void OverlayWindow::ResetToPreset(PositionPreset preset) {
    config_.preset = preset;
    config_.customX = -1;
    config_.customY = -1;
    config_.customRelX = -1;
    config_.customRelY = -1;
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);

    if (config_.layout != OverlayLayout::CustomGrid || config_.gridMode != GridMode::Islands || isPositioningMode_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }

    lastRenderedText_.clear();
    RecalculateWindowPosition(windowWidth_, windowHeight_);
    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(lastMetrics_);
    } else {
        if (lastRows_.empty()) {
            lastRows_ = Formatter::FormatRows(lastMetrics_, &config_);
        }
        if (!lastRows_.empty()) {
            RenderOverlay(lastRows_);
        }
    }
}

void OverlayWindow::RecalculateWindowPosition(int contentWidth, int contentHeight) {
    auto monitor = MonitorHelper::GetMonitor(config_.monitorIndex);

    if (config_.preset == PositionPreset::Custom) {
        if (config_.customRelX >= 0 && config_.customRelY >= 0) {
            windowX_ = monitor.rcMonitor.left + config_.customRelX;
            windowY_ = monitor.rcMonitor.top + config_.customRelY;
        } else if (config_.customX >= 0 && config_.customY >= 0) {
            windowX_ = config_.customX;
            windowY_ = config_.customY;
        } else {
            POINT pt = MonitorHelper::CalculatePosition(monitor, PositionPreset::BottomCenter, contentWidth, contentHeight, config_.margin, config_.margin);
            windowX_ = pt.x;
            windowY_ = pt.y;
        }

        // Clamp inside physical display bounds (rcMonitor) so it CAN sit cleanly on the taskbar without jumping up, while still guarding edges against disappearing off-screen!
        int maxX = monitor.rcMonitor.right - contentWidth;
        int maxY = monitor.rcMonitor.bottom - contentHeight;
        if (maxX < monitor.rcMonitor.left) maxX = monitor.rcMonitor.left;
        if (maxY < monitor.rcMonitor.top) maxY = monitor.rcMonitor.top;

        if (windowX_ < monitor.rcMonitor.left) windowX_ = monitor.rcMonitor.left;
        if (windowX_ > maxX) windowX_ = maxX;
        if (windowY_ < monitor.rcMonitor.top) windowY_ = monitor.rcMonitor.top;
        if (windowY_ > maxY) windowY_ = maxY;
    } else {
        POINT pt = MonitorHelper::CalculatePosition(monitor, config_.preset, contentWidth, contentHeight, config_.margin, config_.margin);
        windowX_ = pt.x;
        windowY_ = pt.y;
    }
}

void OverlayWindow::UpdateMetrics(const SystemMetrics& metrics) {
    lastMetrics_ = metrics;
    if (isAnimatingRollUp_) return;
    if (!isVisible_) return;

    if (config_.layout == OverlayLayout::CustomGrid) {
        RenderCustomGrid(metrics);
        return;
    }

    auto rows = Formatter::FormatRows(metrics, &config_);

    // 15-Nanosecond Dirty Frame Check across all rows:
    std::wstring fullText;
    fullText.reserve(256);
    for (const auto& row : rows) {
        for (const auto& seg : row) {
            fullText += seg.text;
        }
        fullText += L"\n";
    }

    if (!isPositioningMode_ && fullText == lastRenderedText_) {
        HWND prevWnd = GetWindow(hwnd_, GW_HWNDPREV);
        if (prevWnd != nullptr) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
        }
        return; // Zero work! No DIB section, no GDI+, no UpdateLayeredWindow
    }

    lastRenderedText_ = std::move(fullText);
    lastRows_ = std::move(rows);
    RenderOverlay(lastRows_);
}

static void DrawVectorMoveIcon(Graphics& graphics, float cx, float cy, float radius, Color color, float penWidth = 1.5f) {
    Pen pen(color, penWidth);
    SolidBrush brush(color);

    // Cross arms
    graphics.DrawLine(&pen, cx - radius, cy, cx + radius, cy);
    graphics.DrawLine(&pen, cx, cy - radius, cx, cy + radius);

    // Arrow heads
    float arrow = radius * 0.45f;
    // Left
    PointF ptLeft[] = { PointF(cx - radius, cy), PointF(cx - radius + arrow, cy - arrow), PointF(cx - radius + arrow, cy + arrow) };
    graphics.FillPolygon(&brush, ptLeft, 3);
    // Right
    PointF ptRight[] = { PointF(cx + radius, cy), PointF(cx + radius - arrow, cy - arrow), PointF(cx + radius - arrow, cy + arrow) };
    graphics.FillPolygon(&brush, ptRight, 3);
    // Up
    PointF ptUp[] = { PointF(cx, cy - radius), PointF(cx - arrow, cy - radius + arrow), PointF(cx + arrow, cy - radius + arrow) };
    graphics.FillPolygon(&brush, ptUp, 3);
    // Down
    PointF ptDown[] = { PointF(cx, cy + radius), PointF(cx - arrow, cy + radius - arrow), PointF(cx + arrow, cy + radius - arrow) };
    graphics.FillPolygon(&brush, ptDown, 3);
}

static std::wstring ResolveFontFamily(const std::wstring& requestedFont) {
    if (requestedFont.empty() || _wcsicmp(requestedFont.c_str(), L"auto") == 0 || _wcsicmp(requestedFont.c_str(), L"system") == 0) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(NONCLIENTMETRICSW);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0)) {
            if (wcslen(ncm.lfMessageFont.lfFaceName) > 0) {
                return ncm.lfMessageFont.lfFaceName;
            }
        }
        return L"Segoe UI";
    }
    return requestedFont;
}

static std::unique_ptr<Font> CreateOverlayFont(const std::wstring& requestedFace,
                                               const std::wstring& requestedWeight,
                                               int fontSize,
                                               float dpiScale) {
    REAL baseSize = static_cast<REAL>(fontSize > 0 ? fontSize : 13);
    REAL emSize = static_cast<REAL>(std::round(baseSize * dpiScale));
    std::wstring effectiveFace = ResolveFontFamily(requestedFace);

    bool isBold = (_wcsicmp(requestedWeight.c_str(), L"bold") == 0);
    bool isRegular = (_wcsicmp(requestedWeight.c_str(), L"regular") == 0);

    // If semibold (default, matches Windows Taskbar & NVIDIA Overlay)
    if (!isBold && !isRegular) {
        if (_wcsicmp(effectiveFace.c_str(), L"Segoe UI") == 0 ||
            _wcsicmp(effectiveFace.c_str(), L"Segoe UI Semibold") == 0) {
            auto font = std::make_unique<Font>(L"Segoe UI Semibold", emSize, FontStyleRegular, UnitPixel);
            if (font->GetLastStatus() == Status::Ok) {
                return font;
            }
        }

        std::wstring semiFace = effectiveFace + L" Semibold";
        auto font = std::make_unique<Font>(semiFace.c_str(), emSize, FontStyleRegular, UnitPixel);
        if (font->GetLastStatus() == Status::Ok) {
            return font;
        }

        auto fontBold = std::make_unique<Font>(effectiveFace.c_str(), emSize, FontStyleBold, UnitPixel);
        if (fontBold->GetLastStatus() == Status::Ok) {
            return fontBold;
        }
    }

    if (isBold) {
        auto font = std::make_unique<Font>(effectiveFace.c_str(), emSize, FontStyleBold, UnitPixel);
        if (font->GetLastStatus() == Status::Ok) {
            return font;
        }
        return std::make_unique<Font>(L"Segoe UI", emSize, FontStyleBold, UnitPixel);
    }

    // Regular
    auto font = std::make_unique<Font>(effectiveFace.c_str(), emSize, FontStyleRegular, UnitPixel);
    if (font->GetLastStatus() == Status::Ok) {
        return font;
    }
    return std::make_unique<Font>(L"Segoe UI", emSize, FontStyleRegular, UnitPixel);
}

void OverlayWindow::RenderOverlay(const std::vector<FormattedSegment>& segments) {
    if (segments.empty()) return;
    RenderOverlay(std::vector<std::vector<FormattedSegment>>{ segments });
}

void OverlayWindow::RenderOverlay(const std::vector<std::vector<FormattedSegment>>& rows) {
    if (!hwnd_ || rows.empty()) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    if (!memDC) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    std::wstring headerText = L"MOVE MODE: Drag to reposition \x2022 Click outside or Ctrl+Shift+M to lock";

    float dpiScale = (currentDpi_ > 0 ? currentDpi_ : 96) / 96.0f;

    // Uniform font matching NVIDIA Overlay (scaled to monitor DPI)
    auto overlayFont = CreateOverlayFont(config_.fontFace, config_.fontWeight, config_.fontSize, dpiScale);

    float headerFontSize = std::round(11.0f * dpiScale);
    auto headerFont = std::make_unique<Font>(L"Segoe UI", headerFontSize, FontStyleBold, UnitPixel);
    if (headerFont->GetLastStatus() != Status::Ok) {
        headerFont = std::make_unique<Font>(L"Consolas", headerFontSize, FontStyleBold, UnitPixel);
    }

    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    RectF layoutRect(0, 0, 4000.0f * dpiScale, 300.0f * dpiScale);
    RectF headerBounds{};
    REAL maxRowWidth = 0.0f;
    REAL maxLineHeight = 0.0f;

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
        for (const auto& row : rows) {
            REAL rowWidth = 0.0f;
            for (const auto& seg : row) {
                if (seg.text.empty()) continue;
                RectF segBounds{};
                measureGraphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);
                rowWidth += segBounds.Width;
                if (segBounds.Height > maxLineHeight) {
                    maxLineHeight = segBounds.Height;
                }
            }
            if (rowWidth > maxRowWidth) {
                maxRowWidth = rowWidth;
            }
        }
        if (isPositioningMode_) {
            measureGraphics.MeasureString(headerText.c_str(), -1, headerFont.get(), layoutRect, &headerBounds);
        }
    }

    int statsWidth = static_cast<int>(std::ceil(maxRowWidth));
    int singleLineHeight = static_cast<int>(std::ceil(maxLineHeight));
    int rowSpacing = static_cast<int>(std::round(2.0f * dpiScale));
    int numRows = static_cast<int>(rows.size());
    int totalStatsHeight = numRows * singleLineHeight + (numRows > 1 ? (numRows - 1) * rowSpacing : 0);

    int padX = static_cast<int>(std::round(14.0f * dpiScale));
    int padY = static_cast<int>(std::round(10.0f * dpiScale));

    int contentWidth = 0;
    int contentHeight = 0;

    if (isPositioningMode_) {
        int headerTextWidth = static_cast<int>(std::ceil(headerBounds.Width));
        headerHeight_ = static_cast<int>(std::round(24.0f * dpiScale));
        int headerExtra = static_cast<int>(std::round(36.0f * dpiScale));
        contentWidth = (std::max)(statsWidth + padX, headerTextWidth + headerExtra);
        contentHeight = headerHeight_ + totalStatsHeight + padY;
    } else {
        contentWidth = statsWidth + padX;
        contentHeight = totalStatsHeight + padY;
    }

    windowWidth_ = contentWidth;
    windowHeight_ = contentHeight;

    if (!isPositioningMode_) {
        RecalculateWindowPosition(windowWidth_, windowHeight_);
    } else {
        RECT curRect{};
        if (GetWindowRect(hwnd_, &curRect) && (curRect.right - curRect.left > 0)) {
            windowX_ = curRect.left;
            windowY_ = curRect.top;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = contentWidth;
    bmi.bmiHeader.biHeight = -contentHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    if (!hBmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    HBITMAP hOldBmp = reinterpret_cast<HBITMAP>(SelectObject(memDC, hBmp));
    memset(pvBits, 0, contentWidth * contentHeight * 4);

    {
        Graphics graphics(memDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

        if (isPositioningMode_) {
            // 1. Box background plate behind the whole overlay
            SolidBrush bgBrush(Color(235, 18, 24, 34)); // dark plate
            graphics.FillRectangle(&bgBrush, 0, 0, contentWidth, contentHeight);

            // 2. Title bar background (top line)
            SolidBrush titleBgBrush(Color(245, 26, 36, 50));
            graphics.FillRectangle(&titleBgBrush, 1, 1, contentWidth - 2, headerHeight_ - 1);

            // 3. Divider line between top header and stats
            Pen divPen(Color(180, 0, 200, 240), 1.0f);
            graphics.DrawLine(&divPen, 1, headerHeight_, contentWidth - 2, headerHeight_);

            // 4. Outer bounding box border
            Pen borderPen(Color(255, 0, 220, 255), 1.5f * dpiScale); // vibrant cyan
            graphics.DrawRectangle(&borderPen, 1, 1, contentWidth - 3, contentHeight - 3);

            // 5. Draw vector 4-way move icon in top line
            float iconCy = headerHeight_ / 2.0f;
            float iconCx = 14.0f * dpiScale;
            float iconRadius = 5.0f * dpiScale;
            DrawVectorMoveIcon(graphics, iconCx, iconCy, iconRadius, Color(255, 0, 230, 255), 1.5f * dpiScale);

            // 6. Draw header text in top line
            SolidBrush headerTextBrush(Color(255, 255, 215, 0)); // bright gold
            float headerTx = 24.0f * dpiScale;
            float headerTy = (headerHeight_ - headerBounds.Height) / 2.0f;
            graphics.DrawString(headerText.c_str(), -1, headerFont.get(), PointF(headerTx, headerTy), &headerTextBrush);
        }

        // Stats text position:
        REAL statsX = std::round(6.0f * dpiScale);
        REAL statsBaseY = isPositioningMode_ ? static_cast<REAL>(headerHeight_ + std::round(5.0f * dpiScale)) : std::round(5.0f * dpiScale);
        float shadowOffset = (std::max)(1.0f, std::round(1.0f * dpiScale));

        // Base text brush (bright crisp white for numbers and units)
        BYTE rBase = GetRValue(config_.textColor);
        BYTE gBase = GetGValue(config_.textColor);
        BYTE bBase = GetBValue(config_.textColor);
        SolidBrush textBrush(Color(255, rBase, gBase, bBase));

        // Label brush (soft muted silver #A0A0A0 for category titles CPU, GPU, RAM)
        BYTE rLabel = GetRValue(config_.labelColor);
        BYTE gLabel = GetGValue(config_.labelColor);
        BYTE bLabel = GetBValue(config_.labelColor);
        SolidBrush labelBrush(Color(255, rLabel, gLabel, bLabel));

        // Subtler separator brush (quiet subtle divider #6E6E6E)
        BYTE rSep = GetRValue(config_.separatorColor);
        BYTE gSep = GetGValue(config_.separatorColor);
        BYTE bSep = GetBValue(config_.separatorColor);
        SolidBrush sepBrush(Color(255, rSep, gSep, bSep));

        // High text brush (Yellow)
        BYTE rHigh = GetRValue(config_.colorHigh);
        BYTE gHigh = GetGValue(config_.colorHigh);
        BYTE bHigh = GetBValue(config_.colorHigh);
        SolidBrush highBrush(Color(255, rHigh, gHigh, bHigh));

        // Critical text brush (Red)
        BYTE rCrit = GetRValue(config_.colorCritical);
        BYTE gCrit = GetGValue(config_.colorCritical);
        BYTE bCrit = GetBValue(config_.colorCritical);
        SolidBrush critBrush(Color(255, rCrit, gCrit, bCrit));

        SolidBrush shadowBrush(Color(180, 0, 0, 0));

        // Render each row
        for (int r = 0; r < numRows; ++r) {
            REAL rowY = statsBaseY + r * (singleLineHeight + rowSpacing);
            REAL curX = statsX;
            for (const auto& seg : rows[r]) {
                if (seg.text.empty()) continue;

                RectF segBounds{};
                graphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);

                // Draw smooth single-offset shadow for contrast
                if (config_.dropShadow) {
                    graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX + shadowOffset, rowY + shadowOffset), &format, &shadowBrush);
                }

                // Pick color brush by segment type / severity
                Brush* pBrush = &textBrush;
                if (seg.type == SegmentType::Separator || seg.isSeparator) {
                    pBrush = &sepBrush;
                } else if (seg.type == SegmentType::Label) {
                    pBrush = &labelBrush;
                } else if (seg.severity == MetricSeverity::Critical) {
                    pBrush = &critBrush;
                } else if (seg.severity == MetricSeverity::High) {
                    pBrush = &highBrush;
                }

                graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX, rowY), &format, pBrush);

                curX += segBounds.Width;
            }
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE wndSize = { contentWidth, contentHeight };
    POINT ptDst = { windowX_, windowY_ };

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    BYTE alpha = static_cast<BYTE>(std::clamp((config_.opacity * 255) / 100, 25, 255));
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, hdcScreen, &ptDst, &wndSize, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    if (!isPositioningMode_) {
        HWND prevWnd = GetWindow(hwnd_, GW_HWNDPREV);
        if (prevWnd != nullptr) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
        }
    }

    SelectObject(memDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
}

void OverlayWindow::DestroyIslandWindows() {
    for (auto& slot : islandSlots_) {
        if (slot.hwnd) {
            DestroyWindow(slot.hwnd);
            slot.hwnd = nullptr;
        }
    }
    islandSlots_.clear();
}

void OverlayWindow::UpdateIslandPositions() {
    for (auto& slot : islandSlots_) {
        if (slot.hwnd && IsWindow(slot.hwnd)) {
            int curX = windowX_ + slot.relX;
            int curY = windowY_ + slot.relY;
            SetWindowPos(slot.hwnd, HWND_TOPMOST, curX, curY, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
        }
    }
}

static void ComputeGridColumns(const CustomGridDefinition& grid,
                               const std::vector<std::vector<REAL>>& cellWidths,
                               float dpiScale, REAL cellGapPx,
                               std::vector<REAL>& outColWidth,
                               std::vector<REAL>& outColX) {
    outColWidth.assign(grid.numCols, 0.0f);
    outColX.assign(grid.numCols, 0.0f);
    if (grid.numCols <= 0) return;

    // Pass 1: Single-cell width resolution (colSpan == 1)
    for (int r = 0; r < grid.numRows; ++r) {
        if (r >= static_cast<int>(cellWidths.size())) continue;
        for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
            if (ci >= cellWidths[r].size()) continue;
            const auto& cell = grid.rows[r].cells[ci];
            if (cell.colSpan == 1 && !cell.token.empty() && cell.colIndex < grid.numCols) {
                outColWidth[cell.colIndex] = (std::max)(outColWidth[cell.colIndex], cellWidths[r][ci]);
            }
        }
    }

    // Pass 2: Base column width resolution for empty / spacer columns
    REAL sumKnown = 0.0f;
    int countKnown = 0;
    for (int c = 0; c < grid.numCols; ++c) {
        if (outColWidth[c] > 0.0f) {
            sumKnown += outColWidth[c];
            countKnown++;
        }
    }

    REAL avgColWidth = 0.0f;
    if (countKnown > 0) {
        avgColWidth = sumKnown / static_cast<REAL>(countKnown);
    } else {
        REAL maxSpannedPerCol = 0.0f;
        for (int r = 0; r < grid.numRows; ++r) {
            if (r >= static_cast<int>(cellWidths.size())) continue;
            for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
                if (ci >= cellWidths[r].size()) continue;
                const auto& cell = grid.rows[r].cells[ci];
                if (!cell.token.empty() && cell.colSpan > 0) {
                    REAL perCol = cellWidths[r][ci] / static_cast<REAL>(cell.colSpan);
                    if (perCol > maxSpannedPerCol) maxSpannedPerCol = perCol;
                }
            }
        }
        avgColWidth = (maxSpannedPerCol > 0.0f) ? maxSpannedPerCol : std::round(90.0f * dpiScale);
    }

    for (int c = 0; c < grid.numCols; ++c) {
        if (outColWidth[c] <= 0.0f) {
            outColWidth[c] = avgColWidth;
        }
    }

    // Pass 3: Spanned-cell width distribution (colSpan > 1)
    for (int r = 0; r < grid.numRows; ++r) {
        if (r >= static_cast<int>(cellWidths.size())) continue;
        for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
            if (ci >= cellWidths[r].size()) continue;
            const auto& cell = grid.rows[r].cells[ci];
            if (cell.colSpan > 1 && !cell.token.empty() && cell.colIndex < grid.numCols) {
                int endCol = (std::min)(cell.colIndex + cell.colSpan, grid.numCols);
                int span = endCol - cell.colIndex;
                if (span <= 0) continue;

                REAL avail = 0.0f;
                for (int k = cell.colIndex; k < endCol; ++k) {
                    avail += outColWidth[k];
                }
                avail += (span - 1) * cellGapPx;

                if (cellWidths[r][ci] > avail) {
                    REAL extra = (cellWidths[r][ci] - avail) / static_cast<REAL>(span);
                    for (int k = cell.colIndex; k < endCol; ++k) {
                        outColWidth[k] += extra;
                    }
                }
            }
        }
    }

    // Pass 4: Column X offsets
    outColX[0] = 0.0f;
    for (int c = 1; c < grid.numCols; ++c) {
        outColX[c] = outColX[c - 1] + outColWidth[c - 1] + cellGapPx;
    }
}

void OverlayWindow::RenderCustomGrid(const SystemMetrics& metrics) {
    if (config_.gridMode == GridMode::Islands) {
        RenderCustomGridIslands(metrics);
    } else {
        if (!islandSlots_.empty()) {
            DestroyIslandWindows();
        }
        if (hwnd_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
        RenderCustomGridUnified(metrics);
    }
}

void OverlayWindow::RenderCustomGridUnified(const SystemMetrics& metrics) {
    if (!hwnd_) return;
    const auto& grid = config_.customGrid;
    if (grid.numCols <= 0 || grid.numRows <= 0) {
        lastRows_ = Formatter::FormatRows(metrics, &config_);
        RenderOverlay(lastRows_);
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    if (!memDC) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    float dpiScale = (currentDpi_ > 0 ? currentDpi_ : 96) / 96.0f;
    auto overlayFont = CreateOverlayFont(config_.fontFace, config_.fontWeight, config_.fontSize, dpiScale);

    float headerFontSize = std::round(11.0f * dpiScale);
    auto headerFont = std::make_unique<Font>(L"Segoe UI", headerFontSize, FontStyleBold, UnitPixel);
    if (headerFont->GetLastStatus() != Status::Ok) {
        headerFont = std::make_unique<Font>(L"Consolas", headerFontSize, FontStyleBold, UnitPixel);
    }

    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    std::vector<std::vector<std::vector<FormattedSegment>>> cellSegs(grid.numRows);
    std::vector<std::vector<REAL>> cellWidths(grid.numRows);
    REAL maxLineHeight = 0.0f;
    std::wstring dirtyCheckText;
    dirtyCheckText.reserve(512);

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

        for (int r = 0; r < grid.numRows; ++r) {
            cellSegs[r].resize(grid.rows[r].cells.size());
            cellWidths[r].resize(grid.rows[r].cells.size(), 0.0f);

            for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
                const auto& cell = grid.rows[r].cells[ci];
                if (cell.token.empty()) {
                    dirtyCheckText += L"[.]";
                    continue;
                }

                auto segs = Formatter::FormatCell(cell.token, metrics, &config_);
                REAL segW = 0.0f;
                for (const auto& s : segs) {
                    if (s.text.empty()) continue;
                    dirtyCheckText += s.text;
                    RectF sb{};
                    measureGraphics.MeasureString(s.text.c_str(), static_cast<int>(s.text.length()), overlayFont.get(), PointF(0, 0), &format, &sb);
                    segW += sb.Width;
                    if (sb.Height > maxLineHeight) {
                        maxLineHeight = sb.Height;
                    }
                }
                dirtyCheckText += L"|";
                cellWidths[r][ci] = segW;
                cellSegs[r][ci] = std::move(segs);
            }
            dirtyCheckText += L"\n";
        }
    }

    // 15-Nanosecond Dirty Frame Check
    if (!isPositioningMode_ && dirtyCheckText == lastRenderedText_) {
        HWND prevWnd = GetWindow(hwnd_, GW_HWNDPREV);
        if (prevWnd != nullptr) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
        }
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }
    lastRenderedText_ = std::move(dirtyCheckText);

    std::vector<REAL> colWidth(grid.numCols, 0.0f);
    std::vector<REAL> colX(grid.numCols, 0.0f);
    REAL cellGapPx = config_.cellGap * dpiScale;
    REAL rowGapPx = config_.rowGap * dpiScale;

    ComputeGridColumns(grid, cellWidths, dpiScale, cellGapPx, colWidth, colX);

    int singleLineHeight = static_cast<int>(std::ceil(maxLineHeight));
    if (singleLineHeight <= 0) {
        singleLineHeight = static_cast<int>(std::round(18.0f * dpiScale));
    }

    REAL totalStatsWidthReal = (grid.numCols > 0) ? (colX.back() + colWidth.back()) : 0.0f;
    int statsWidth = static_cast<int>(std::ceil(totalStatsWidthReal));
    int totalStatsHeight = grid.numRows * singleLineHeight + (grid.numRows > 1 ? static_cast<int>(std::round((grid.numRows - 1) * rowGapPx)) : 0);

    int padX = static_cast<int>(std::round(14.0f * dpiScale));
    int padY = static_cast<int>(std::round(10.0f * dpiScale));

    std::wstring headerText = L"MOVE MODE: Drag to reposition \x2022 Click outside or Ctrl+Shift+M to lock";
    RectF headerBounds{};
    int contentWidth = 0;
    int contentHeight = 0;

    if (isPositioningMode_) {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
        RectF layoutRect(0, 0, 4000.0f * dpiScale, 300.0f * dpiScale);
        measureGraphics.MeasureString(headerText.c_str(), -1, headerFont.get(), layoutRect, &headerBounds);

        int headerTextWidth = static_cast<int>(std::ceil(headerBounds.Width));
        headerHeight_ = static_cast<int>(std::round(24.0f * dpiScale));
        int headerExtra = static_cast<int>(std::round(36.0f * dpiScale));
        contentWidth = (std::max)(statsWidth + padX, headerTextWidth + headerExtra);
        contentHeight = headerHeight_ + totalStatsHeight + padY;
    } else {
        contentWidth = statsWidth + padX;
        contentHeight = totalStatsHeight + padY;
    }

    windowWidth_ = contentWidth;
    windowHeight_ = contentHeight;

    if (!isPositioningMode_) {
        RecalculateWindowPosition(windowWidth_, windowHeight_);
    } else {
        RECT curRect{};
        if (GetWindowRect(hwnd_, &curRect) && (curRect.right - curRect.left > 0)) {
            windowX_ = curRect.left;
            windowY_ = curRect.top;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = contentWidth;
    bmi.bmiHeader.biHeight = -contentHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    if (!hBmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    HBITMAP hOldBmp = reinterpret_cast<HBITMAP>(SelectObject(memDC, hBmp));
    memset(pvBits, 0, contentWidth * contentHeight * 4);

    {
        Graphics graphics(memDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

        if (isPositioningMode_) {
            SolidBrush bgBrush(Color(235, 18, 24, 34));
            graphics.FillRectangle(&bgBrush, 0, 0, contentWidth, contentHeight);

            SolidBrush titleBgBrush(Color(245, 26, 36, 50));
            graphics.FillRectangle(&titleBgBrush, 1, 1, contentWidth - 2, headerHeight_ - 1);

            Pen divPen(Color(180, 0, 200, 240), 1.0f);
            graphics.DrawLine(&divPen, 1, headerHeight_, contentWidth - 2, headerHeight_);

            Pen borderPen(Color(255, 0, 220, 255), 1.5f * dpiScale);
            graphics.DrawRectangle(&borderPen, 1, 1, contentWidth - 3, contentHeight - 3);

            float iconCy = headerHeight_ / 2.0f;
            float iconCx = 14.0f * dpiScale;
            float iconRadius = 5.0f * dpiScale;
            DrawVectorMoveIcon(graphics, iconCx, iconCy, iconRadius, Color(255, 0, 230, 255), 1.5f * dpiScale);

            SolidBrush headerTextBrush(Color(255, 255, 215, 0));
            float headerTx = 24.0f * dpiScale;
            float headerTy = (headerHeight_ - headerBounds.Height) / 2.0f;
            graphics.DrawString(headerText.c_str(), -1, headerFont.get(), PointF(headerTx, headerTy), &headerTextBrush);
        }

        REAL statsX = std::round(6.0f * dpiScale);
        REAL statsBaseY = isPositioningMode_ ? static_cast<REAL>(headerHeight_ + std::round(5.0f * dpiScale)) : std::round(5.0f * dpiScale);
        float shadowOffset = (std::max)(1.0f, std::round(1.0f * dpiScale));

        BYTE rBase = GetRValue(config_.textColor), gBase = GetGValue(config_.textColor), bBase = GetBValue(config_.textColor);
        SolidBrush textBrush(Color(255, rBase, gBase, bBase));

        BYTE rLabel = GetRValue(config_.labelColor), gLabel = GetGValue(config_.labelColor), bLabel = GetBValue(config_.labelColor);
        SolidBrush labelBrush(Color(255, rLabel, gLabel, bLabel));

        BYTE rSep = GetRValue(config_.separatorColor), gSep = GetGValue(config_.separatorColor), bSep = GetBValue(config_.separatorColor);
        SolidBrush sepBrush(Color(255, rSep, gSep, bSep));

        BYTE rHigh = GetRValue(config_.colorHigh), gHigh = GetGValue(config_.colorHigh), bHigh = GetBValue(config_.colorHigh);
        SolidBrush highBrush(Color(255, rHigh, gHigh, bHigh));

        BYTE rCrit = GetRValue(config_.colorCritical), gCrit = GetGValue(config_.colorCritical), bCrit = GetBValue(config_.colorCritical);
        SolidBrush critBrush(Color(255, rCrit, gCrit, bCrit));

        SolidBrush shadowBrush(Color(180, 0, 0, 0));

        // Draw each grid cell at its exact computed column coordinate
        for (int r = 0; r < grid.numRows; ++r) {
            REAL rowY = statsBaseY + r * (singleLineHeight + rowGapPx);

            for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
                const auto& cell = grid.rows[r].cells[ci];
                if (cell.token.empty() || cell.colIndex >= grid.numCols) continue;

                REAL cellX = statsX + colX[cell.colIndex];
                REAL curX = cellX;

                for (const auto& seg : cellSegs[r][ci]) {
                    if (seg.text.empty()) continue;
                    RectF segBounds{};
                    graphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);

                    if (config_.dropShadow) {
                        graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX + shadowOffset, rowY + shadowOffset), &format, &shadowBrush);
                    }

                    Brush* pBrush = &textBrush;
                    if (seg.type == SegmentType::Separator || seg.isSeparator) {
                        pBrush = &sepBrush;
                    } else if (seg.type == SegmentType::Label) {
                        pBrush = &labelBrush;
                    } else if (seg.severity == MetricSeverity::Critical) {
                        pBrush = &critBrush;
                    } else if (seg.severity == MetricSeverity::High) {
                        pBrush = &highBrush;
                    }

                    graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX, rowY), &format, pBrush);
                    curX += segBounds.Width;
                }
            }
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE wndSize = { contentWidth, contentHeight };
    POINT ptDst = { windowX_, windowY_ };

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    BYTE alpha = static_cast<BYTE>(std::clamp((config_.opacity * 255) / 100, 25, 255));
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, hdcScreen, &ptDst, &wndSize, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    if (!isPositioningMode_) {
        HWND prevWnd = GetWindow(hwnd_, GW_HWNDPREV);
        if (prevWnd != nullptr) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
        }
    }

    SelectObject(memDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
}

void OverlayWindow::RenderCustomGridIslands(const SystemMetrics& metrics) {
    if (!hwnd_) return;
    const auto& grid = config_.customGrid;
    if (grid.numCols <= 0 || grid.numRows <= 0) {
        lastRows_ = Formatter::FormatRows(metrics, &config_);
        RenderOverlay(lastRows_);
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    if (!memDC) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    float dpiScale = (currentDpi_ > 0 ? currentDpi_ : 96) / 96.0f;
    auto overlayFont = CreateOverlayFont(config_.fontFace, config_.fontWeight, config_.fontSize, dpiScale);

    float headerFontSize = std::round(11.0f * dpiScale);
    auto headerFont = std::make_unique<Font>(L"Segoe UI", headerFontSize, FontStyleBold, UnitPixel);
    if (headerFont->GetLastStatus() != Status::Ok) {
        headerFont = std::make_unique<Font>(L"Consolas", headerFontSize, FontStyleBold, UnitPixel);
    }

    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    struct ActiveCell {
        int r = 0;
        int ci = 0;
        int colIndex = 0;
        int colSpan = 1;
        std::string token;
        std::vector<FormattedSegment> segs;
        REAL width = 0.0f;
    };

    std::vector<ActiveCell> activeCells;
    REAL maxLineHeight = 0.0f;

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

        for (int r = 0; r < grid.numRows; ++r) {
            for (size_t ci = 0; ci < grid.rows[r].cells.size(); ++ci) {
                const auto& cell = grid.rows[r].cells[ci];
                if (cell.token.empty()) continue;

                auto segs = Formatter::FormatCell(cell.token, metrics, &config_);
                if (segs.empty()) continue;

                REAL segW = 0.0f;
                for (const auto& s : segs) {
                    if (s.text.empty()) continue;
                    RectF sb{};
                    measureGraphics.MeasureString(s.text.c_str(), static_cast<int>(s.text.length()), overlayFont.get(), PointF(0, 0), &format, &sb);
                    segW += sb.Width;
                    if (sb.Height > maxLineHeight) {
                        maxLineHeight = sb.Height;
                    }
                }
                activeCells.push_back({ r, static_cast<int>(ci), cell.colIndex, cell.colSpan, cell.token, std::move(segs), segW });
            }
        }
    }

    std::vector<REAL> colWidth(grid.numCols, 0.0f);
    std::vector<REAL> colX(grid.numCols, 0.0f);
    REAL cellGapPx = config_.cellGap * dpiScale;
    REAL rowGapPx = config_.rowGap * dpiScale;

    std::vector<std::vector<REAL>> cellWidths(grid.numRows);
    for (int r = 0; r < grid.numRows; ++r) {
        cellWidths[r].resize(grid.rows[r].cells.size(), 0.0f);
    }
    for (const auto& ac : activeCells) {
        if (ac.r < grid.numRows && ac.ci < static_cast<int>(cellWidths[ac.r].size())) {
            cellWidths[ac.r][ac.ci] = ac.width;
        }
    }

    ComputeGridColumns(grid, cellWidths, dpiScale, cellGapPx, colWidth, colX);

    int singleLineHeight = static_cast<int>(std::ceil(maxLineHeight));
    if (singleLineHeight <= 0) singleLineHeight = static_cast<int>(std::round(18.0f * dpiScale));

    REAL totalStatsWidthReal = (grid.numCols > 0) ? (colX.back() + colWidth.back()) : 0.0f;
    int statsWidth = static_cast<int>(std::ceil(totalStatsWidthReal));
    int totalStatsHeight = grid.numRows * singleLineHeight + (grid.numRows > 1 ? static_cast<int>(std::round((grid.numRows - 1) * rowGapPx)) : 0);

    int padX = static_cast<int>(std::round(10.0f * dpiScale));
    int padY = static_cast<int>(std::round(6.0f * dpiScale));

    int contentWidth = statsWidth + padX;
    int contentHeight = totalStatsHeight + padY;

    if (!isPositioningMode_) {
        RecalculateWindowPosition(contentWidth, contentHeight);
    } else {
        RECT curRect{};
        if (GetWindowRect(hwnd_, &curRect) && (curRect.right - curRect.left > 0)) {
            windowX_ = curRect.left;
            windowY_ = curRect.top;
        }
    }

    // Synchronize island window instances
    if (islandSlots_.size() != activeCells.size()) {
        DestroyIslandWindows();
        islandSlots_.resize(activeCells.size());
        for (size_t i = 0; i < activeCells.size(); ++i) {
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            HWND hIsland = CreateWindowExW(exStyle, L"BareStatsIslandClass", L"BareStats Island", WS_POPUP,
                                           0, 0, 1, 1, nullptr, nullptr, hInstance_, nullptr);
            islandSlots_[i].hwnd = hIsland;
        }
    }

    std::wstring headerText = L"MOVE MODE: Drag Anchor to reposition \x2022 Ctrl+Shift+M to lock";
    RectF headerBounds{};

    if (isPositioningMode_) {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
        RectF layoutRect(0, 0, 4000.0f * dpiScale, 300.0f * dpiScale);
        measureGraphics.MeasureString(headerText.c_str(), -1, headerFont.get(), layoutRect, &headerBounds);
        headerHeight_ = static_cast<int>(std::round(24.0f * dpiScale));
        windowWidth_ = (std::max)(contentWidth, static_cast<int>(std::ceil(headerBounds.Width)) + static_cast<int>(std::round(36.0f * dpiScale)));
        windowHeight_ = headerHeight_;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = windowWidth_;
        bmi.bmiHeader.biHeight = -windowHeight_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pvBits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
        if (hBmp) {
            HBITMAP hOldBmp = reinterpret_cast<HBITMAP>(SelectObject(memDC, hBmp));
            memset(pvBits, 0, windowWidth_ * windowHeight_ * 4);
            Graphics graphics(memDC);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

            SolidBrush titleBgBrush(Color(245, 26, 36, 50));
            graphics.FillRectangle(&titleBgBrush, 1, 1, windowWidth_ - 2, headerHeight_ - 1);
            Pen borderPen(Color(255, 0, 220, 255), 1.5f * dpiScale);
            graphics.DrawRectangle(&borderPen, 1, 1, windowWidth_ - 3, headerHeight_ - 3);

            float iconCy = headerHeight_ / 2.0f;
            float iconCx = 14.0f * dpiScale;
            float iconRadius = 5.0f * dpiScale;
            DrawVectorMoveIcon(graphics, iconCx, iconCy, iconRadius, Color(255, 0, 230, 255), 1.5f * dpiScale);

            SolidBrush headerTextBrush(Color(255, 255, 215, 0));
            float headerTx = 24.0f * dpiScale;
            float headerTy = (headerHeight_ - headerBounds.Height) / 2.0f;
            graphics.DrawString(headerText.c_str(), -1, headerFont.get(), PointF(headerTx, headerTy), &headerTextBrush);

            POINT ptSrc = { 0, 0 };
            SIZE wndSize = { windowWidth_, windowHeight_ };
            POINT ptDst = { windowX_, windowY_ };
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            UpdateLayeredWindow(hwnd_, hdcScreen, &ptDst, &wndSize, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

            SelectObject(memDC, hOldBmp);
            DeleteObject(hBmp);
        }
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }

    BYTE rBase = GetRValue(config_.textColor), gBase = GetGValue(config_.textColor), bBase = GetBValue(config_.textColor);
    SolidBrush textBrush(Color(255, rBase, gBase, bBase));
    BYTE rLabel = GetRValue(config_.labelColor), gLabel = GetGValue(config_.labelColor), bLabel = GetBValue(config_.labelColor);
    SolidBrush labelBrush(Color(255, rLabel, gLabel, bLabel));
    BYTE rSep = GetRValue(config_.separatorColor), gSep = GetGValue(config_.separatorColor), bSep = GetBValue(config_.separatorColor);
    SolidBrush sepBrush(Color(255, rSep, gSep, bSep));
    BYTE rHigh = GetRValue(config_.colorHigh), gHigh = GetGValue(config_.colorHigh), bHigh = GetBValue(config_.colorHigh);
    SolidBrush highBrush(Color(255, rHigh, gHigh, bHigh));
    BYTE rCrit = GetRValue(config_.colorCritical), gCrit = GetGValue(config_.colorCritical), bCrit = GetBValue(config_.colorCritical);
    SolidBrush critBrush(Color(255, rCrit, gCrit, bCrit));
    SolidBrush shadowBrush(Color(180, 0, 0, 0));
    float shadowOffset = (std::max)(1.0f, std::round(1.0f * dpiScale));

    // Render each Island window independently
    for (size_t i = 0; i < activeCells.size(); ++i) {
        const auto& ac = activeCells[i];
        auto& slot = islandSlots_[i];
        slot.token = ac.token;
        slot.colIndex = ac.colIndex;
        slot.rowIndex = ac.r;

        int relX = static_cast<int>(colX[ac.colIndex]);
        int relY = isPositioningMode_ ? (headerHeight_ + 4 + static_cast<int>(ac.r * (singleLineHeight + rowGapPx)))
                                      : static_cast<int>(ac.r * (singleLineHeight + rowGapPx));
        slot.relX = relX;
        slot.relY = relY;

        int endCol = (std::min)(ac.colIndex + ac.colSpan, grid.numCols);
        int spanWidth = (endCol > ac.colIndex)
            ? static_cast<int>(std::ceil(colX[endCol - 1] + colWidth[endCol - 1] - colX[ac.colIndex]))
            : static_cast<int>(std::ceil(ac.width));
        int w = (std::max)(static_cast<int>(std::ceil(ac.width)), spanWidth) + padX;
        int h = singleLineHeight + padY;
        slot.width = w;
        slot.height = h;

        int screenX = windowX_ + relX;
        int screenY = windowY_ + relY;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pvBits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
        if (!hBmp) continue;

        HBITMAP hOldBmp = reinterpret_cast<HBITMAP>(SelectObject(memDC, hBmp));
        memset(pvBits, 0, w * h * 4);

        {
            Graphics graphics(memDC);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

            if (isPositioningMode_) {
                SolidBrush cellBg(Color(180, 15, 20, 28));
                graphics.FillRectangle(&cellBg, 0, 0, w, h);
                Pen cellBorder(Color(200, 0, 200, 240), 1.0f);
                graphics.DrawRectangle(&cellBorder, 0, 0, w - 1, h - 1);
            }

            REAL curX = std::round(4.0f * dpiScale);
            REAL curY = std::round(3.0f * dpiScale);

            for (const auto& seg : ac.segs) {
                if (seg.text.empty()) continue;
                RectF segBounds{};
                graphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);

                if (config_.dropShadow) {
                    graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX + shadowOffset, curY + shadowOffset), &format, &shadowBrush);
                }

                Brush* pBrush = &textBrush;
                if (seg.type == SegmentType::Separator || seg.isSeparator) {
                    pBrush = &sepBrush;
                } else if (seg.type == SegmentType::Label) {
                    pBrush = &labelBrush;
                } else if (seg.severity == MetricSeverity::Critical) {
                    pBrush = &critBrush;
                } else if (seg.severity == MetricSeverity::High) {
                    pBrush = &highBrush;
                }

                graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX, curY), &format, pBrush);
                curX += segBounds.Width;
            }
        }

        POINT ptSrc = { 0, 0 };
        SIZE wndSize = { w, h };
        POINT ptDst = { screenX, screenY };

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        BYTE alpha = static_cast<BYTE>(std::clamp((config_.opacity * 255) / 100, 25, 255));
        blend.SourceConstantAlpha = alpha;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(slot.hwnd, hdcScreen, &ptDst, &wndSize, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
        ShowWindow(slot.hwnd, SW_SHOWNOACTIVATE);

        if (!isPositioningMode_) {
            HWND prevWnd = GetWindow(slot.hwnd, GW_HWNDPREV);
            if (prevWnd != nullptr) {
                SetWindowPos(slot.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
            }
        }

        SelectObject(memDC, hOldBmp);
        DeleteObject(hBmp);
    }

    DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
}

} // namespace BareStats
