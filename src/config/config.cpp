#include "config.h"
#include "ini_parser.h"
#include "../metrics/gpu_collector.h"
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <filesystem>
#include <cmath>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <cstdlib>
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#include <cstdlib>
#endif

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

#ifdef _WIN32
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
#else
static std::string WideToUtf8(const std::wstring& wstr) {
    std::string out;
    for (wchar_t wc : wstr) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string& str) {
    std::wstring out;
    size_t i = 0;
    while (i < str.size()) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80) {
            out.push_back(static_cast<wchar_t>(c));
            i++;
        } else if ((c >> 5) == 0x6 && i + 1 < str.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < str.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < str.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            i++;
        }
    }
    return out;
}
#endif

static std::string JoinMetricOrder(const std::vector<std::string>& order) {
    std::ostringstream oss;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << order[i];
    }
    return oss.str();
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
        << "# Dynamic color alerts on critical thresholds (1 = enabled, 0 = disabled)\n"
        << "enable_dynamic_colors = " << (config.enableDynamicColors ? "1" : "0") << "\n"
        << "# Text drop shadow for legibility over light backgrounds\n"
        << "drop_shadow = " << (config.dropShadow ? "1" : "0") << "\n"
        << "# Divider separator glyph (default: box drawings light vertical │)\n"
        << "separator = " << WideToUtf8(config.separator) << "\n"
        << "# Global overlay opacity percentage (10 to 100, default: 100)\n"
        << "opacity = " << config.opacity << "\n\n"
        << "# Show icon in Linux system tray / top panel (1 = enabled, 0 = disabled)\n"
        << "# Set to 0 to keep the taskbar clean and use Dock/Desktop right-click instead.\n"
        << "show_tray_icon = " << (config.showTrayIcon ? "1" : "0") << "\n\n"
        << "[Telemetry]\n"
        << "# Telemetry polling and refresh interval in milliseconds (default: 1000)\n"
        << "refresh_interval_ms = " << config.refreshIntervalMs << "\n\n";

    // Dynamic Hardware Introspection: Probe physical GPUs present via GpuCollector
    auto detectedGpus = GpuCollector::GetAvailableGpus();
    oss << "# ----------------------------------------------------------------------------\n"
        << "# Hardware Detected: " << detectedGpus.size() << " physical GPU(s) available\n";
    if (detectedGpus.empty()) {
        oss << "#   (Universal fallback active)\n";
    } else {
        for (const auto& d : detectedGpus) {
            char vBuf[16];
            snprintf(vBuf, sizeof(vBuf), "0x%04X", d.vendorId);
            oss << "#   [" << d.index << "] " << d.name << " (Vendor: " << vBuf << ", Dedicated VRAM: "
                << (d.dedicatedVideoMemory / (1024 * 1024)) << " MB)\n";
        }
    }
    oss << "# ----------------------------------------------------------------------------\n\n"
        << "# Target GPU adapter index or multi-GPU target expression:\n"
        << "#   gpu_index = 0       (monitor dedicated GPU0)\n"
        << "#   gpu_index = 0, 1    (monitor GPU0 and GPU1 side-by-side)\n"
        << "#   gpu_index = all     (monitor all hardware GPU adapters)\n"
        << "gpu_index = " << (config.gpuTarget.empty() ? std::to_string(config.gpuIndex) : config.gpuTarget) << "\n"
        << "# VRAM capacity calculation mode: physical (default) or usable (AI/CUDA net limit)\n"
        << "vram_mode = " << (config.vramMode == VramMode::Usable ? "usable" : "physical") << "\n"
        << "# Telemetry category display ordering sequence\n"
        << "order = " << JoinMetricOrder(config.metricOrder) << "\n"
        << "# Individual metric category toggles (1 = enabled, 0 = disabled)\n"
        << "show_cpu = " << (config.showCpu ? "1" : "0") << "\n"
        << "show_gpu = " << (config.showGpu ? "1" : "0") << "\n"
        << "show_ram = " << (config.showRam ? "1" : "0") << "\n"
        << "show_net = " << (config.showNet ? "1" : "0") << "\n"
        << "show_gpu_temp = " << (config.showGpuTemp ? "1" : "0") << "\n"
        << "show_gpu_power = " << (config.showGpuPower ? "1" : "0") << "\n"
        << "show_gpu_fan = " << (config.showGpuFan ? "1" : "0") << "\n"
        << "show_max_memory = " << (config.showMaxMemory ? "1" : "0") << "\n"
        << "hide_offline_metrics = " << (config.hideOfflineMetrics ? "1" : "0") << "\n\n"
        << "# Per-GPU Hierarchical Configuration Overrides (optional)\n"
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

    std::ofstream file(iniPath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        std::string content = oss.str();
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        return file.good();
    }
    return false;
}

