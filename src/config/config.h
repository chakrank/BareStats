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
    int customRelX = -1;
    int customRelY = -1;
    int margin = 12;

    std::wstring fontFace = L"auto";
    std::wstring fontWeight = L"semibold";
    int fontSize = 13;
    COLORREF textColor = RGB(255, 255, 255);       // bright crisp white for data and units
    COLORREF labelColor = RGB(160, 160, 160);      // soft muted silver for labels
    COLORREF separatorColor = RGB(110, 110, 110);  // quiet subtle divider
    COLORREF colorHigh = RGB(255, 215, 0);         // yellow for high utilization
    COLORREF colorCritical = RGB(255, 77, 77);     // red for critical / near-max
    bool enableDynamicColors = true;
    bool dropShadow = true;

    uint32_t refreshIntervalMs = 1000;

    bool showCpu = true;
    bool showGpu = true;
    bool showRam = true;
    bool showGpuTemp = true;
    bool showGpuPower = true;
    bool showGpuFan = true;
    bool showMaxMemory = true;         // If false, shows only current (e.g. 11.7GB instead of 11.7/16GB)
    bool hideOfflineMetrics = false;   // If true, completely hides subsystems that are offline/unavailable
};

class ConfigManager {
public:
    static AppConfig Load(const std::string& iniPath);
    static bool Save(const std::string& iniPath, const AppConfig& config);
    static std::string GetDefaultConfigPath();
};

} // namespace BareStats
