#pragma once

#include "../overlay/monitor_helper.h"
#include <windows.h>
#include <string>

namespace BareStats {

struct AppConfig {
    int monitorIndex = 0;
    PositionPreset preset = PositionPreset::BottomCenter;
    int customX = -1;
    int customY = -1;
    int margin = 12;

    std::wstring fontFace = L"Consolas";
    int fontSize = 14;
    COLORREF textColor = RGB(220, 220, 220); // crisp light silver-white
    COLORREF colorHigh = RGB(255, 215, 0);   // yellow for high utilization
    COLORREF colorCritical = RGB(255, 77, 77); // red for critical / near-max
    bool enableDynamicColors = true;
    bool dropShadow = true;

    uint32_t refreshIntervalMs = 1000;

    bool showCpu = true;
    bool showGpu = true;
    bool showRam = true;
    bool showGpuTemp = true;
    bool showGpuPower = true;
    bool showGpuFan = true;
};

class ConfigManager {
public:
    static AppConfig Load(const std::string& iniPath);
    static bool Save(const std::string& iniPath, const AppConfig& config);
    static std::string GetDefaultConfigPath();
};

} // namespace BareStats
