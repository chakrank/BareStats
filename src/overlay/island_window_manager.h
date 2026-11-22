#pragma once

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace BareStats {

using NativeWindowHandle = void*;

class IslandWindowManager {
public:
    void UpdateIslandVisibility(NativeWindowHandle hwnd, bool should_be_visible) {
        if (!hwnd) return;
        bool currently_visible = IsWindowVisibleOS(hwnd);

        if (should_be_visible && !currently_visible) {
            // Unhide safely without stealing focus from active games/IDEs
            ShowSafely(hwnd);
        } else if (!should_be_visible && currently_visible) {
            Hide(hwnd);
        }
    }

    bool IsWindowVisibleOS(NativeWindowHandle hwnd) const {
        if (!hwnd) return false;
#if defined(_WIN32)
        return IsWindowVisible(static_cast<HWND>(hwnd)) != FALSE;
#else
        return true;
#endif
    }

    void ShowSafely(NativeWindowHandle hwnd) {
        if (!hwnd) return;
#if defined(_WIN32)
        // Windows: SW_SHOWNOACTIVATE shows the window without transferring keyboard focus
        ShowWindow(static_cast<HWND>(hwnd), SW_SHOWNOACTIVATE);
#endif
    }

    void Hide(NativeWindowHandle hwnd) {
        if (!hwnd) return;
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(hwnd), SW_HIDE);
#endif
    }
};

} // namespace BareStats
