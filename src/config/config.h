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

struct LlmConfig {
    bool enabled = true;
    std::string endpoints = "local=http://127.0.0.1:11434, local=http://127.0.0.1:8080, local=http://127.0.0.1:8000, local=http://127.0.0.1:1234, local=http://127.0.0.1:38111";
    uint32_t timeoutMs = 300;
    uint32_t pollIntervalMs = 1000;
    uint32_t retryIntervalMs = 10000;
    uint32_t decayHoldSec = 3;
    uint32_t maxModelLen = 16;
    bool filterEmbeddings = true;
    uint32_t minVramMb = 500;
    std::string targetModel = "active";
    std::unordered_map<std::string, std::string> aliases;
};

struct ClusterConfig {
    std::string clusterKey; // 256-bit AEAD key (64 hex characters)
    uint16_t listenPort = 9889;
    std::string allowedRigs; // Comma-separated list of allowed rig IDs (empty = allow all)
    std::unordered_map<std::string, std::string> aliases; // Friendly display aliases (e.g. rig1 = MacMini1)
};

enum class VramMode {
    Physical, // Gross hardware silicon capacity (e.g. 16.0 GB, matches retail box & Task Manager)
    Usable    // Net allocatable capacity excluding driver reservations (e.g. 15.6 GB, AI/CUDA OOM guard)
};

enum class IgpuVramMode {
    Usage,     // True active memory usage only, appends [S] when spilling into shared memory (e.g. '0.4GB [S]' or '45MB')
    Shared,    // Shows usage against max shared system memory pool (e.g. '0.4/7.9GB [S]')
    Dedicated  // Strictly bounds to BIOS dedicated memory (e.g. '128MB')
};

struct AppConfig {
    bool headless = false;                         // General: 1 = Headless Satellite, 0 = Desktop GUI Overlay
    int monitorIndex = 0;
    LlmConfig llm;
    ClusterConfig cluster;
    PositionPreset preset = PositionPreset::TopRight;
    int customX = -1;
    int customY = -1;
    int customRelX = -1;
    int customRelY = -1;
    int margin = 24;

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
    bool showTrayIcon = true;                      // System tray / menu bar icon (Windows, macOS, Linux)
    std::wstring separator = L"\x2502";            // Telemetry divider symbol (default: │)
    bool hasUserDefinedSeparator = false;          // True if separator was explicitly configured by user in config.ini
    std::wstring arrowDown = L"\x2193";            // Download indicator (default: ↓, fallback: v)
    std::wstring arrowUp = L"\x2191";              // Upload indicator (default: ↑, fallback: ^)
    std::string symbolStyle = "auto";              // Symbol resolution: "auto", "ascii", "unicode"

    uint32_t refreshIntervalMs = 1000;
    int gpuIndex = 0;                  // Target GPU device index (0 = primary/dGPU, 1 = secondary/iGPU)
    std::string gpuTarget = "0";       // Multi-GPU target expression ("0", "0, 1", "all")
    std::vector<uint32_t> targetGpuIndices = { 0 };
    bool targetAllGpus = false;
    VramMode vramMode = VramMode::Physical; // VRAM reporting mode (physical vs usable)
    IgpuVramMode igpuVramMode = IgpuVramMode::Usage; // iGPU/on-board memory display mode (usage vs shared vs dedicated)
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

    bool IsMetricInActiveLayout(const std::string& metricType) const {
        std::string lowerType = metricType;
        for (char& c : lowerType) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        auto normalizeToken = [](std::string t) -> std::string {
            for (char& c : t) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (t == "processor") return "cpu";
            if (t.rfind("processor_", 0) == 0) return "cpu_" + t.substr(10);
            if (t == "graphics") return "gpu";
            if (t.rfind("graphics_", 0) == 0) return "gpu_" + t.substr(9);
            if (t == "video") return "gpu";
            if (t.rfind("video_", 0) == 0) return "gpu_" + t.substr(6);
            if (t == "memory") return "ram";
            if (t.rfind("memory_", 0) == 0) return "ram_" + t.substr(7);
            if (t == "mem") return "ram";
            if (t.rfind("mem_", 0) == 0) return "ram_" + t.substr(4);
            if (t == "network") return "net";
            if (t.rfind("network_", 0) == 0) return "net_" + t.substr(8);
            if (t == "wifi") return "net";
            if (t.rfind("wifi_", 0) == 0) return "net_" + t.substr(5);
            if (t == "lan") return "net";
            if (t.rfind("lan_", 0) == 0) return "net_" + t.substr(4);
            return t;
        };

        lowerType = normalizeToken(lowerType);

        if (layout == OverlayLayout::Horizontal || layout == OverlayLayout::Grid) {
            for (const auto& token : metricOrder) {
                std::string t = normalizeToken(token);
                if (lowerType == "cpu" && (t == "cpu" || t.rfind("cpu", 0) == 0)) return true;
                if (lowerType == "ram" && (t == "ram" || t.rfind("ram", 0) == 0)) return true;
                if (lowerType == "gpu" && (t == "gpu" || t.rfind("gpu", 0) == 0)) return true;
                if (lowerType == "net" && (t == "net" || t.rfind("net", 0) == 0)) return true;
            }
            if (layout == OverlayLayout::Grid && lowerType == "gpu") {
                return true;
            }
            return false;
        }

        if (layout == OverlayLayout::CustomGrid) {
            for (const auto& row : customGrid.rows) {
                for (const auto& cell : row.cells) {
                    std::string t = normalizeToken(cell.token);
                    if (lowerType == "cpu" && (t == "cpu" || t.rfind("cpu_", 0) == 0)) return true;
                    if (lowerType == "ram" && (t == "ram" || t.rfind("ram_", 0) == 0)) return true;
                    if (lowerType == "gpu" && (t == "gpu" || t.rfind("gpu", 0) == 0)) return true;
                    if (lowerType == "net" && (t == "net" || t.rfind("net_", 0) == 0)) return true;
                }
            }
            return false;
        }

        return false;
    }
};

// Schema versioning: MAJOR * 100 + MINOR * 10 (e.g. v1.0 = 100, v1.1 = 110, v1.2 = 120, v1.3 = 130)
constexpr int CURRENT_CONFIG_SCHEMA_VERSION = 140;

class ConfigManager {
public:
    static bool CreateDefaultConfigFile(const std::string& iniPath, const AppConfig& config = AppConfig());
    static bool MigrateSchema(const std::string& iniPath, int oldVersion, int newVersion);
    static AppConfig Load(const std::string& iniPath);
    static bool Save(const std::string& iniPath, const AppConfig& config);
    static std::string GetDefaultConfigPath();
    static bool ExportSatelliteIni(const std::string& targetPath, const std::string& clusterKey, const std::string& rigId = "rig1", const std::string& targetHost = "auto", uint16_t targetPort = 9889);
};

} // namespace BareStats
