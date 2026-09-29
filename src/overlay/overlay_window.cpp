#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay_window.h"
#include "monitor_helper.h"
#include "../util/formatter.h"
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

static HHOOK s_hMouseHook = nullptr;
static OverlayWindow* s_pOverlayInstance = nullptr;

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
    if (s_hMouseHook) {
        UnhookWindowsHookEx(s_hMouseHook);
        s_hMouseHook = nullptr;
    }
    s_pOverlayInstance = nullptr;

    RemoveTrayIcon();

    if (hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_M);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_ALT_M);
        UnregisterHotKey(hwnd_, HOTKEY_CTRL_ALT_P);
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
            if (wParam == HOTKEY_CTRL_SHIFT_M || wParam == HOTKEY_CTRL_ALT_M || wParam == HOTKEY_CTRL_ALT_P) {
                SetPositioningMode(!isPositioningMode_);
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

        case WM_WINDOWPOSCHANGING: {
            auto* pwp = reinterpret_cast<WINDOWPOS*>(lParam);
            pwp->flags |= SWP_NOOWNERZORDER | SWP_NOACTIVATE;
            return 0;
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

    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));

    // Register hotkeys with fallbacks
    // Primary: Ctrl + Shift + M (most reliable across Windows setups)
    RegisterHotKey(hwnd_, HOTKEY_CTRL_SHIFT_M, MOD_CONTROL | MOD_SHIFT, 'M');
    // Secondary: Ctrl + Alt + M
    RegisterHotKey(hwnd_, HOTKEY_CTRL_ALT_M, MOD_CONTROL | MOD_ALT, 'M');
    // Tertiary: Ctrl + Alt + P
    RegisterHotKey(hwnd_, HOTKEY_CTRL_ALT_P, MOD_CONTROL | MOD_ALT, 'P');

    SetupTrayIcon();

    return true;
}

void OverlayWindow::SetupTrayIcon() {
    memset(&nid_, 0, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
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
    } else if (cmd == ID_TRAY_EXIT) {
        DestroyWindow(hwnd_);
    }
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
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd_, HWND_TOPMOST, windowX_, windowY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    RenderOverlay(lastRenderedText_);
}

void OverlayWindow::OnMoved() {
    if (!hwnd_) return;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    windowX_ = rc.left;

    if (isPositioningMode_) {
        windowY_ = rc.top;
        config_.preset = PositionPreset::Custom;
        config_.customX = rc.left;
        config_.customY = rc.top + headerHeight_;
    } else {
        windowY_ = rc.top;
        config_.preset = PositionPreset::Custom;
        config_.customX = rc.left;
        config_.customY = rc.top;
    }
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);
}

void OverlayWindow::ResetToPreset(PositionPreset preset) {
    config_.preset = preset;
    config_.customX = -1;
    config_.customY = -1;
    ConfigManager::Save(ConfigManager::GetDefaultConfigPath(), config_);

    RecalculateWindowPosition(windowWidth_, windowHeight_);
    RenderOverlay(lastRenderedText_);
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
    RenderOverlay(text);
}

static void DrawVectorMoveIcon(Graphics& graphics, float cx, float cy, float radius, Color color) {
    Pen pen(color, 1.5f);
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

void OverlayWindow::RenderOverlay(const std::wstring& text) {
    if (!hwnd_ || text.empty()) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC memDC = CreateCompatibleDC(hdcScreen);
    if (!memDC) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    std::wstring statsText = text;
    std::wstring headerText = L"MOVE MODE: Drag to reposition \x2022 Click outside or Ctrl+Shift+M to lock";

    // Fonts
    auto font = std::make_unique<Font>(config_.fontFace.c_str(), static_cast<REAL>(config_.fontSize), FontStyleBold, UnitPixel);
    if (font->GetLastStatus() != Status::Ok) {
        font = std::make_unique<Font>(L"Consolas", static_cast<REAL>(config_.fontSize), FontStyleBold, UnitPixel);
    }

    auto headerFont = std::make_unique<Font>(L"Segoe UI", 11.0f, FontStyleBold, UnitPixel);
    if (headerFont->GetLastStatus() != Status::Ok) {
        headerFont = std::make_unique<Font>(L"Consolas", 11.0f, FontStyleBold, UnitPixel);
    }

    RectF layoutRect(0, 0, 4000, 300);
    RectF statsBounds{};
    RectF headerBounds{};

    {
        Graphics measureGraphics(memDC);
        measureGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        measureGraphics.MeasureString(statsText.c_str(), -1, font.get(), layoutRect, &statsBounds);
        if (isPositioningMode_) {
            measureGraphics.MeasureString(headerText.c_str(), -1, headerFont.get(), layoutRect, &headerBounds);
        }
    }

    int statsWidth = static_cast<int>(std::ceil(statsBounds.Width));
    int statsHeight = static_cast<int>(std::ceil(statsBounds.Height));

    int contentWidth = 0;
    int contentHeight = 0;

    if (isPositioningMode_) {
        int headerTextWidth = static_cast<int>(std::ceil(headerBounds.Width));
        headerHeight_ = 24;
        contentWidth = (std::max)(statsWidth + 14, headerTextWidth + 36);
        contentHeight = headerHeight_ + statsHeight + 10;
    } else {
        contentWidth = statsWidth + 14;
        contentHeight = statsHeight + 10;
    }

    windowWidth_ = contentWidth;
    windowHeight_ = contentHeight;

    if (!isPositioningMode_ && config_.preset != PositionPreset::Custom) {
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
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

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
            Pen borderPen(Color(255, 0, 220, 255), 1.5f); // vibrant cyan
            graphics.DrawRectangle(&borderPen, 1, 1, contentWidth - 3, contentHeight - 3);

            // 5. Draw vector 4-way move icon in top line
            float iconCy = headerHeight_ / 2.0f;
            DrawVectorMoveIcon(graphics, 14.0f, iconCy, 5.0f, Color(255, 0, 230, 255));

            // 6. Draw header text in top line
            SolidBrush headerTextBrush(Color(255, 255, 215, 0)); // bright gold
            float headerTy = (headerHeight_ - headerBounds.Height) / 2.0f;
            graphics.DrawString(headerText.c_str(), -1, headerFont.get(), PointF(24.0f, headerTy), &headerTextBrush);
        }

        // Stats text position:
        // In Normal Mode: text is at X = 6.0f, Y = 5.0f
        // In Move Mode: text is at X = 6.0f, Y = headerHeight_ + 5.0f
        REAL statsX = 6.0f;
        REAL statsY = isPositioningMode_ ? static_cast<REAL>(headerHeight_ + 5) : 5.0f;

        // Draw shadow for contrast
        if (config_.dropShadow) {
            SolidBrush shadowBrush(Color(220, 0, 0, 0));
            graphics.DrawString(statsText.c_str(), -1, font.get(), PointF(statsX + 1.0f, statsY + 1.0f), &shadowBrush);
            graphics.DrawString(statsText.c_str(), -1, font.get(), PointF(statsX + 2.0f, statsY + 2.0f), &shadowBrush);
        }

        // Main stats text color
        BYTE r = GetRValue(config_.textColor);
        BYTE g = GetGValue(config_.textColor);
        BYTE b = GetBValue(config_.textColor);

        SolidBrush textBrush(Color(255, r, g, b));
        graphics.DrawString(statsText.c_str(), -1, font.get(), PointF(statsX, statsY), &textBrush);
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

    SelectObject(memDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdcScreen);
}

} // namespace BareStats
