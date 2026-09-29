#pragma once

#include <windows.h>
#include <vector>
#include <string>

namespace BareStats {

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
    HMONITOR handle = nullptr;
    RECT rcMonitor{};
    RECT rcWork{};
    bool isPrimary = false;
};

class MonitorHelper {
public:
    static std::vector<MonitorInfo> EnumerateMonitors();
    static MonitorInfo GetMonitor(int monitorIndex);
    static POINT CalculatePosition(const MonitorInfo& monitor, PositionPreset preset, int windowWidth, int windowHeight, int marginX = 12, int marginY = 12);
    static PositionPreset StringToPreset(const std::string& str);
    static std::string PresetToString(PositionPreset preset);
};

} // namespace BareStats
