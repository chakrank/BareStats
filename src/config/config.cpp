#include "config.h"
#include "../metrics/gpu_collector.h"
#include <shlobj.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace BareStats {

static std::string ColorToHex(COLORREF color) {
    std::ostringstream oss;
    oss << "#"
        << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << static_cast<int>(GetRValue(color))
        << std::setw(2) << static_cast<int>(GetGValue(color))
        << std::setw(2) << static_cast<int>(GetBValue(color));
    return oss.str();
}

static COLORREF HexToColor(const std::string& hex, COLORREF defaultColor) {
    if (hex.size() < 7 || hex[0] != '#') return defaultColor;
    try {
        int r = std::stoi(hex.substr(1, 2), nullptr, 16);
        int g = std::stoi(hex.substr(3, 2), nullptr, 16);
        int b = std::stoi(hex.substr(5, 2), nullptr, 16);
        return RGB(r, g, b);
    } catch (...) {
        return defaultColor;
    }
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

static std::string JoinMetricOrder(const std::vector<std::string>& order) {
    std::ostringstream oss;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << order[i];
    }
    return oss.str();
}

static bool ReadIniBool(const char* section, const char* key, bool defaultVal, const std::string& iniPath) {
    char buf[64] = {0};
    DWORD ret = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath.c_str());
    if (ret == 0 || buf[0] == '\0') {
        return defaultVal;
    }
    std::string val(buf);
    size_t first = val.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return defaultVal;
    size_t last = val.find_last_not_of(" \t\r\n");
    val = val.substr(first, last - first + 1);

    if (val == "0" || _stricmp(val.c_str(), "false") == 0 || _stricmp(val.c_str(), "off") == 0 || _stricmp(val.c_str(), "no") == 0) {
        return false;
    }
    if (val == "1" || _stricmp(val.c_str(), "true") == 0 || _stricmp(val.c_str(), "on") == 0 || _stricmp(val.c_str(), "yes") == 0) {
        return true;
    }
    return defaultVal;
}

