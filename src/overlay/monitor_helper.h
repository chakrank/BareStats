#pragma once

#include "../util/geometry.h"
#include <vector>
#include <string>

namespace BareStats {

class MonitorHelper {
public:
    static std::vector<MonitorInfo> EnumerateMonitors();
    static MonitorInfo GetMonitor(int monitorIndex);
    static Point CalculatePosition(const MonitorInfo& monitor, PositionPreset preset, int windowWidth, int windowHeight, int marginX = 12, int marginY = 12);
    static PositionPreset StringToPreset(const std::string& str);
    static std::string PresetToString(PositionPreset preset);
};

} // namespace BareStats
