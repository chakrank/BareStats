#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay_window.h"
#include "monitor_helper.h"
#include "../util/formatter.h"
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace BareStats {

OverlayWindow::OverlayWindow(AppConfig& config)
    : config_(config) {
}

OverlayWindow::~OverlayWindow() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_DRAG_ID);
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
    switch (msg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_DRAG_ID) {
                SetPositioningMode(!isPositioningMode_);
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
            if (isPositioningMode_) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
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

        case WM_DESTROY:
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

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(exStyle, L"BareStatsOverlayClass", L"BareStats Overlay", style,
                            0, 0, windowWidth_, windowHeight_,
                            nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    RegisterHotKey(hwnd_, HOTKEY_DRAG_ID, MOD_CONTROL | MOD_ALT, 'M');

    return true;
}

void OverlayWindow::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

void OverlayWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void OverlayWindow::SetPositioningMode(bool enable) {
    if (isPositioningMode_ == enable) return;
    isPositioningMode_ = enable;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        exStyle &= ~WS_EX_TRANSPARENT;
    } else {
        exStyle |= WS_EX_TRANSPARENT;
        OnMoved();
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    RenderText(lastRenderedText_);
}

void OverlayWindow::OnMoved() {
    if (!hwnd_) return;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    windowX_ = rc.left;
    windowY_ = rc.top;

    config_.preset = PositionPreset::Custom;
    config_.customX = windowX_;
    config_.customY = windowY_;
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);
}

void OverlayWindow::RecalculateWindowPosition(int contentWidth, int contentHeight) {
    if (config_.preset == PositionPreset::Custom && config_.customX >= 0 && config_.customY >= 0) {
        windowX_ = config_.customX;
        windowY_ = config_.customY;
    } else {
        auto monitor = MonitorHelper::GetMonitor(config_.monitorIndex);
        POINT pt = MonitorHelper::CalculatePosition(monitor, config_.preset, contentWidth, contentHeight, config_.margin, config_.margin);
        windowX_ = pt.x;
        windowY_ = pt.y;
    }
}

void OverlayWindow::UpdateMetrics(const SystemMetrics& metrics) {
    std::wstring text = Formatter::Format(metrics, &config_);
    lastRenderedText_ = text;
    RenderText(text);
}

void OverlayWindow::RenderText(const std::wstring& text) {
    if (!hwnd_ || text.empty()) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    if (!memDC) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    std::wstring displayText = text;
    if (isPositioningMode_) {
        displayText = L"[DRAG MODE - Ctrl+Alt+M to lock] " + text;
    }

    // Measure text bounds
    auto font = std::make_unique<Font>(config_.fontFace.c_str(), static_cast<REAL>(config_.fontSize), FontStyleBold, UnitPixel);
    if (font->GetLastStatus() != Status::Ok) {
        font = std::make_unique<Font>(L"Consolas", static_cast<REAL>(config_.fontSize), FontStyleBold, UnitPixel);
    }

    RectF layoutRect(0, 0, 4000, 300);
    RectF boundingBox{};

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        measureGraphics.MeasureString(displayText.c_str(), -1, font.get(), layoutRect, &boundingBox);
    }

    int contentWidth = static_cast<int>(std::ceil(boundingBox.Width)) + 16;
    int contentHeight = static_cast<int>(std::ceil(boundingBox.Height)) + 12;

    if (contentWidth < 50) contentWidth = 50;
    if (contentHeight < 20) contentHeight = 20;

    windowWidth_ = contentWidth;
    windowHeight_ = contentHeight;

    RecalculateWindowPosition(windowWidth_, windowHeight_);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = contentWidth;
    bmi.bmiHeader.biHeight = -contentHeight; // Top-down DIB
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
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        REAL textX = 4.0f;
        REAL textY = 4.0f;

        // Draw shadow/outline for legibility over light windows
        if (config_.dropShadow) {
            SolidBrush shadowBrush(Color(220, 0, 0, 0));
            graphics.DrawString(displayText.c_str(), -1, font.get(), PointF(textX + 1.0f, textY + 1.0f), &shadowBrush);
            graphics.DrawString(displayText.c_str(), -1, font.get(), PointF(textX + 2.0f, textY + 2.0f), &shadowBrush);
        }

        // Draw text
        BYTE r = GetRValue(config_.textColor);
        BYTE g = GetGValue(config_.textColor);
        BYTE b = GetBValue(config_.textColor);

        if (isPositioningMode_) {
            // Gold/amber in drag mode
            r = 255; g = 215; b = 0;
        }

        SolidBrush textBrush(Color(255, r, g, b));
        graphics.DrawString(displayText.c_str(), -1, font.get(), PointF(textX, textY), &textBrush);

        if (isPositioningMode_) {
            Pen borderPen(Color(220, 255, 215, 0), 1.0f);
            borderPen.SetDashStyle(DashStyleDash);
            graphics.DrawRectangle(&borderPen, 1, 1, contentWidth - 3, contentHeight - 3);
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE wndSize = { contentWidth, contentHeight };
    POINT ptDst = { windowX_, windowY_ };

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, hdcScreen, &ptDst, &wndSize, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, windowWidth_, windowHeight_, SWP_NOACTIVATE);

    SelectObject(memDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
}

} // namespace BareStats