bool ConfigManager::CreateDefaultConfigFile(const std::string& iniPath, const AppConfig& config) {
    std::ostringstream oss;
    oss << "# BareStats Configuration\n\n"
        << "[BareStats]\n"
        << "# Configuration schema version. Used for automated zero-overhead in-place migrations.\n"
        << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n\n"
        << "[Position]\n"
        << "# Selected monitor index (0 is primary)\n"
        << "monitor_index = " << config.monitorIndex << "\n\n"
        << "# Preset positions:\n"
        << "# top-left, top-center, top-right\n"
        << "# center-left, center, center-right\n"
        << "# bottom-left, bottom-center, bottom-right\n"
        << "# custom\n"
        << "preset = " << MonitorHelper::PresetToString(config.preset) << "\n\n"
        << "# Custom coordinates (used when preset = custom or after Ctrl+Shift+M reposition)\n"
        << "custom_x = " << config.customX << "\n"
        << "custom_y = " << config.customY << "\n"
        << "# Monitor-relative coordinates for multi-monitor / resolution stability\n"
        << "custom_rel_x = " << config.customRelX << "\n"
        << "custom_rel_y = " << config.customRelY << "\n\n"
        << "# Edge margin in pixels\n"
        << "margin = " << config.margin << "\n\n"
        << "# Layout geometry:\n"
        << "#   horizontal (default) = Single-line status ribbon\n"
        << "#   grid                 = Compact multi-row widget (auto-wrapped)\n"
        << "#   custom_grid          = User-defined mathematical matrix grid\n"
        << "layout = " << (config.layout == OverlayLayout::CustomGrid ? "custom_grid" : (config.layout == OverlayLayout::Grid ? "grid" : "horizontal")) << "\n\n"
        << "# Grid engine mode: unified (default single compact window) or islands (distributed micro-HWNDs for large gaps & cross-monitor placement)\n"
        << "grid_mode = " << (config.gridMode == GridMode::Islands ? "islands" : "unified") << "\n\n"
        << "# Column gap and row gap in pixels for CustomGrid (default: cell_gap = 16, row_gap = 4)\n"
        << "cell_gap = " << config.cellGap << "\n"
        << "row_gap = " << config.rowGap << "\n\n"
        << "# Maximum GPUs per row when layout is grid or multi-GPU row wrapping is active (default: 2)\n"
        << "max_gpus_per_row = " << config.maxGpusPerRow << "\n\n"
        << "# Custom Matrix Grid rows (used when layout = custom_grid):\n"
        << "# Use '.' or 'null' for empty slots. Repeat tokens on a row to merge columns (colspan).\n"
        << "# Example:\n"
        << "#   grid_row_1 = cpu  | .    | .    | ram\n"
        << "#   grid_row_2 = .    | .    | gpu0 | gpu0\n"
        << "#   grid_row_3 = gpu1 | gpu1 | gpu1 | gpu1\n"
        << "#   grid_row_4 = .    | net  | net  | net\n\n"
        << "[Appearance]\n"
        << "# Font face family (auto inherits Windows System UI font like Segoe UI)\n"
        << "font_name = auto\n"
        << "# Font weight: semibold (default, matches Windows Taskbar & NVIDIA overlay), regular, bold\n"
        << "font_weight = " << (config.fontWeight == L"bold" ? "bold" : (config.fontWeight == L"regular" ? "regular" : "semibold")) << "\n"
        << "# Font size in pixels (13 recommended; 0 = auto-detect from Windows system metrics)\n"
        << "font_size = " << config.fontSize << "\n"
        << "# Data & units text color in hex (#RRGGBB) - bright crisp white\n"
        << "text_color = " << ColorToHex(config.textColor) << "\n"
        << "# Category label text color (CPU, GPU, RAM) - soft muted silver\n"
        << "label_color = " << ColorToHex(config.labelColor) << "\n"
        << "# Divider separator color - quiet subtle gray\n"
        << "separator_color = " << ColorToHex(config.separatorColor) << "\n"
        << "# Threshold alert colors\n"
        << "color_high = " << ColorToHex(config.colorHigh) << "\n"
        << "color_critical = " << ColorToHex(config.colorCritical) << "\n"
        << "enable_dynamic_colors = " << (config.enableDynamicColors ? "1" : "0") << "\n"
        << "# Subtle dark drop-shadow for contrast over white windows (1 = enabled, 0 = disabled)\n"
        << "drop_shadow = " << (config.dropShadow ? "1" : "0") << "\n"
        << "# Telemetry category divider symbol (e.g. │, |, •, /, -, ::)\n"
        << "separator = " << WideToUtf8(config.separator) << "\n"
        << "# Global overlay window opacity percentage (10 to 100)\n"
        << "opacity = " << config.opacity << "\n\n"
        << "[Telemetry]\n"
        << "# Polling refresh interval in milliseconds (min: 100, max: 60000, default: 1000)\n"
        << "refresh_interval_ms = " << config.refreshIntervalMs << "\n\n";

    // Dynamic Hardware Introspection: Probe physical GPUs present via DXGI
    auto detectedGpus = GpuCollector::GetAvailableGpus();
    oss << "# ----------------------------------------------------------------------------\n"
        << "# Hardware Detected: " << detectedGpus.size() << " physical GPU(s) available\n";
    if (detectedGpus.empty()) {
        oss << "#   (Universal Windows D3DKMT / DXGI fallback active)\n";
    } else {
        for (const auto& d : detectedGpus) {
            char vBuf[16];
            snprintf(vBuf, sizeof(vBuf), "0x%04X", d.vendorId);
            oss << "#   [" << d.index << "] " << d.name << " (Vendor: " << vBuf << ", Dedicated VRAM: "
                << (d.dedicatedVideoMemory / (1024 * 1024)) << " MB)\n";
        }
    }
    oss << "# ----------------------------------------------------------------------------\n"
        << "# Selected GPU target index: single (0 = primary display), multi (0, 1), or all (all)\n"
        << "# Examples:\n"
        << "#   gpu_index = 0          -> Monitor primary display GPU only\n"
        << "#   gpu_index = 0, 1       -> Monitor GPU0 and GPU1 concurrently\n"
        << "#   gpu_index = all        -> Monitor all discovered physical GPUs simultaneously\n"
        << "gpu_index = " << (config.gpuTarget.empty() ? std::to_string(config.gpuIndex) : config.gpuTarget) << "\n\n"
        << "# VRAM capacity reporting mode:\n"
        << "#   physical = Gross hardware silicon capacity (e.g. 16.0 GB, matches retail box & Task Manager)\n"
        << "#   usable   = Net allocatable capacity excluding driver reservations (e.g. 15.6 GB, AI/CUDA OOM guard)\n"
        << "vram_mode = " << (config.vramMode == VramMode::Usable ? "usable" : "physical") << "\n\n"
        << "# Metric segment display ordering (comma-separated: CPU, GPU, RAM)\n"
        << "order = " << JoinMetricOrder(config.metricOrder) << "\n\n"
        << "# Toggle specific metrics (1 = show, 0 = hide)\n"
        << "show_cpu = " << (config.showCpu ? "1" : "0") << "\n"
        << "show_gpu = " << (config.showGpu ? "1" : "0") << "\n"
        << "show_ram = " << (config.showRam ? "1" : "0") << "\n"
        << "show_net = " << (config.showNet ? "1" : "0") << "\n"
        << "# GPU sensor toggles (NVIDIA NVML, AMD ADL, Intel IGCL with universal DXGI & D3DKMT fallback)\n"
        << "show_gpu_temp = " << (config.showGpuTemp ? "1" : "0") << "\n"
        << "show_gpu_power = " << (config.showGpuPower ? "1" : "0") << "\n"
        << "show_gpu_fan = " << (config.showGpuFan ? "1" : "0") << "\n"
        << "# Show max memory total (1 = 11.7/16GB, 0 = 11.7GB current only)\n"
        << "show_max_memory = " << (config.showMaxMemory ? "1" : "0") << "\n"
        << "# Automatically hide offline/unavailable metrics after launch (1 = hide, 0 = show N/A)\n"
        << "hide_offline_metrics = " << (config.hideOfflineMetrics ? "1" : "0") << "\n\n"
        << "# ----------------------------------------------------------------------------\n"
        << "# Optional Per-GPU Overrides (uncomment and configure per adapter index)\n"
        << "# Allows asymmetric telemetry (e.g. detailed graphics sensors on primary card,\n"
        << "# compact VRAM-only monitoring on background compute/inference card)\n"
        << "# ----------------------------------------------------------------------------\n"
        << "# [GPU0]\n"
        << "# show_temp = 1\n"
        << "# show_power = 1\n"
        << "# show_fan = 1\n"
        << "# label = Primary\n"
        << "#\n"
        << "# [GPU1]\n"
        << "# show_temp = 1\n"
        << "# show_power = 0\n"
        << "# show_fan = 0\n"
        << "# label = Compute\n";

    HANDLE hFile = CreateFileA(iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string content = oss.str();
        DWORD written = 0;
        WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(hFile);
        return true;
    }
    return false;
}

