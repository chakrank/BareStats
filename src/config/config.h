#pragma once

#include "../overlay/monitor_helper.h"
#include "../util/geometry.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace BareStats {

enum class OverlayLayout {
    Horizontal, // Single-line horizontal ribbon (default)
    Grid,       // Multi-row compact widget layout
    CustomGrid  // User-defined mathematical matrix grid with arbitrary cell placement & colspans
};

enum class GridMode {
    Unified, // Single compact layered window (default, ultra-low memory for compact matrix)
    Islands  // Multi-window / distributed micro-HWNDs (zero overhead across large gaps & multi-monitors)
};

struct GridCell {
    std::string token;  // "cpu", "ram", "gpu0", "net", etc. (empty string for null/empty slots)
    int colIndex = 0;   // 0-based column start index
    int colSpan = 1;    // number of columns spanned
};

struct GridRow {
    std::vector<GridCell> cells;
};

struct CustomGridDefinition {
    int numCols = 0;
    int numRows = 0;
    std::vector<GridRow> rows;
    bool IsEmpty() const {
        if (rows.empty()) return true;
        for (const auto& r : rows) {
            for (const auto& c : r.cells) {
                if (!c.token.empty()) return false;
            }
        }
        return true;
    }
};

struct PerGpuConfig {
    bool enabled = true;
    bool showUtil = true;
    bool showVram = true;
    bool showTemp = true;
    bool showPower = true;
    bool showFan = true;
    std::string label; // Custom label override (e.g. "RTX4090"). If empty, default "GPU" or "GPU0" is used.
};

enum class VramMode {
    Physical, // Gross hardware silicon capacity (e.g. 16.0 GB, matches retail box & Task Manager)
    Usable    // Net allocatable capacity excluding driver reservations (e.g. 15.6 GB, AI/CUDA OOM guard)
};

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
    int opacity = 100;                             // Global opacity in % (10 to 100, default: 100)
    bool showTrayIcon = false;                     // Show system tray / panel icon on Linux (default: false to avoid taskbar clutter)
    std::wstring separator = L"\x2502";            // Telemetry divider symbol (default: │)

    uint32_t refreshIntervalMs = 1000;
    int gpuIndex = 0;                  // Target GPU device index (0 = primary/dGPU, 1 = secondary/iGPU)
    std::string gpuTarget = "0";       // Multi-GPU target expression ("0", "0, 1", "all")
    std::vector<uint32_t> targetGpuIndices = { 0 };
    bool targetAllGpus = false;
    VramMode vramMode = VramMode::Physical; // VRAM reporting mode (physical vs usable)
    std::vector<std::string> metricOrder = { "CPU", "GPU", "RAM", "NET" }; // Metric display ordering

    bool showCpu = true;
    bool showGpu = true;
    bool showRam = true;
    bool showNet = false;              // Network throughput toggle (default: false)
    bool showGpuTemp = true;
    bool showGpuPower = true;
    bool showGpuFan = true;
    bool showMaxMemory = true;         // If false, shows only current (e.g. 11.7GB instead of 11.7/16GB)
    bool hideOfflineMetrics = false;   // If true, completely hides subsystems that are offline/unavailable

    OverlayLayout layout = OverlayLayout::Horizontal; // Presentation layout (Horizontal vs Grid vs CustomGrid)
    GridMode gridMode = GridMode::Unified;            // Unified window vs Islands micro-HWNDs
    int cellGap = 16;                                 // Column gap in pixels for CustomGrid
    int rowGap = 4;                                   // Row gap in pixels for CustomGrid
    CustomGridDefinition customGrid;                  // Parsed matrix grid definition
    int maxGpusPerRow = 2;                            // Max GPUs rendered per row in Grid mode or multi-GPU wrapping
    std::unordered_map<uint32_t, PerGpuConfig> perGpuConfigs;

    PerGpuConfig GetGpuConfig(uint32_t adapterIndex) const {
        auto it = perGpuConfigs.find(adapterIndex);
        if (it != perGpuConfigs.end()) {
            return it->second;
        }
        PerGpuConfig def;
        def.showUtil = true;
        def.showVram = true;
        def.showTemp = showGpuTemp;
        def.showPower = showGpuPower;
        def.showFan = showGpuFan;
        return def;
    }
};

// Schema versioning: MAJOR * 100 + MINOR * 10 (e.g. v1.0 = 100, v1.1 = 110, v1.2 = 120, v1.3 = 130)
constexpr int CURRENT_CONFIG_SCHEMA_VERSION = 130;

class ConfigManager {
public:
    static bool CreateDefaultConfigFile(const std::string& iniPath, const AppConfig& config = AppConfig());
    static bool MigrateSchema(const std::string& iniPath, int oldVersion, int newVersion);
    static AppConfig Load(const std::string& iniPath);
    static bool Save(const std::string& iniPath, const AppConfig& config);
    static std::string GetDefaultConfigPath();
};

} // namespace BareStats