std::string ConfigManager::GetDefaultConfigPath() {
#ifdef _WIN32
    // 1. Check Current Working Directory (./config.ini)
    std::error_code cwdEc;
    if (std::filesystem::exists("config.ini", cwdEc)) {
        return "config.ini";
    }

    // 2. Check for Portable Mode (config.ini right next to the executable)
    char exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string path = exePath;
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            std::string exeDir = path.substr(0, lastSlash + 1);
            std::string localIni = exeDir + "config.ini";

            DWORD attrib = GetFileAttributesA(localIni.c_str());
            if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
                HANDLE hFile = CreateFileA(localIni.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    CloseHandle(hFile);
                    return localIni; // Portable mode confirmed
                }
            } else {
                std::string testFile = exeDir + ".__write_test.tmp";
                HANDLE hTest = CreateFileA(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (hTest != INVALID_HANDLE_VALUE) {
                    CloseHandle(hTest);
                    return localIni; // Portable directory writable
                }
            }
        }
    }

    // 3. Fallback to %LOCALAPPDATA%\BareStats\config.ini (Installed / Protected Mode)
    char localAppData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::string appDir = std::string(localAppData) + "\\BareStats";
        std::error_code ec;
        std::filesystem::create_directories(appDir, ec);
        return appDir + "\\config.ini";
    }

    return "config.ini";
#elif defined(__APPLE__)
    // macOS Implementation
    // 1. Check for Current Working Directory (./config.ini)
    std::error_code cwdEc;
    if (std::filesystem::exists("config.ini", cwdEc) && access("config.ini", R_OK) == 0) {
        return "config.ini";
    }

    // 2. Check for Portable Mode (config.ini right next to executable / inside bundle)
    char exePath[4096] = { 0 };
    uint32_t bufSize = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &bufSize) == 0) {
        std::string path(exePath);
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string exeDir = path.substr(0, lastSlash + 1);
            std::string localIni = exeDir + "config.ini";
            if (std::filesystem::exists(localIni) && access(localIni.c_str(), W_OK) == 0) {
                return localIni;
            }
        }
    }

    // 3. Native User Configuration: ~/Library/Application Support/BareStats/config.ini
    const char* envHome = std::getenv("HOME");
    if (envHome && envHome[0] != '\0') {
        std::string appSupportDir = std::string(envHome) + "/Library/Application Support/BareStats";
        std::string appSupportIni = appSupportDir + "/config.ini";
        if (std::filesystem::exists(appSupportIni)) {
            return appSupportIni;
        }

        // 4. Backward-compatible fallback check: ~/.config/barestats/config.ini
        std::string legacyIni = std::string(envHome) + "/.config/barestats/config.ini";
        if (std::filesystem::exists(legacyIni)) {
            return legacyIni;
        }

        // Default target: create ~/Library/Application Support/BareStats/config.ini
        std::error_code ec;
        std::filesystem::create_directories(appSupportDir, ec);
        return appSupportIni;
    }

    return "config.ini";