std::string ConfigManager::GetDefaultConfigPath() {
    // 1. Check for Portable Mode (config.ini right next to the executable)
    char exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string path = exePath;
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            std::string exeDir = path.substr(0, lastSlash + 1);
            std::string localIni = exeDir + "config.ini";

            // If config.ini exists next to the .exe, test if it is writable
            DWORD attrib = GetFileAttributesA(localIni.c_str());
            if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
                HANDLE hFile = CreateFileA(localIni.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    CloseHandle(hFile);
                    return localIni; // Portable mode confirmed
                }
            } else {
                // Test if the directory is writable
                std::string testFile = exeDir + ".__write_test.tmp";
                HANDLE hTest = CreateFileA(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (hTest != INVALID_HANDLE_VALUE) {
                    CloseHandle(hTest);
                    return localIni; // Portable directory writable
                }
            }
        }
    }

    // 2. Fallback to %LOCALAPPDATA%\BareStats\config.ini (Installed / Protected Mode)
    char localAppData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::string appDir = std::string(localAppData) + "\\BareStats";
        CreateDirectoryA(appDir.c_str(), nullptr);
        return appDir + "\\config.ini";
    }

    return "config.ini";
}

bool ConfigManager::MigrateSchema(const std::string& iniPath, int /*oldVersion*/, int /*newVersion*/) {
    // 1. Create safety backup (.bak) before modifying
    std::string bakPath = iniPath + ".bak";
    CopyFileA(iniPath.c_str(), bakPath.c_str(), FALSE);

    // 2. Read entire file line-by-line
    std::ifstream inFile(iniPath);
    if (!inFile.is_open()) {
        return false;
    }
    std::vector<std::string> lines;
    std::string rawLine;
    while (std::getline(inFile, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }
        lines.push_back(rawLine);
    }
    inFile.close();

    auto trim = [](const std::string& s) -> std::string {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };

    // 3. Inventory existing sections and keys
    std::unordered_map<std::string, std::unordered_set<std::string>> existingKeys;
    std::unordered_set<std::string> existingSections;
    bool hasGpuSectionOrComment = false;
    std::string currentSec;

    for (const auto& l : lines) {
        std::string t = trim(l);
        if (t.find("[GPU0]") != std::string::npos || t.find("# [GPU0]") != std::string::npos) {
            hasGpuSectionOrComment = true;
        }
        if (t.empty()) continue;
        if (t.front() == '[' && t.back() == ']') {
            currentSec = t.substr(1, t.size() - 2);
            for (char& c : currentSec) c = static_cast<char>(tolower(c));
            existingSections.insert(currentSec);
        } else if (!currentSec.empty() && t.front() != '#' && t.front() != ';') {
            size_t eqPos = t.find('=');
            if (eqPos != std::string::npos) {
                std::string key = trim(t.substr(0, eqPos));
                for (char& c : key) c = static_cast<char>(tolower(c));
                existingKeys[currentSec].insert(key);
            }
        }
    }

    // Determine missing keys per section
    bool needBareStatsSec = (existingSections.count("barestats") == 0);
    bool needLayout = (existingKeys["position"].count("layout") == 0);
    bool needMaxGpusPerRow = (existingKeys["position"].count("max_gpus_per_row") == 0);
    bool needVramMode = (existingKeys["telemetry"].count("vram_mode") == 0);
    bool needOrder = (existingKeys["telemetry"].count("order") == 0);
    bool needShowNet = (existingKeys["telemetry"].count("show_net") == 0);
    bool needGpuOverrides = !hasGpuSectionOrComment;

    std::ostringstream out;

    // If [BareStats] section is completely missing, insert at the top
    if (needBareStatsSec) {
        out << "[BareStats]\n"
            << "# Configuration schema version. Used for automated zero-overhead in-place migrations.\n"
            << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n\n";
    }

    currentSec = "";
    auto injectMissingForSec = [&](const std::string& sec) {
        if (sec == "position") {
            if (needLayout) {
                out << "# Layout geometry: horizontal (single-line status ribbon) or grid (compact multi-row widget)\n"
                    << "layout = horizontal\n\n";
                needLayout = false;
            }
            if (needMaxGpusPerRow) {
                out << "# Maximum GPUs per row when layout is grid or multi-GPU row wrapping is active (default: 2)\n"
                    << "max_gpus_per_row = 2\n\n";
                needMaxGpusPerRow = false;
            }
        } else if (sec == "telemetry") {
            if (needVramMode) {
                out << "# VRAM capacity calculation mode: physical (default) or usable (AI/CUDA net limit)\n"
                    << "vram_mode = physical\n\n";
                needVramMode = false;
            }
            if (needOrder) {
                out << "# Telemetry category display ordering sequence\n"
                    << "order = CPU, GPU, RAM, NET\n\n";
                needOrder = false;
            }
            if (needShowNet) {
                out << "# Network throughput telemetry toggle (1 = enabled, 0 = disabled)\n"
                    << "show_net = 0\n\n";
                needShowNet = false;
            }
            if (needGpuOverrides) {
                out << "# ----------------------------------------------------------------------------\n"
                    << "# Optional Per-GPU Overrides (uncomment and configure per adapter index)\n"
                    << "# Allows asymmetric telemetry (e.g. detailed graphics sensors on primary card,\n"
                    << "# compact VRAM-only monitoring on background compute/inference card)\n"
                    << "# ----------------------------------------------------------------------------\n"
                    << "# [GPU0]\n"
                    << "# show_temp = 1\n"
                    << "# show_power = 1\n"
                    << "# show_fan = 1\n"
                    << "# label = Primary\n"
                    << "#\n"
                    << "# [GPU1]\n"
                    << "# show_temp = 1\n"
                    << "# show_power = 0\n"
                    << "# show_fan = 0\n"
                    << "# label = Compute\n\n";
                needGpuOverrides = false;
            }
        }
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string t = trim(line);

        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            // Before starting a new section, inject missing keys for previous section
            if (!currentSec.empty()) {
                injectMissingForSec(currentSec);
            }
            currentSec = t.substr(1, t.size() - 2);
            for (char& c : currentSec) c = static_cast<char>(tolower(c));
        }

        // If inside [BareStats] and line is schema_version, update it to CURRENT_CONFIG_SCHEMA_VERSION
        if (currentSec == "barestats" && t.find("schema_version") == 0) {
            out << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n";
            continue;
        }

        out << line << "\n";
    }

    // Check end of file for currentSec injections
    if (!currentSec.empty()) {
        injectMissingForSec(currentSec);
    }

    std::string newContent = out.str();
    HANDLE hFile = CreateFileA(iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, newContent.data(), static_cast<DWORD>(newContent.size()), &written, nullptr);
        CloseHandle(hFile);
        return true;
    }
    return false;
}

