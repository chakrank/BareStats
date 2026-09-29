#pragma once

#include "../config/config.h"
#include "../metrics/metrics.h"
#include <windows.h>
#include <string>

namespace BareStats {

class OverlayWindow {
public:
    OverlayWindow(AppConfig& config);
    ~OverlayWindow();

    bool Initialize(HINSTANCE hInstance);
    void Show();
    void Hide();
    void UpdateMetrics(const SystemMetrics& metrics);
    void SetPositioningMode(bool enable);
    bool IsPositioningMode() const { return isPositioningMode_; }
    HWND GetHwnd() const { return hwnd_; }

    void OnMoved();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderText(const std::wstring& text);
    void RecalculateWindowPosition(int contentWidth, int contentHeight);

    AppConfig& config_;
    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;

    bool isPositioningMode_ = false;
    std::wstring lastRenderedText_;
    int windowX_ = 0;
    int windowY_ = 0;
    int windowWidth_ = 600;
    int windowHeight_ = 40;

    static const UINT_PTR HOTKEY_DRAG_ID = 1001;
};

} // namespace BareStats