#else
    // Linux Implementation
    // 1. Check for Current Working Directory (./config.ini)
    std::error_code cwdEc;
    if (std::filesystem::exists("config.ini", cwdEc) && access("config.ini", R_OK) == 0) {
        return "config.ini";
    }

    // 2. Check for Portable Mode (config.ini right next to /proc/self/exe)
    char exePath[4096] = { 0 };
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string path(exePath);
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string exeDir = path.substr(0, lastSlash + 1);
            std::string localIni = exeDir + "config.ini";
            if (std::filesystem::exists(localIni) && access(localIni.c_str(), W_OK) == 0) {
                return localIni;
            }
        }
    }

    // 3. XDG Configuration Directory: $XDG_CONFIG_HOME/barestats/config.ini or ~/.config/barestats/config.ini
    std::string configHome;
    const char* envXdg = std::getenv("XDG_CONFIG_HOME");
    if (envXdg && envXdg[0] != '\0') {
        configHome = envXdg;
    } else {
        const char* envHome = std::getenv("HOME");
        if (envHome && envHome[0] != '\0') {
            configHome = std::string(envHome) + "/.config";
        }
    }

    if (!configHome.empty()) {
        std::string appDir = configHome + "/barestats";
        std::error_code ec;
        std::filesystem::create_directories(appDir, ec);
        return appDir + "/config.ini";
    }

    return "config.ini";
#endif
}

bool ConfigManager::MigrateSchema(const std::string& iniPath, int /*oldVersion*/, int /*newVersion*/) {
    std::string bakPath = iniPath + ".bak";
    std::error_code ec;
    std::filesystem::copy_file(iniPath, bakPath, std::filesystem::copy_options::overwrite_existing, ec);

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

    bool needBareStatsSec = (existingSections.count("barestats") == 0);
    bool needLayout = (existingKeys["position"].count("layout") == 0);
    bool needMaxGpusPerRow = (existingKeys["position"].count("max_gpus_per_row") == 0);
    bool needVramMode = (existingKeys["telemetry"].count("vram_mode") == 0);
    bool needOrder = (existingKeys["telemetry"].count("order") == 0);
    bool needShowNet = (existingKeys["telemetry"].count("show_net") == 0);
    bool needShowTrayIcon = (existingKeys["appearance"].count("show_tray_icon") == 0);
    bool needGpuOverrides = !hasGpuSectionOrComment;

    std::ostringstream out;

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
        } else if (sec == "appearance") {
            if (needShowTrayIcon) {
                out << "# Show icon in Linux system tray / top panel (1 = enabled, 0 = disabled)\n"
                    << "# Set to 0 to keep the taskbar clean and use Dock/Desktop right-click instead.\n"
                    << "show_tray_icon = 0\n\n";
                needShowTrayIcon = false;
            }
            if (needGpuOverrides) {
                out << "\n# Per-GPU Hierarchical Configuration Overrides (optional)\n"
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
            if (!currentSec.empty()) {
                injectMissingForSec(currentSec);
            }
            currentSec = t.substr(1, t.size() - 2);
            for (char& c : currentSec) c = static_cast<char>(tolower(c));
        }

        if (currentSec == "barestats" && t.find("schema_version") == 0) {
            out << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n";
            continue;
        }

        out << line << "\n";
    }

    if (!currentSec.empty()) {
        injectMissingForSec(currentSec);
    }

    std::string newContent = out.str();
    std::ofstream outFile(iniPath, std::ios::binary | std::ios::trunc);
    if (outFile.is_open()) {
        outFile.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
        return outFile.good();
    }
    return false;
}

