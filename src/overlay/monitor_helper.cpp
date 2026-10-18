#include "monitor_helper.h"
#include <algorithm>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace BareStats {

#ifdef _WIN32

struct EnumContext {
    std::vector<MonitorInfo> monitors;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* ctx = reinterpret_cast<EnumContext*>(dwData);
    
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfo(hMonitor, &mi)) {
        MonitorInfo info;
        info.handle = hMonitor;
        info.rcMonitor = mi.rcMonitor;
        info.rcWork = mi.rcWork;
        info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        ctx->monitors.push_back(info);
    }
    return TRUE;
}

std::vector<MonitorInfo> MonitorHelper::EnumerateMonitors() {
    EnumContext ctx;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));

    // Ensure primary monitor is index 0
    std::stable_sort(ctx.monitors.begin(), ctx.monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.isPrimary != b.isPrimary) {
            return a.isPrimary > b.isPrimary;
        }
        return a.rcMonitor.left < b.rcMonitor.left;
    });

    for (size_t i = 0; i < ctx.monitors.size(); ++i) {
        ctx.monitors[i].index = static_cast<int>(i);
    }

    if (ctx.monitors.empty()) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.isPrimary = true;
        fallback.rcMonitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        SystemParametersInfo(SPI_GETWORKAREA, 0, &fallback.rcWork, 0);
        ctx.monitors.push_back(fallback);
    }

    return ctx.monitors;
}

#elif defined(__APPLE__)

std::vector<MonitorInfo> MonitorHelper::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;

    CGDirectDisplayID displayIDs[32];
    uint32_t displayCount = 0;
    if (CGGetActiveDisplayList(32, displayIDs, &displayCount) == kCGErrorSuccess && displayCount > 0) {
        CGDirectDisplayID mainDisplay = CGMainDisplayID();
        for (uint32_t i = 0; i < displayCount; ++i) {
            CGDirectDisplayID id = displayIDs[i];
            CGRect bounds = CGDisplayBounds(id);

            MonitorInfo info;
            info.index = static_cast<int>(i);
            info.isPrimary = (id == mainDisplay);
            info.name = "Display " + std::to_string(i + 1);
            info.rcMonitor = {
                static_cast<int>(bounds.origin.x),
                static_cast<int>(bounds.origin.y),
                static_cast<int>(bounds.origin.x + bounds.size.width),
                static_cast<int>(bounds.origin.y + bounds.size.height)
            };
            info.rcWork = info.rcMonitor;
            if (info.isPrimary) {
                info.rcWork.top += 25; // Default macOS menu bar height
            }
            monitors.push_back(info);
        }
    }

    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.isPrimary != b.isPrimary) {
            return a.isPrimary > b.isPrimary;
        }
        return a.rcMonitor.left < b.rcMonitor.left;
    });

    for (size_t i = 0; i < monitors.size(); ++i) {
        monitors[i].index = static_cast<int>(i);
    }

    if (monitors.empty()) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.isPrimary = true;
        fallback.rcMonitor = { 0, 0, 1920, 1080 };
        fallback.rcWork = { 0, 25, 1920, 1080 };
        monitors.push_back(fallback);
    }

    return monitors;
}

#else // Linux fallback

std::vector<MonitorInfo> MonitorHelper::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;

    MonitorInfo primary;
    primary.index = 0;
    primary.isPrimary = true;
    primary.rcMonitor = { 0, 0, 1920, 1080 };
    primary.rcWork = { 0, 0, 1920, 1080 };
    monitors.push_back(primary);

    return monitors;
}

#endif // _WIN32

MonitorInfo MonitorHelper::GetMonitor(int monitorIndex) {
    auto monitors = EnumerateMonitors();
    if (monitorIndex >= 0 && monitorIndex < static_cast<int>(monitors.size())) {
        return monitors[monitorIndex];
    }
    return monitors.front();
}

Point MonitorHelper::CalculatePosition(const MonitorInfo& monitor, PositionPreset preset, int windowWidth, int windowHeight, int marginX, int marginY) {
    const Rect& work = monitor.rcWork;
    int workWidth = work.right - work.left;
    int workHeight = work.bottom - work.top;

    Point pt{};
    switch (preset) {
        case PositionPreset::TopLeft:
            pt.x = work.left + marginX;
            pt.y = work.top + marginY;
            break;
        case PositionPreset::TopCenter:
            pt.x = work.left + (workWidth - windowWidth) / 2;
            pt.y = work.top + marginY;
            break;
        case PositionPreset::TopRight:
            pt.x = work.right - windowWidth - marginX;
            pt.y = work.top + marginY;
            break;
        case PositionPreset::CenterLeft:
            pt.x = work.left + marginX;
            pt.y = work.top + (workHeight - windowHeight) / 2;
            break;
        case PositionPreset::Center:
            pt.x = work.left + (workWidth - windowWidth) / 2;
            pt.y = work.top + (workHeight - windowHeight) / 2;
            break;
        case PositionPreset::CenterRight:
            pt.x = work.right - windowWidth - marginX;
            pt.y = work.top + (workHeight - windowHeight) / 2;
            break;
        case PositionPreset::BottomLeft:
            pt.x = work.left + marginX;
            pt.y = work.bottom - windowHeight - marginY;
            break;
        case PositionPreset::BottomRight:
            pt.x = work.right - windowWidth - marginX;
            pt.y = work.bottom - windowHeight - marginY;
            break;
        case PositionPreset::Custom:
        case PositionPreset::BottomCenter:
        default:
            pt.x = work.left + (workWidth - windowWidth) / 2;
            pt.y = work.bottom - windowHeight - marginY;
            break;
    }
    return pt;
}

PositionPreset MonitorHelper::StringToPreset(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower == "top-left" || lower == "topleft") return PositionPreset::TopLeft;
    if (lower == "top-center" || lower == "topcenter") return PositionPreset::TopCenter;
    if (lower == "top-right" || lower == "topright") return PositionPreset::TopRight;
    if (lower == "center-left" || lower == "centerleft") return PositionPreset::CenterLeft;
    if (lower == "center") return PositionPreset::Center;
    if (lower == "center-right" || lower == "centerright") return PositionPreset::CenterRight;
    if (lower == "bottom-left" || lower == "bottomleft") return PositionPreset::BottomLeft;
    if (lower == "bottom-center" || lower == "bottomcenter") return PositionPreset::BottomCenter;
    if (lower == "bottom-right" || lower == "bottomright") return PositionPreset::BottomRight;
    if (lower == "custom") return PositionPreset::Custom;

    return PositionPreset::BottomCenter;
}

std::string MonitorHelper::PresetToString(PositionPreset preset) {
    switch (preset) {
        case PositionPreset::TopLeft: return "top-left";
        case PositionPreset::TopCenter: return "top-center";
        case PositionPreset::TopRight: return "top-right";
        case PositionPreset::CenterLeft: return "center-left";
        case PositionPreset::Center: return "center";
        case PositionPreset::CenterRight: return "center-right";
        case PositionPreset::BottomLeft: return "bottom-left";
        case PositionPreset::BottomCenter: return "bottom-center";
        case PositionPreset::BottomRight: return "bottom-right";
        case PositionPreset::Custom: return "custom";
    }
    return "bottom-center";
}

} // namespace BareStats
