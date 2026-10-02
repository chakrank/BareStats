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

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (s_pOverlayInstance && !s_pOverlayInstance->IsPositioningMode()) {
        HWND overlayHwnd = s_pOverlayInstance->GetHwnd();
        if (overlayHwnd && overlayHwnd != hwnd) {
            SetWindowPos(overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            s_pOverlayInstance->ForceRepaint();
        }
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
                    lastSegments_ = Formatter::FormatSegments(animTargetMetrics_, &config_);
                    RenderOverlay(lastSegments_);

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

                cur.memory.usedBytes = static_cast<uint64_t>(animTargetMetrics_.memory.usedBytes * progress);
                cur.memory.totalBytes = animTargetMetrics_.memory.totalBytes; // Keep max static!

                lastSegments_ = Formatter::FormatSegments(cur, &config_);
                RenderOverlay(lastSegments_);
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
            RenderOverlay(lastSegments_);
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            RecalculateWindowPosition(windowWidth_, windowHeight_);
            SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            RenderOverlay(lastSegments_);
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
    RenderOverlay(lastSegments_);
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
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

void OverlayWindow::Hide() {
    isVisible_ = false;
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
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

    lastRenderedText_.clear();
    RecalculateWindowPosition(windowWidth_, windowHeight_);
    SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    if (!lastSegments_.empty()) {
        lastSegments_ = Formatter::FormatSegments(lastMetrics_, &config_);
        RenderOverlay(lastSegments_);
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
    RenderOverlay(lastSegments_);
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

    zeroMetrics.memory.usedBytes = 0;
    // Keep max static! (zeroMetrics.memory.totalBytes untouched)

    lastRenderedText_.clear();
    lastSegments_ = Formatter::FormatSegments(zeroMetrics, &config_);
    RenderOverlay(lastSegments_);
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

    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);
}

void OverlayWindow::ResetToPreset(PositionPreset preset) {
    config_.preset = preset;
    config_.customX = -1;
    config_.customY = -1;
    config_.customRelX = -1;
    config_.customRelY = -1;
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);

    lastRenderedText_.clear();
    RecalculateWindowPosition(windowWidth_, windowHeight_);
    RenderOverlay(lastSegments_);
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

    auto segments = Formatter::FormatSegments(metrics, &config_);

    // 15-Nanosecond Dirty Frame Check:
    // If formatted text is identical and we are not in interactive move mode, skip 100% of GDI+ redraw!
    std::wstring fullText;
    fullText.reserve(128);
    for (const auto& seg : segments) {
        fullText += seg.text;
    }

    if (!isPositioningMode_ && fullText == lastRenderedText_) {
        return; // Zero work! No DIB section, no GDI+, no UpdateLayeredWindow
    }

    lastRenderedText_ = std::move(fullText);
    lastSegments_ = std::move(segments);
    RenderOverlay(lastSegments_);
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
    if (!hwnd_ || segments.empty()) return;

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
    REAL totalStatsWidth = 0.0f;
    REAL maxStatsHeight = 0.0f;

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
        for (const auto& seg : segments) {
            if (seg.text.empty()) continue;
            RectF segBounds{};
            measureGraphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);
            totalStatsWidth += segBounds.Width;
            if (segBounds.Height > maxStatsHeight) {
                maxStatsHeight = segBounds.Height;
            }
        }
        if (isPositioningMode_) {
            measureGraphics.MeasureString(headerText.c_str(), -1, headerFont.get(), layoutRect, &headerBounds);
        }
    }

    int statsWidth = static_cast<int>(std::ceil(totalStatsWidth));
    int statsHeight = static_cast<int>(std::ceil(maxStatsHeight));

    int padX = static_cast<int>(std::round(14.0f * dpiScale));
    int padY = static_cast<int>(std::round(10.0f * dpiScale));

    int contentWidth = 0;
    int contentHeight = 0;

    if (isPositioningMode_) {
        int headerTextWidth = static_cast<int>(std::ceil(headerBounds.Width));
        headerHeight_ = static_cast<int>(std::round(24.0f * dpiScale));
        int headerExtra = static_cast<int>(std::round(36.0f * dpiScale));
        contentWidth = (std::max)(statsWidth + padX, headerTextWidth + headerExtra);
        contentHeight = headerHeight_ + statsHeight + padY;
    } else {
        contentWidth = statsWidth + padX;
        contentHeight = statsHeight + padY;
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
        REAL statsY = isPositioningMode_ ? static_cast<REAL>(headerHeight_ + std::round(5.0f * dpiScale)) : std::round(5.0f * dpiScale);
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

        // Render each segment token-by-token
        REAL curX = statsX;
        for (const auto& seg : segments) {
            if (seg.text.empty()) continue;

            RectF segBounds{};
            graphics.MeasureString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(0, 0), &format, &segBounds);

            // Draw smooth single-offset shadow for contrast
            if (config_.dropShadow) {
                graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX + shadowOffset, statsY + shadowOffset), &format, &shadowBrush);
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

            graphics.DrawString(seg.text.c_str(), static_cast<int>(seg.text.length()), overlayFont.get(), PointF(curX, statsY), &format, pBrush);

            curX += segBounds.Width;
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

} // namespace BareStats
