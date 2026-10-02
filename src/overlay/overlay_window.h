#pragma once

#include "../config/config.h"
#include "../metrics/metrics.h"
#include "../util/formatter.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

#include <functional>

namespace BareStats {

class OverlayWindow {
public:
    OverlayWindow(AppConfig& config);
    ~OverlayWindow();

    bool Initialize(HINSTANCE hInstance);
    void Show();
    void Hide();
    void ToggleVisibility();
    bool IsVisible() const { return isVisible_; }
    void ApplyConfig(const AppConfig& newConfig);
    void UpdateMetrics(const SystemMetrics& metrics);
    void SetPositioningMode(bool enable);
    bool IsPositioningMode() const { return isPositioningMode_; }
    HWND GetHwnd() const { return hwnd_; }

    void OnMoved();
    void ResetToPreset(PositionPreset preset = PositionPreset::BottomCenter);
    void TriggerLaunchAnimation(const SystemMetrics& targetMetrics);

    void SetDisplayStateCallback(std::function<void(bool /*screenOn*/)> cb) { onDisplayStateChanged_ = std::move(cb); }
    void SetTelemetryCallback(std::function<void()> cb) { onTelemetryReady_ = std::move(cb); }
    void SetSystemResumeCallback(std::function<void()> cb) { onSystemResume_ = std::move(cb); }
    void SetEndSessionCallback(std::function<void()> cb) { onEndSession_ = std::move(cb); }
    void SetConfigReloadCallback(std::function<void()> cb) { onConfigReload_ = std::move(cb); }

    UINT GetCurrentDpi() const { return currentDpi_; }

    void ForceRepaint();
    bool IsAutoStartEnabled() const;
    bool SetAutoStart(bool enable);

    static const UINT ID_EXIT_MOVE_MODE      = 2004;
    static const UINT WM_TRIGGER_LAUNCH_ANIM = WM_USER + 101;
    static const UINT WM_TELEMETRY_UPDATED   = WM_USER + 102;
    static const UINT WM_CONFIG_RELOADED     = WM_USER + 103;
    static UINT s_uTaskbarRestartMsg;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderOverlay(const std::vector<FormattedSegment>& segments);
    void RecalculateWindowPosition(int contentWidth, int contentHeight);

    void SetupTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayContextMenu();

    AppConfig& config_;
    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    HPOWERNOTIFY hPowerNotify_ = nullptr;

    std::function<void(bool)> onDisplayStateChanged_;
    std::function<void()> onTelemetryReady_;
    std::function<void()> onSystemResume_;
    std::function<void()> onEndSession_;
    std::function<void()> onConfigReload_;
    std::wstring lastRenderedText_;
    UINT currentDpi_ = 96;

    bool isVisible_ = true;
    bool isPositioningMode_ = false;
    bool isAnimatingRollUp_ = false;
    int animTicksRemaining_ = 0;
    SystemMetrics animTargetMetrics_{};
    SystemMetrics lastMetrics_{};
    std::vector<FormattedSegment> lastSegments_;
    int windowX_ = 0;
    int windowY_ = 0;
    int windowWidth_ = 600;
    int windowHeight_ = 40;
    int headerHeight_ = 26;

    NOTIFYICONDATAW nid_{};
    bool trayIconActive_ = false;

    static const UINT_PTR HOTKEY_CTRL_SHIFT_M = 1001;
    static const UINT_PTR HOTKEY_CTRL_ALT_M   = 1002;
    static const UINT_PTR HOTKEY_CTRL_ALT_P   = 1003;
    static const UINT_PTR HOTKEY_CTRL_SHIFT_H = 1004;
    static const UINT_PTR HOTKEY_CTRL_ALT_H   = 1005;
    static const UINT_PTR TIMER_ANIM_ID       = 2005;

    static const UINT WM_TRAYICON             = WM_USER + 100;
    static const UINT ID_TRAY_TOGGLE_MOVE     = 2001;
    static const UINT ID_TRAY_RESET_POS       = 2002;
    static const UINT ID_TRAY_EXIT            = 2003;
    static const UINT ID_TRAY_AUTOSTART       = 2006;
    static const UINT ID_TRAY_TOGGLE_VISIBILITY = 2007;
    static const UINT ID_TRAY_RELOAD_CONFIG   = 2008;
};

} // namespace BareStats