AppConfig ConfigManager::Load(const std::string& iniPath) {
    AppConfig config; // Built-in binary fallback defaults

    DWORD attrib = GetFileAttributesA(iniPath.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        // Missing config file! Auto-generate with all defaults and return built-in fallback.
        CreateDefaultConfigFile(iniPath, config);
        return config;
    }

    // Fast Single-Call Schema Check (~2 microseconds)
    int diskVersion = GetPrivateProfileIntA("BareStats", "schema_version", 0, iniPath.c_str());
    if (diskVersion < CURRENT_CONFIG_SCHEMA_VERSION) {
        MigrateSchema(iniPath, diskVersion, CURRENT_CONFIG_SCHEMA_VERSION);
    }

    char buf[256] = {0};

    // [Position]
    config.monitorIndex = GetPrivateProfileIntA("Position", "monitor_index", config.monitorIndex, iniPath.c_str());
    
    GetPrivateProfileStringA("Position", "preset", MonitorHelper::PresetToString(config.preset).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.preset = MonitorHelper::StringToPreset(buf);

    config.customX = GetPrivateProfileIntA("Position", "custom_x", config.customX, iniPath.c_str());
    config.customY = GetPrivateProfileIntA("Position", "custom_y", config.customY, iniPath.c_str());
    config.customRelX = GetPrivateProfileIntA("Position", "custom_rel_x", config.customRelX, iniPath.c_str());
    config.customRelY = GetPrivateProfileIntA("Position", "custom_rel_y", config.customRelY, iniPath.c_str());
    config.margin = GetPrivateProfileIntA("Position", "margin", config.margin, iniPath.c_str());

    char layoutBuf[64] = {0};
    GetPrivateProfileStringA("Position", "layout", "horizontal", layoutBuf, sizeof(layoutBuf), iniPath.c_str());
    std::string layoutStr(layoutBuf);
    for (char& c : layoutStr) c = static_cast<char>(tolower(c));
    if (layoutStr == "grid") {
        config.layout = OverlayLayout::Grid;
    } else if (layoutStr == "custom_grid" || layoutStr == "matrix" || layoutStr == "custom") {
        config.layout = OverlayLayout::CustomGrid;
    } else {
        config.layout = OverlayLayout::Horizontal;
    }

    config.maxGpusPerRow = GetPrivateProfileIntA("Position", "max_gpus_per_row", config.maxGpusPerRow, iniPath.c_str());
    if (config.maxGpusPerRow < 1) config.maxGpusPerRow = 1;

    char gridModeBuf[32] = {0};
    GetPrivateProfileStringA("Position", "grid_mode", "unified", gridModeBuf, sizeof(gridModeBuf), iniPath.c_str());
    std::string gridModeStr(gridModeBuf);
    for (char& c : gridModeStr) c = static_cast<char>(tolower(c));
    config.gridMode = (gridModeStr == "islands" || gridModeStr == "split" || gridModeStr == "distributed") 
        ? GridMode::Islands 
        : GridMode::Unified;

    config.cellGap = GetPrivateProfileIntA("Position", "cell_gap", config.cellGap, iniPath.c_str());
    if (config.cellGap < 0) config.cellGap = 0;
    config.rowGap = GetPrivateProfileIntA("Position", "row_gap", config.rowGap, iniPath.c_str());
    if (config.rowGap < 0) config.rowGap = 0;

    // Parse Custom Matrix Grid rows
    int maxColsFound = 0;
    for (int r = 1; r <= 32; ++r) {
        char rowKey[32] = {0};
        sprintf_s(rowKey, "grid_row_%d", r);
        char rowBuf[512] = {0};
        GetPrivateProfileStringA("Position", rowKey, "", rowBuf, sizeof(rowBuf), iniPath.c_str());
        if (rowBuf[0] == '\0') {
            GetPrivateProfileStringA("Grid", rowKey, "", rowBuf, sizeof(rowBuf), iniPath.c_str());
        }
        if (rowBuf[0] == '\0') {
            sprintf_s(rowKey, "row_%d", r);
            GetPrivateProfileStringA("Position", rowKey, "", rowBuf, sizeof(rowBuf), iniPath.c_str());
            if (rowBuf[0] == '\0') {
                GetPrivateProfileStringA("Grid", rowKey, "", rowBuf, sizeof(rowBuf), iniPath.c_str());
            }
        }
        if (rowBuf[0] == '\0') continue;

        std::string lineStr(rowBuf);
        std::vector<std::string> rawTokens;
        std::string curTok;
        for (char ch : lineStr) {
            if (ch == '|' || ch == ',') {
                rawTokens.push_back(curTok);
                curTok.clear();
            } else {
                curTok += ch;
            }
        }
        rawTokens.push_back(curTok);

        GridRow gridRow;
        int nextCol = 0;

        for (const auto& rawTok : rawTokens) {
            size_t f = rawTok.find_first_not_of(" \t\r\n");
            if (f == std::string::npos) {
                gridRow.cells.push_back({ "", nextCol, 1 });
                nextCol++;
                continue;
            }
            size_t l = rawTok.find_last_not_of(" \t\r\n");
            std::string tok = rawTok.substr(f, l - f + 1);
            for (char& c : tok) c = static_cast<char>(tolower(c));

            int span = 1;
            size_t colonPos = tok.find(':');
            if (colonPos != std::string::npos) {
                std::string spanPart = tok.substr(colonPos + 1);
                tok = tok.substr(0, colonPos);
                size_t numStart = spanPart.find_first_of("0123456789");
                if (numStart != std::string::npos) {
                    try {
                        span = std::stoi(spanPart.substr(numStart));
                    } catch (...) {
                        span = 1;
                    }
                    if (span < 1) span = 1;
                }
            }

            if (tok == "." || tok == "null" || tok == "empty" || tok == "-") {
                tok = "";
            }

            // Merge identical non-empty adjacent tokens into colspan
            if (!tok.empty() && !gridRow.cells.empty() && gridRow.cells.back().token == tok) {
                gridRow.cells.back().colSpan += span;
                nextCol += span;
            } else {
                gridRow.cells.push_back({ tok, nextCol, span });
                nextCol += span;
            }
        }

        if (nextCol > maxColsFound) {
            maxColsFound = nextCol;
        }
        config.customGrid.rows.push_back(std::move(gridRow));
    }

    config.customGrid.numCols = maxColsFound;
    config.customGrid.numRows = static_cast<int>(config.customGrid.rows.size());

    // Fallback if CustomGrid is chosen without explicit rows or if grid_mode = islands is active
    if ((config.layout == OverlayLayout::CustomGrid || config.gridMode == GridMode::Islands) && config.customGrid.IsEmpty()) {
        GridRow r0;
        r0.cells.push_back({ "cpu", 0, 1 });
        r0.cells.push_back({ "ram", 1, 1 });
        r0.cells.push_back({ "net", 2, 1 });
        GridRow r1;
        r1.cells.push_back({ "gpu", 0, 3 });
        config.customGrid.rows.push_back(std::move(r0));
        config.customGrid.rows.push_back(std::move(r1));
        config.customGrid.numCols = 3;
        config.customGrid.numRows = 2;
    }
    if (config.gridMode == GridMode::Islands) {
        config.layout = OverlayLayout::CustomGrid;
    }

    // [Appearance]
    GetPrivateProfileStringA("Appearance", "font_name", "auto", buf, sizeof(buf), iniPath.c_str());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wfont(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, &wfont[0], wlen);
        config.fontFace = wfont;
    }

    GetPrivateProfileStringA("Appearance", "font_weight", "semibold", buf, sizeof(buf), iniPath.c_str());
    int wlenWeight = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (wlenWeight > 0) {
        std::wstring wweight(wlenWeight - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, &wweight[0], wlenWeight);
        config.fontWeight = wweight;
    }

    config.fontSize = GetPrivateProfileIntA("Appearance", "font_size", config.fontSize, iniPath.c_str());
    if (config.fontSize <= 0) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(NONCLIENTMETRICSW);
        int basePt = 13;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0)) {
            int h = std::abs(ncm.lfMessageFont.lfHeight);
            if (h > 0) {
                basePt = h;
            }
        }
        DWORD textScale = 100;
        DWORD dataSize = sizeof(textScale);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &textScale, &dataSize);
        if (textScale > 100) {
            config.fontSize = static_cast<int>(std::round(basePt * (textScale / 100.0)));
        } else {
            config.fontSize = basePt;
        }
    }

    GetPrivateProfileStringA("Appearance", "text_color", ColorToHex(config.textColor).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.textColor = HexToColor(buf, config.textColor);

    GetPrivateProfileStringA("Appearance", "label_color", ColorToHex(config.labelColor).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.labelColor = HexToColor(buf, config.labelColor);

    GetPrivateProfileStringA("Appearance", "separator_color", ColorToHex(config.separatorColor).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.separatorColor = HexToColor(buf, config.separatorColor);

    GetPrivateProfileStringA("Appearance", "color_high", ColorToHex(config.colorHigh).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.colorHigh = HexToColor(buf, config.colorHigh);

    GetPrivateProfileStringA("Appearance", "color_critical", ColorToHex(config.colorCritical).c_str(), buf, sizeof(buf), iniPath.c_str());
    config.colorCritical = HexToColor(buf, config.colorCritical);

    config.enableDynamicColors = ReadIniBool("Appearance", "enable_dynamic_colors", config.enableDynamicColors, iniPath);
    config.dropShadow = ReadIniBool("Appearance", "drop_shadow", config.dropShadow, iniPath);

    // Separator
    char sepBuf[64] = {0};
    GetPrivateProfileStringA("Appearance", "separator", "│", sepBuf, sizeof(sepBuf), iniPath.c_str());
    std::wstring wsep = Utf8ToWide(sepBuf);
    if (!wsep.empty()) {
        config.separator = wsep;
    }

    // Opacity
    char opBuf[32] = {0};
    GetPrivateProfileStringA("Appearance", "opacity", "100", opBuf, sizeof(opBuf), iniPath.c_str());
    std::string opStr(opBuf);
    size_t pctPos = opStr.find('%');
    if (pctPos != std::string::npos) opStr = opStr.substr(0, pctPos);
    try {
        double parsed = std::stod(opStr);
        if (parsed > 0.0 && parsed <= 1.0) {
            config.opacity = static_cast<int>(std::round(parsed * 100.0));
        } else {
            config.opacity = static_cast<int>(std::round(parsed));
        }
    } catch (...) {
        config.opacity = 100;
    }
    config.opacity = std::clamp(config.opacity, 10, 100);

    // [Telemetry]
    config.refreshIntervalMs = static_cast<uint32_t>(GetPrivateProfileIntA("Telemetry", "refresh_interval_ms", config.refreshIntervalMs, iniPath.c_str()));
    if (config.refreshIntervalMs < 100 || config.refreshIntervalMs > 60000) {
        if (config.refreshIntervalMs < 100) config.refreshIntervalMs = 1000;
        else if (config.refreshIntervalMs > 60000) config.refreshIntervalMs = 60000;
    }

    char gpuTargetBuf[64] = {0};
    GetPrivateProfileStringA("Telemetry", "gpu_index", "0", gpuTargetBuf, sizeof(gpuTargetBuf), iniPath.c_str());
    std::string gpuTargetStr(gpuTargetBuf);
    size_t gFirst = gpuTargetStr.find_first_not_of(" \t\r\n");
    size_t gLast = gpuTargetStr.find_last_not_of(" \t\r\n");
    if (gFirst != std::string::npos && gLast != std::string::npos) {
        gpuTargetStr = gpuTargetStr.substr(gFirst, gLast - gFirst + 1);
    }
    config.gpuTarget = gpuTargetStr;

    std::string lowerTarget = gpuTargetStr;
    for (char& c : lowerTarget) c = static_cast<char>(tolower(c));

    config.targetGpuIndices.clear();
    if (lowerTarget == "all") {
        config.targetAllGpus = true;
        config.gpuIndex = 0;
    } else {
        config.targetAllGpus = false;
        std::stringstream ssGpu(gpuTargetStr);
        std::string token;
        while (std::getline(ssGpu, token, ',')) {
            size_t tFirst = token.find_first_not_of(" \t\r\n");
            size_t tLast = token.find_last_not_of(" \t\r\n");
            if (tFirst != std::string::npos && tLast != std::string::npos) {
                try {
                    int idx = std::stoi(token.substr(tFirst, tLast - tFirst + 1));
                    if (idx >= 0) {
                        config.targetGpuIndices.push_back(static_cast<uint32_t>(idx));
                    }
                } catch (...) {}
            }
        }
        if (config.targetGpuIndices.empty()) {
            config.targetGpuIndices.push_back(0);
        }
        config.gpuIndex = static_cast<int>(config.targetGpuIndices[0]);
    }

    // Dynamic CustomGrid GPU auto-targeting:
    // If the CustomGrid matrix specifies a secondary or specific GPU adapter index (e.g. gpu1, gpu1_vram) or "gpu" token,
    // automatically ensure the telemetry collector targets all GPUs so adapter ordering and telemetry are guaranteed.
    if (config.layout == OverlayLayout::CustomGrid || config.gridMode == GridMode::Islands) {
        bool hasGpuToken = false;
        for (const auto& row : config.customGrid.rows) {
            for (const auto& cell : row.cells) {
                if (cell.token.rfind("gpu", 0) == 0) {
                    hasGpuToken = true;
                    break;
                }
            }
            if (hasGpuToken) break;
        }
        if (hasGpuToken && !config.targetAllGpus) {
            config.targetAllGpus = true;
        }
    }

    char vramBuf[32] = {0};
    GetPrivateProfileStringA("Telemetry", "vram_mode", "physical", vramBuf, sizeof(vramBuf), iniPath.c_str());
    std::string vramStr(vramBuf);
    for (char& c : vramStr) c = static_cast<char>(tolower(c));
    if (vramStr == "usable") {
        config.vramMode = VramMode::Usable;
    } else {
        config.vramMode = VramMode::Physical;
    }

    // Metric ordering
    char orderBuf[128] = {0};
    GetPrivateProfileStringA("Telemetry", "order", "CPU, GPU, RAM, NET", orderBuf, sizeof(orderBuf), iniPath.c_str());
    std::string orderStr(orderBuf);
    std::vector<std::string> parsedOrder;
    std::stringstream ss(orderStr);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t first = token.find_first_not_of(" \t\r\n");
        size_t last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            std::string trimmed = token.substr(first, last - first + 1);
            for (char& c : trimmed) c = static_cast<char>(toupper(c));
            if (trimmed == "CPU" || trimmed == "GPU" || trimmed == "RAM" || trimmed == "NET") {
                if (std::find(parsedOrder.begin(), parsedOrder.end(), trimmed) == parsedOrder.end()) {
                    parsedOrder.push_back(trimmed);
                }
            }
        }
    }
    if (!parsedOrder.empty()) {
        config.metricOrder = parsedOrder;
    }

    config.showCpu = ReadIniBool("Telemetry", "show_cpu", config.showCpu, iniPath);
    config.showGpu = ReadIniBool("Telemetry", "show_gpu", config.showGpu, iniPath);
    config.showRam = ReadIniBool("Telemetry", "show_ram", config.showRam, iniPath);
    config.showNet = ReadIniBool("Telemetry", "show_net", config.showNet, iniPath);
    config.showGpuTemp = ReadIniBool("Telemetry", "show_gpu_temp", config.showGpuTemp, iniPath);
    config.showGpuPower = ReadIniBool("Telemetry", "show_gpu_power", config.showGpuPower, iniPath);
    config.showGpuFan = ReadIniBool("Telemetry", "show_gpu_fan", config.showGpuFan, iniPath);
    config.showMaxMemory = ReadIniBool("Telemetry", "show_max_memory", config.showMaxMemory, iniPath);
    config.hideOfflineMetrics = ReadIniBool("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics, iniPath);

    // [GPU0] .. [GPU15] Per-Adapter Hierarchical Configuration Overrides
    config.perGpuConfigs.clear();
    for (uint32_t i = 0; i < 16; ++i) {
        std::string section = "GPU" + std::to_string(i);
        char testBuf[64] = {0};
        if (GetPrivateProfileStringA(section.c_str(), nullptr, "", testBuf, sizeof(testBuf), iniPath.c_str()) > 0) {
            PerGpuConfig gpuCfg;
            gpuCfg.enabled = ReadIniBool(section.c_str(), "enabled", true, iniPath);
            gpuCfg.showUtil = ReadIniBool(section.c_str(), "show_util", true, iniPath);
            gpuCfg.showVram = ReadIniBool(section.c_str(), "show_vram", true, iniPath);
            gpuCfg.showTemp = ReadIniBool(section.c_str(), "show_temp", config.showGpuTemp, iniPath);
            gpuCfg.showPower = ReadIniBool(section.c_str(), "show_power", config.showGpuPower, iniPath);
            gpuCfg.showFan = ReadIniBool(section.c_str(), "show_fan", config.showGpuFan, iniPath);

            char lblBuf[64] = {0};
            GetPrivateProfileStringA(section.c_str(), "label", "", lblBuf, sizeof(lblBuf), iniPath.c_str());
            gpuCfg.label = lblBuf;

            config.perGpuConfigs[i] = gpuCfg;
        }
    }

    return config;
}

bool ConfigManager::Save(const std::string& iniPath, const AppConfig& config) {
    DWORD attrib = GetFileAttributesA(iniPath.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return CreateDefaultConfigFile(iniPath, config);
    }

    WritePrivateProfileStringA("Position", "monitor_index", std::to_string(config.monitorIndex).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "preset", MonitorHelper::PresetToString(config.preset).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_x", std::to_string(config.customX).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_y", std::to_string(config.customY).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_rel_x", std::to_string(config.customRelX).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_rel_y", std::to_string(config.customRelY).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "margin", std::to_string(config.margin).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "layout", config.layout == OverlayLayout::CustomGrid ? "custom_grid" : (config.layout == OverlayLayout::Grid ? "grid" : "horizontal"), iniPath.c_str());
    WritePrivateProfileStringA("Position", "grid_mode", config.gridMode == GridMode::Islands ? "islands" : "unified", iniPath.c_str());
    WritePrivateProfileStringA("Position", "cell_gap", std::to_string(config.cellGap).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "row_gap", std::to_string(config.rowGap).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "max_gpus_per_row", std::to_string(config.maxGpusPerRow).c_str(), iniPath.c_str());

    // Convert fontFace to UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, 0, config.fontFace.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulen > 0) {
        std::string ufont(ulen - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, config.fontFace.c_str(), -1, &ufont[0], ulen, nullptr, nullptr);
        WritePrivateProfileStringA("Appearance", "font_name", ufont.c_str(), iniPath.c_str());
    }

    int ulenWeight = WideCharToMultiByte(CP_UTF8, 0, config.fontWeight.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulenWeight > 0) {
        std::string uweight(ulenWeight - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, config.fontWeight.c_str(), -1, &uweight[0], ulenWeight, nullptr, nullptr);
        WritePrivateProfileStringA("Appearance", "font_weight", uweight.c_str(), iniPath.c_str());
    }

    WritePrivateProfileStringA("Appearance", "font_size", std::to_string(config.fontSize).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "text_color", ColorToHex(config.textColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "label_color", ColorToHex(config.labelColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "separator_color", ColorToHex(config.separatorColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "color_high", ColorToHex(config.colorHigh).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "color_critical", ColorToHex(config.colorCritical).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "enable_dynamic_colors", config.enableDynamicColors ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "drop_shadow", config.dropShadow ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "separator", WideToUtf8(config.separator).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "opacity", std::to_string(config.opacity).c_str(), iniPath.c_str());

    WritePrivateProfileStringA("Telemetry", "refresh_interval_ms", std::to_string(config.refreshIntervalMs).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "gpu_index", config.gpuTarget.empty() ? std::to_string(config.gpuIndex).c_str() : config.gpuTarget.c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "vram_mode", config.vramMode == VramMode::Usable ? "usable" : "physical", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "order", JoinMetricOrder(config.metricOrder).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_cpu", config.showCpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu", config.showGpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_ram", config.showRam ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_net", config.showNet ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_temp", config.showGpuTemp ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_power", config.showGpuPower ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_fan", config.showGpuFan ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_max_memory", config.showMaxMemory ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics ? "1" : "0", iniPath.c_str());

    return true;
}

} // namespace BareStats