AppConfig ConfigManager::Load(const std::string& iniPath) {
    AppConfig config;

    std::error_code ec;
    if (!std::filesystem::exists(iniPath, ec)) {
        CreateDefaultConfigFile(iniPath, config);
        return config;
    }

    IniDocument doc;
    if (!doc.Load(iniPath)) {
        CreateDefaultConfigFile(iniPath, config);
        return config;
    }

    int diskVersion = doc.GetInt("BareStats", "schema_version", 0);
    if (diskVersion < CURRENT_CONFIG_SCHEMA_VERSION) {
        MigrateSchema(iniPath, diskVersion, CURRENT_CONFIG_SCHEMA_VERSION);
        doc.Load(iniPath);
    }

    // [Position]
    config.monitorIndex = doc.GetInt("Position", "monitor_index", config.monitorIndex);
    config.preset = MonitorHelper::StringToPreset(doc.GetString("Position", "preset", MonitorHelper::PresetToString(config.preset)));
    config.customX = doc.GetInt("Position", "custom_x", config.customX);
    config.customY = doc.GetInt("Position", "custom_y", config.customY);
    config.customRelX = doc.GetInt("Position", "custom_rel_x", config.customRelX);
    config.customRelY = doc.GetInt("Position", "custom_rel_y", config.customRelY);
    config.margin = doc.GetInt("Position", "margin", config.margin);

    std::string layoutStr = doc.GetString("Position", "layout", "horizontal");
    for (char& c : layoutStr) c = static_cast<char>(tolower(c));
    if (layoutStr == "grid" || layoutStr == "multiline" || layoutStr == "multi-line") {
        config.layout = OverlayLayout::Grid;
    } else if (layoutStr == "custom_grid" || layoutStr == "matrix" || layoutStr == "custom") {
        config.layout = OverlayLayout::CustomGrid;
    } else {
        config.layout = OverlayLayout::Horizontal;
    }

    config.maxGpusPerRow = doc.GetInt("Position", "max_gpus_per_row", config.maxGpusPerRow);
    if (config.maxGpusPerRow < 1) config.maxGpusPerRow = 1;

    std::string gridModeStr = doc.GetString("Position", "grid_mode", "unified");
    for (char& c : gridModeStr) c = static_cast<char>(tolower(c));
    config.gridMode = (gridModeStr == "islands" || gridModeStr == "split" || gridModeStr == "distributed")
        ? GridMode::Islands
        : GridMode::Unified;

    config.cellGap = doc.GetInt("Position", "cell_gap", config.cellGap);
    if (config.cellGap < 0) config.cellGap = 0;
    config.rowGap = doc.GetInt("Position", "row_gap", config.rowGap);
    if (config.rowGap < 0) config.rowGap = 0;

    // Parse Custom Matrix Grid rows
    int maxColsFound = 0;
    for (int r = 1; r <= 32; ++r) {
        std::string rowKey = "grid_row_" + std::to_string(r);
        std::string rowVal = doc.GetString("Position", rowKey, "");
        if (rowVal.empty()) {
            rowVal = doc.GetString("Grid", rowKey, "");
        }
        if (rowVal.empty()) {
            std::string altKey = "row_" + std::to_string(r);
            rowVal = doc.GetString("Position", altKey, "");
            if (rowVal.empty()) {
                rowVal = doc.GetString("Grid", altKey, "");
            }
        }
        if (rowVal.empty()) continue;

        std::vector<std::string> rawTokens;
        std::string curTok;
        for (char ch : rowVal) {
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
    std::string fontName = doc.GetString("Appearance", "font_name", "auto");
    config.fontFace = Utf8ToWide(fontName);

    std::string fontWeight = doc.GetString("Appearance", "font_weight", "semibold");
    config.fontWeight = Utf8ToWide(fontWeight);

    config.fontSize = doc.GetInt("Appearance", "font_size", config.fontSize);
    if (config.fontSize <= 0) {
#ifdef _WIN32
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(NONCLIENTMETRICSW);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            config.fontSize = std::abs(ncm.lfCaptionFont.lfHeight);
        } else {
            config.fontSize = 13;
        }
#else
        config.fontSize = 13;
#endif
    }

    config.textColor = HexToColor(doc.GetString("Appearance", "text_color", "#FFFFFF"), config.textColor);
    config.labelColor = HexToColor(doc.GetString("Appearance", "label_color", "#A0A0A0"), config.labelColor);
    config.separatorColor = HexToColor(doc.GetString("Appearance", "separator_color", "#6E6E6E"), config.separatorColor);
    config.colorHigh = HexToColor(doc.GetString("Appearance", "color_high", "#FFD700"), config.colorHigh);
    config.colorCritical = HexToColor(doc.GetString("Appearance", "color_critical", "#FF4D4D"), config.colorCritical);

    config.enableDynamicColors = doc.GetBool("Appearance", "enable_dynamic_colors", config.enableDynamicColors);
    config.dropShadow = doc.GetBool("Appearance", "drop_shadow", config.dropShadow);

    std::string sepStr = doc.GetString("Appearance", "separator", "");
    if (!sepStr.empty()) {
        config.separator = Utf8ToWide(sepStr);
    }

    std::string opStr = doc.GetString("Appearance", "opacity", "100");
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
    config.showTrayIcon = doc.GetBool("Appearance", "show_tray_icon", false);

    // [Telemetry]
    config.refreshIntervalMs = static_cast<uint32_t>(doc.GetInt("Telemetry", "refresh_interval_ms", static_cast<int>(config.refreshIntervalMs)));
    if (config.refreshIntervalMs < 100 || config.refreshIntervalMs > 60000) {
        if (config.refreshIntervalMs < 100) config.refreshIntervalMs = 1000;
        else if (config.refreshIntervalMs > 60000) config.refreshIntervalMs = 60000;
    }

    std::string gpuTargetStr = doc.GetString("Telemetry", "gpu_index", "0");
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

    std::string vramStr = doc.GetString("Telemetry", "vram_mode", "physical");
    for (char& c : vramStr) c = static_cast<char>(tolower(c));
    if (vramStr == "usable") {
        config.vramMode = VramMode::Usable;
    } else {
        config.vramMode = VramMode::Physical;
    }

    std::string orderStr = doc.GetString("Telemetry", "order", "CPU, GPU, RAM, NET");
    std::vector<std::string> parsedOrder;
    std::stringstream ss(orderStr);
    std::string ordToken;
    while (std::getline(ss, ordToken, ',')) {
        size_t first = ordToken.find_first_not_of(" \t\r\n");
        size_t last = ordToken.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            std::string trimmed = ordToken.substr(first, last - first + 1);
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

    config.showCpu = doc.GetBool("Telemetry", "show_cpu", config.showCpu);
    config.showGpu = doc.GetBool("Telemetry", "show_gpu", config.showGpu);
    config.showRam = doc.GetBool("Telemetry", "show_ram", config.showRam);
    config.showNet = doc.GetBool("Telemetry", "show_net", config.showNet);
    config.showGpuTemp = doc.GetBool("Telemetry", "show_gpu_temp", config.showGpuTemp);
    config.showGpuPower = doc.GetBool("Telemetry", "show_gpu_power", config.showGpuPower);
    config.showGpuFan = doc.GetBool("Telemetry", "show_gpu_fan", config.showGpuFan);
    config.showMaxMemory = doc.GetBool("Telemetry", "show_max_memory", config.showMaxMemory);
    config.hideOfflineMetrics = doc.GetBool("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics);

    // [GPU0] .. [GPU15]
    config.perGpuConfigs.clear();
    for (uint32_t i = 0; i < 16; ++i) {
        std::string section = "GPU" + std::to_string(i);
        if (doc.HasSection(section)) {
            PerGpuConfig gpuCfg;
            gpuCfg.enabled = doc.GetBool(section, "enabled", true);
            gpuCfg.showUtil = doc.GetBool(section, "show_util", true);
            gpuCfg.showVram = doc.GetBool(section, "show_vram", true);
            gpuCfg.showTemp = doc.GetBool(section, "show_temp", config.showGpuTemp);
            gpuCfg.showPower = doc.GetBool(section, "show_power", config.showGpuPower);
            gpuCfg.showFan = doc.GetBool(section, "show_fan", config.showGpuFan);
            gpuCfg.label = doc.GetString(section, "label", "");
            config.perGpuConfigs[i] = gpuCfg;
        }
    }

    return config;
}

bool ConfigManager::Save(const std::string& iniPath, const AppConfig& config) {
    std::error_code ec;
    size_t lastSlash = iniPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        std::string dir = iniPath.substr(0, lastSlash);
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
        }
    }

    if (!std::filesystem::exists(iniPath, ec)) {
        return CreateDefaultConfigFile(iniPath, config);
    }

    IniDocument doc;
    doc.Load(iniPath);

    doc.SetInt("Position", "monitor_index", config.monitorIndex);
    doc.SetString("Position", "preset", MonitorHelper::PresetToString(config.preset));
    doc.SetInt("Position", "custom_x", config.customX);
    doc.SetInt("Position", "custom_y", config.customY);
    doc.SetInt("Position", "custom_rel_x", config.customRelX);
    doc.SetInt("Position", "custom_rel_y", config.customRelY);
    doc.SetInt("Position", "margin", config.margin);
    doc.SetString("Position", "layout", config.layout == OverlayLayout::CustomGrid ? "custom_grid" : (config.layout == OverlayLayout::Grid ? "grid" : "horizontal"));
    doc.SetString("Position", "grid_mode", config.gridMode == GridMode::Islands ? "islands" : "unified");
    doc.SetInt("Position", "cell_gap", config.cellGap);
    doc.SetInt("Position", "row_gap", config.rowGap);
    doc.SetInt("Position", "max_gpus_per_row", config.maxGpusPerRow);

    doc.SetString("Appearance", "font_name", WideToUtf8(config.fontFace));
    doc.SetString("Appearance", "font_weight", WideToUtf8(config.fontWeight));
    doc.SetInt("Appearance", "font_size", config.fontSize);
    doc.SetString("Appearance", "text_color", ColorToHex(config.textColor));
    doc.SetString("Appearance", "label_color", ColorToHex(config.labelColor));
    doc.SetString("Appearance", "separator_color", ColorToHex(config.separatorColor));
    doc.SetString("Appearance", "color_high", ColorToHex(config.colorHigh));
    doc.SetString("Appearance", "color_critical", ColorToHex(config.colorCritical));
    doc.SetBool("Appearance", "enable_dynamic_colors", config.enableDynamicColors);
    doc.SetBool("Appearance", "drop_shadow", config.dropShadow);
    doc.SetString("Appearance", "separator", WideToUtf8(config.separator));
    doc.SetInt("Appearance", "opacity", config.opacity);
    doc.SetBool("Appearance", "show_tray_icon", config.showTrayIcon);

    doc.SetInt("Telemetry", "refresh_interval_ms", static_cast<int>(config.refreshIntervalMs));
    doc.SetString("Telemetry", "gpu_index", config.gpuTarget.empty() ? std::to_string(config.gpuIndex) : config.gpuTarget);
    doc.SetString("Telemetry", "vram_mode", config.vramMode == VramMode::Usable ? "usable" : "physical");
    doc.SetString("Telemetry", "order", JoinMetricOrder(config.metricOrder));
    doc.SetBool("Telemetry", "show_cpu", config.showCpu);
    doc.SetBool("Telemetry", "show_gpu", config.showGpu);
    doc.SetBool("Telemetry", "show_ram", config.showRam);
    doc.SetBool("Telemetry", "show_net", config.showNet);
    doc.SetBool("Telemetry", "show_gpu_temp", config.showGpuTemp);
    doc.SetBool("Telemetry", "show_gpu_power", config.showGpuPower);
    doc.SetBool("Telemetry", "show_gpu_fan", config.showGpuFan);
    doc.SetBool("Telemetry", "show_max_memory", config.showMaxMemory);
    doc.SetBool("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics);

    for (const auto& pair : config.perGpuConfigs) {
        std::string section = "GPU" + std::to_string(pair.first);
        const auto& gpuCfg = pair.second;
        doc.SetBool(section, "enabled", gpuCfg.enabled);
        doc.SetBool(section, "show_util", gpuCfg.showUtil);
        doc.SetBool(section, "show_vram", gpuCfg.showVram);
        doc.SetBool(section, "show_temp", gpuCfg.showTemp);
        doc.SetBool(section, "show_power", gpuCfg.showPower);
        doc.SetBool(section, "show_fan", gpuCfg.showFan);
        if (!gpuCfg.label.empty()) {
            doc.SetString(section, "label", gpuCfg.label);
        }
    }

    return doc.Save(iniPath);
}

} // namespace BareStats
