#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BareStats {

struct Point {
    int x = 0;
    int y = 0;

#ifdef _WIN32
    Point() = default;
    Point(int px, int py) : x(px), y(py) {}
    Point(const POINT& pt) : x(pt.x), y(pt.y) {}
    operator POINT() const { return POINT{ x, y }; }
#endif
};

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }

#ifdef _WIN32
    Rect() = default;
    Rect(int l, int t, int r, int b) : left(l), top(t), right(r), bottom(b) {}
    Rect(const RECT& rc) : left(rc.left), top(rc.top), right(rc.right), bottom(rc.bottom) {}
    operator RECT() const { return RECT{ left, top, right, bottom }; }
#endif
};

enum class PositionPreset {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    Custom
};

struct MonitorInfo {
    int index = 0;
    std::string name;
    Rect rcMonitor{};
    Rect rcWork{};
    bool isPrimary = false;
#ifdef _WIN32
    HMONITOR handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

#ifndef _WIN32
using COLORREF = uint32_t;
#define RGB(r, g, b) ((COLORREF)(((uint8_t)(r) | ((uint16_t)((uint8_t)(g)) << 8)) | (((uint32_t)((uint8_t)(b))) << 16)))
#define GetRValue(rgb) ((uint8_t)(rgb))
#define GetGValue(rgb) ((uint8_t)(((uint16_t)(rgb)) >> 8))
#define GetBValue(rgb) ((uint8_t)((rgb) >> 16))
#endif

} // namespace BareStats
