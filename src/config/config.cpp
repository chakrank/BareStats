#include "config.h"
#include "ini_parser.h"
#include "../metrics/gpu_collector.h"
#include "../util/crypto/crypto_box.h"
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
#include <sddl.h>
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
#elif defined(__APPLE__)
#include <unistd.h>
#include <cstdlib>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <cstdlib>
#include <sys/stat.h>
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

static void HardenConfigFilePermissions(const std::string& path) {
#if defined(_WIN32)
    std::wstring widePath = Utf8ToWide(path);
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1,
            &pSD,
            NULL)) {
        SetFileSecurityW(widePath.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, pSD);
        LocalFree(pSD);
    }
#else
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

bool ConfigManager::CreateDefaultConfigFile(const std::string& iniPath, const AppConfig& config) {
    std::ostringstream oss;
    oss << "# ==============================================================================\n"
        << "# BareStats - Configuration Reference\n"
        << "# ==============================================================================\n"
        << "# Configuration search paths:\n"
        << "#   Windows:\n"
        << "#     1. Portable Mode: <exe_directory>\\config.ini\n"
        << "#     2. Installed Mode: %LOCALAPPDATA%\\BareStats\\config.ini\n"
        << "#   macOS:\n"
        << "#     1. Portable Mode: <app_bundle>/Contents/MacOS/config.ini (or next to binary)\n"
        << "#     2. Standard Mode: ~/Library/Application Support/BareStats/config.ini\n"
        << "#   Linux:\n"
        << "#     1. Current Working Directory: ./config.ini\n"
        << "#     2. Portable Mode: <exe_directory>/config.ini\n"
        << "#     3. Standard Mode: ~/.config/barestats/config.ini (or $XDG_CONFIG_HOME/barestats/config.ini)\n"
        << "# ==============================================================================\n\n"
        << "[BareStats]\n"
        << "# Configuration schema version. Used for automated near-zero-overhead in-place migrations.\n"
        << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n\n"
        << "[General]\n"
        << "# Run mode: 0 = Desktop GUI Overlay (default), 1 = Silent Headless Daemon (Windows, macOS, Linux)\n"
        << "headless = " << (config.headless ? "1" : "0") << "\n\n"
        << "# System tray / menu bar icon (1 = enabled, 0 = disabled)\n"
        << "#   Windows: System Tray Notification Area\n"
        << "#   macOS:   Top Menu Bar (NSStatusItem)\n"
        << "#   Linux:   AppIndicator / StatusNotifierItem top panel indicator\n"
        << "show_tray_icon = " << (config.showTrayIcon ? "1" : "0") << "\n\n"
        << "[Position]\n"
        << "# Target monitor index: 0 = primary display, 1 = secondary (Windows, macOS, Linux)\n"
        << "monitor_index = " << config.monitorIndex << "\n\n"
        << "# Preset positions:\n"
        << "#   top-left, top-center, top-right\n"
        << "#   center-left, center, center-right\n"
        << "#   bottom-left, bottom-center, bottom-right\n"
        << "#   custom\n"
        << "preset = " << MonitorHelper::PresetToString(config.preset) << "\n\n"
        << "# Custom coordinates in pixels (active when preset = custom)\n"
        << "custom_x = " << config.customX << "\n"
        << "custom_y = " << config.customY << "\n\n"
        << "# Monitor-relative normalized coordinates (persisted automatically on drag-to-reposition)\n"
        << "custom_rel_x = " << config.customRelX << "\n"
        << "custom_rel_y = " << config.customRelY << "\n\n"
        << "# Edge margin in pixels from display boundaries\n"
        << "margin = " << config.margin << "\n\n"
        << "[Layout]\n"
        << "# Layout presentation mode: (Windows, macOS, Linux)\n"
        << "#   horizontal  = Single-line status ribbon driven by 'order' sequence below\n"
        << "#   grid        = Compact multi-row widget (Row 0 follows non-GPU 'order', subsequent rows wrap GPUs)\n"
        << "#   custom_grid = 2D matrix layout defined explicitly via grid_row_* below\n"
        << "mode = " << (config.layout == OverlayLayout::CustomGrid ? "custom_grid" : (config.layout == OverlayLayout::Grid ? "grid" : "horizontal")) << "\n\n"
        << "# Metric display sequence (used for 'horizontal' ribbon and Row 0 of 'grid'):\n"
        << "# Tokens: CPU, GPU, RAM, NET, LLM, or remote cluster tokens (e.g. rig1:cpu, rig1:gpu, rig1:llm)\n"
        << "order = " << JoinMetricOrder(config.metricOrder) << "\n\n"
        << "# Maximum GPUs rendered per row when layout is grid or multi-GPU row wrapping is active (default: 2)\n"
        << "max_gpus_per_row = " << config.maxGpusPerRow << "\n\n"
        << "# Custom Matrix Grid options (active when mode = custom_grid):\n"
        << "# grid_mode:\n"
        << "#   unified (default) = Single compact layered window (Windows, macOS, Linux)\n"
        << "#   islands           = Distributed micro-windows [Windows only: native per-monitor DPI & click-through]\n"
        << "grid_mode = " << (config.gridMode == GridMode::Islands ? "islands" : "unified") << "\n\n"
        << "# Column gap and row gap in pixels for CustomGrid\n"
        << "cell_gap = " << config.cellGap << "\n"
        << "row_gap = " << config.rowGap << "\n\n"
        << "# Custom Matrix Grid rows (used when mode = custom_grid):\n"
        << "# Use '.' or 'null' for empty slots. Repeat adjacent tokens on a row to merge columns (colspan).\n"
        << "# Available tokens: cpu, ram, net, gpu (or gpu0, gpu1...)\n"
        << "# Sub-metrics: cpu_util, cpu_temp, cpu_power, cpu_fan, ram_used, ram_pct,\n"
        << "#              gpu0_util, gpu0_vram, gpu0_temp, gpu0_power, gpu0_fan, net_rx, net_tx\n"
        << "# Examples:\n"
        << "#   grid_row_1 = cpu | ram | net\n"
        << "#   grid_row_2 = gpu\n\n"
        << "[Appearance]\n"
        << "# Font face: 'auto' dynamically resolves to Segoe UI (Windows), San Francisco (macOS), or system sans-serif (Linux)\n"
        << "font_name = auto\n"
        << "# Font weight: semibold (default), regular, bold\n"
        << "font_weight = " << (config.fontWeight == L"bold" ? "bold" : (config.fontWeight == L"regular" ? "regular" : "semibold")) << "\n"
        << "# Font size in pixels at standard 96 DPI / 1x scale (Per-Monitor V2 and Retina scale automatically)\n"
        << "font_size = " << config.fontSize << "\n"
        << "# Text color for numeric values and units in hex (#RRGGBB)\n"
        << "text_color = " << ColorToHex(config.textColor) << "\n"
        << "# Label color for subsystem titles (CPU, GPU, RAM) in hex (#RRGGBB)\n"
        << "label_color = " << ColorToHex(config.labelColor) << "\n"
        << "# Category divider separator color (#RRGGBB)\n"
        << "separator_color = " << ColorToHex(config.separatorColor) << "\n"
        << "# Threshold alert colors\n"
        << "color_high = " << ColorToHex(config.colorHigh) << "\n"
        << "color_critical = " << ColorToHex(config.colorCritical) << "\n"
        << "# Dynamic color alerts on high/critical thresholds (1 = enabled, 0 = disabled)\n"
        << "enable_dynamic_colors = " << (config.enableDynamicColors ? "1" : "0") << "\n"
        << "# Text drop shadow for legibility over light backgrounds\n"
        << "drop_shadow = " << (config.dropShadow ? "1" : "0") << "\n"
        << "# Symbol style for telemetry divider and network rate arrows (auto, ascii, unicode)\n"
        << "symbol_style = " << config.symbolStyle << "\n"
        << "# Divider separator glyph (default: │ box drawings light vertical; empty/none = no divider)\n"
        << "separator = " << WideToUtf8(config.separator) << "\n"
        << "# Global overlay opacity percentage (10 to 100, default: 100) (Windows, macOS, Linux)\n"
        << "opacity = " << config.opacity << "\n\n"
        << "[Telemetry]\n"
        << "# Polling and refresh interval in milliseconds (default: 1000 = 1 sec)\n"
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
        << "# Target GPU adapter index or multi-GPU target expression (e.g. 0, '0, 1', or 'all')\n"
        << "gpu_index = " << (config.gpuTarget.empty() ? std::to_string(config.gpuIndex) : config.gpuTarget) << "\n"
        << "# VRAM capacity calculation mode: physical (default) or usable (allocatable limit excluding driver carve-outs)\n"
        << "vram_mode = " << (config.vramMode == VramMode::Usable ? "usable" : "physical") << "\n"
        << "# Integrated GPU / UMA memory display mode: usage (true active usage with [S]), shared, dedicated\n"
        << "igpu_vram_mode = " << (config.igpuVramMode == IgpuVramMode::Shared ? "shared" : (config.igpuVramMode == IgpuVramMode::Dedicated ? "dedicated" : "usage")) << "\n"
        << "# Background collector switches (1 = enabled, 0 = disabled)\n"
        << "# Note: Metrics present in active layout automatically collect. These toggles force background collection\n"
        << "# for UDP cluster streaming or logging even when omitted from the visible desktop layout.\n"
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
        << "# label = Compute\n\n"
        << "[Cluster]\n"
        << "# Control Center Master Private Key (Curve25519 256-bit secret, 64 hex characters)\n"
        << "# Auto-generated when exporting satellite.ini. Only the derived Public Key is placed on satellites.\n";
    if (!config.cluster.clusterKey.empty()) {
        oss << "cluster_key = " << config.cluster.clusterKey << "\n"
            << "listen_port = " << config.cluster.listenPort << "\n";
        if (!config.cluster.allowedRigs.empty()) {
            oss << "allowed_rigs = " << config.cluster.allowedRigs << "\n\n";
        } else {
            oss << "# allowed_rigs = \n\n";
        }
    } else {
        oss << "# cluster_key = \n"
            << "# listen_port = 9889\n"
            << "# allowed_rigs = \n\n";
    }
    oss << "[Cluster.Aliases]\n"
        << "# Friendly display aliases for remote rigs on this Control Center\n"
        << "# Maps remote rig_id from incoming UDP packets to custom overlay labels:\n";
    if (!config.cluster.aliases.empty()) {
        for (const auto& a : config.cluster.aliases) {
            oss << a.first << " = " << a.second << "\n";
        }
        oss << "\n";
    } else {
        oss << "# rig1 = MacMini1\n"
            << "# rig2 = LinuxWorkstation\n\n";
    }
    oss << "[LLM]\n"
        << "# AI Inference Telemetry (Ollama, vLLM, llama.cpp, TGI, SGLang, OpenAI-compatible)\n"
        << "enabled = " << (config.llm.enabled ? "1" : "0") << "\n"
        << "endpoints = " << config.llm.endpoints << "\n"
        << "timeout_ms = " << config.llm.timeoutMs << "\n"
        << "poll_interval_ms = " << config.llm.pollIntervalMs << "\n"
        << "retry_interval_ms = " << config.llm.retryIntervalMs << "\n"
        << "decay_hold_sec = " << config.llm.decayHoldSec << "\n"
        << "max_model_len = " << config.llm.maxModelLen << "\n"
        << "filter_embeddings = " << (config.llm.filterEmbeddings ? "1" : "0") << "\n"
        << "min_vram_mb = " << config.llm.minVramMb << "\n"
        << "target_model = " << config.llm.targetModel << "\n\n"
        << "[LLM.Aliases]\n"
        << "# Friendly display aliases for long model names (case-insensitive substring match)\n"
        << "# Maps 40-60 character server model identifiers down to clean compact badges:\n";
    if (!config.llm.aliases.empty()) {
        for (const auto& a : config.llm.aliases) {
            oss << a.first << " = " << a.second << "\n";
        }
    } else {
        oss << "# deepseek-r1-distill-qwen-14b = R1-14B\n"
            << "# llama-3.3-70b-instruct = Llama-3.3\n";
    }

    std::ofstream file(iniPath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        std::string content = oss.str();
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        bool ok = file.good();
        file.close();

        if (ok && !config.cluster.clusterKey.empty()) {
            HardenConfigFilePermissions(iniPath);
        }
        return ok;
    }
    return false;
}

std::string ConfigManager::GetDefaultConfigPath() {
#ifdef _WIN32
    // 1. Check for Portable Mode (config.ini right next to the executable)
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
    // 1. Check for Portable Mode (config.ini right next to executable / inside bundle)
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
    // 1. Check for Portable Mode (config.ini right next to /proc/self/exe)
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
    bool hasClusterKey = false;
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
                if (currentSec == "cluster" && key == "cluster_key") {
                    std::string val = trim(t.substr(eqPos + 1));
                    if (!val.empty()) {
                        hasClusterKey = true;
                    }
                }
            }
        }
    }

    bool needBareStatsSec = (existingSections.count("barestats") == 0);
    bool needLayout = (existingKeys["position"].count("layout") == 0);
    bool needMaxGpusPerRow = (existingKeys["position"].count("max_gpus_per_row") == 0);
    bool needVramMode = (existingKeys["telemetry"].count("vram_mode") == 0);
    bool needOrder = (existingKeys["telemetry"].count("order") == 0);
    bool needShowNet = (existingKeys["telemetry"].count("show_net") == 0);
    bool hasShowTrayIcon = (existingKeys["appearance"].count("show_tray_icon") > 0) ||
                           (existingKeys["general"].count("show_tray_icon") > 0);
    bool needShowTrayIcon = !hasShowTrayIcon;
    bool needGpuOverrides = !hasGpuSectionOrComment;
    bool needClusterSec = (existingSections.count("cluster") == 0);
    bool needLlmSec = (existingSections.count("llm") == 0);

    std::ostringstream out;

    if (needBareStatsSec) {
        out << "[BareStats]\n"
            << "# Configuration schema version. Used for automated near-zero-overhead in-place migrations.\n"
            << "schema_version = " << CURRENT_CONFIG_SCHEMA_VERSION << "\n\n";
    }

    currentSec = "";
    auto injectMissingForSec = [&](const std::string& sec) {
        if (sec == "general") {
            if (needShowTrayIcon) {
                out << "# System tray / menu bar icon (1 = enabled, 0 = disabled)\n"
                    << "#   Windows: System Tray Notification Area\n"
                    << "#   macOS:   Top Menu Bar (NSStatusItem)\n"
                    << "#   Linux:   AppIndicator / StatusNotifierItem top panel indicator\n"
                    << "show_tray_icon = 1\n\n";
                needShowTrayIcon = false;
            }
        } else if (sec == "position") {
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

    if (needShowTrayIcon) {
        out << "\n[General]\n"
            << "# System tray / menu bar icon (1 = enabled, 0 = disabled)\n"
            << "#   Windows: System Tray Notification Area\n"
            << "#   macOS:   Top Menu Bar (NSStatusItem)\n"
            << "#   Linux:   AppIndicator / StatusNotifierItem top panel indicator\n"
            << "show_tray_icon = 1\n";
        needShowTrayIcon = false;
    }

    if (needClusterSec) {
        out << "\n[Cluster]\n"
            << "# 256-bit AEAD Cluster Secret for encrypted UDP Satellite communication (64 hex characters)\n"
            << "# cluster_key = \n"
            << "listen_port = 9889\n";
    }

    if (needLlmSec) {
        out << "\n[LLM]\n"
            << "# Enable local/remote AI inference monitoring\n"
            << "enabled = 1\n"
            << "# Direct Pull HTTP endpoints (comma or semicolon separated)\n"
            << "# Auto-probes popular local frameworks: Ollama (11434), llama.cpp (8080), vLLM/TGI/SGLang (8000), LM Studio (1234), Python (38111)\n"
            << "endpoints = local=http://127.0.0.1:11434, local=http://127.0.0.1:8080, local=http://127.0.0.1:8000, local=http://127.0.0.1:1234, local=http://127.0.0.1:38111\n"
            << "timeout_ms = 300\n"
            << "poll_interval_ms = 1000\n"
            << "retry_interval_ms = 10000\n"
            << "decay_hold_sec = 3\n"
            << "max_model_len = 16\n"
            << "filter_embeddings = 1\n"
            << "min_vram_mb = 500\n"
            << "target_model = active\n\n"
            << "[LLM.Aliases]\n"
            << "# Friendly display aliases for long model names (case-insensitive substring match)\n"
            << "# deepseek-r1-distill-qwen-14b = R1-14B\n";
    }

    std::string newContent = out.str();
    std::ofstream outFile(iniPath, std::ios::binary | std::ios::trunc);
    if (outFile.is_open()) {
        outFile.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
        bool ok = outFile.good();
        outFile.close();
        if (ok && hasClusterKey) {
            HardenConfigFilePermissions(iniPath);
            HardenConfigFilePermissions(bakPath);
        }
        return ok;
    }
    return false;
}

AppConfig ConfigManager::Load(const std::string& iniPath) {
    AppConfig config;

    std::error_code ec;
    if (!std::filesystem::exists(iniPath, ec)) {
        // Return in-memory defaults without writing to disk
        return config;
    }

    IniDocument doc;
    if (!doc.Load(iniPath)) {
        // Corrupted or unreadable; return in-memory defaults without overwriting
        return config;
    }

    int diskVersion = doc.GetInt("BareStats", "schema_version", 0);
    if (diskVersion < CURRENT_CONFIG_SCHEMA_VERSION) {
        MigrateSchema(iniPath, diskVersion, CURRENT_CONFIG_SCHEMA_VERSION);
        doc.Load(iniPath);
    }

    // [General]
    std::string headlessStr = doc.GetString("General", "headless", "0");
    for (char& c : headlessStr) c = static_cast<char>(tolower(c));
    config.headless = (headlessStr == "1" || headlessStr == "true" || headlessStr == "yes" || headlessStr == "on");
    if (doc.HasKey("General", "show_tray_icon")) {
        config.showTrayIcon = doc.GetBool("General", "show_tray_icon", config.showTrayIcon);
    } else if (doc.HasKey("Appearance", "show_tray_icon")) {
#if defined(_WIN32) || defined(__APPLE__)
        // Legacy v1.3 migration erroneously injected show_tray_icon = 0 into [Appearance].
        // On Windows and macOS, default to true unless explicitly overridden in [General].
        config.showTrayIcon = true;
#else
        config.showTrayIcon = doc.GetBool("Appearance", "show_tray_icon", config.showTrayIcon);
#endif
    }

    // [Position]
    config.monitorIndex = doc.GetInt("Position", "monitor_index", config.monitorIndex);
    config.preset = MonitorHelper::StringToPreset(doc.GetString("Position", "preset", MonitorHelper::PresetToString(config.preset)));
    config.customX = doc.GetInt("Position", "custom_x", config.customX);
    config.customY = doc.GetInt("Position", "custom_y", config.customY);
    config.customRelX = doc.GetInt("Position", "custom_rel_x", config.customRelX);
    config.customRelY = doc.GetInt("Position", "custom_rel_y", config.customRelY);
    config.margin = doc.GetInt("Position", "margin", config.margin);

    // [Layout] (with full fallback to [Position])
    std::string layoutStr = doc.GetString("Layout", "mode", "");
    if (layoutStr.empty()) layoutStr = doc.GetString("Layout", "layout", "");
    if (layoutStr.empty()) layoutStr = doc.GetString("Position", "layout", "horizontal");
    for (char& c : layoutStr) c = static_cast<char>(tolower(c));
    if (layoutStr == "grid" || layoutStr == "multiline" || layoutStr == "multi-line") {
        config.layout = OverlayLayout::Grid;
    } else if (layoutStr == "custom_grid" || layoutStr == "matrix" || layoutStr == "custom") {
        config.layout = OverlayLayout::CustomGrid;
    } else {
        config.layout = OverlayLayout::Horizontal;
    }

    config.maxGpusPerRow = doc.GetInt("Layout", "max_gpus_per_row", doc.GetInt("Position", "max_gpus_per_row", config.maxGpusPerRow));
    if (config.maxGpusPerRow < 1) config.maxGpusPerRow = 1;

    std::string gridModeStr = doc.GetString("Layout", "grid_mode", doc.GetString("Position", "grid_mode", "unified"));
    for (char& c : gridModeStr) c = static_cast<char>(tolower(c));
    config.gridMode = (gridModeStr == "islands" || gridModeStr == "split" || gridModeStr == "distributed")
        ? GridMode::Islands
        : GridMode::Unified;

    config.cellGap = doc.GetInt("Layout", "cell_gap", doc.GetInt("Position", "cell_gap", config.cellGap));
    if (config.cellGap < 0) config.cellGap = 0;
    config.rowGap = doc.GetInt("Layout", "row_gap", doc.GetInt("Position", "row_gap", config.rowGap));
    if (config.rowGap < 0) config.rowGap = 0;

    // Parse metric display order (used for 'horizontal' ribbon and Row 0 of 'grid', or as bridge to CustomGrid)
    std::string orderStr = doc.GetString("Layout", "order", "");
    if (orderStr.empty()) {
        orderStr = doc.GetString("Telemetry", "order", "CPU, GPU, RAM, NET");
    }
    std::vector<std::string> parsedOrder;
    std::stringstream ss(orderStr);
    std::string ordToken;
    while (std::getline(ss, ordToken, ',')) {
        size_t first = ordToken.find_first_not_of(" \t\r\n");
        size_t last = ordToken.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            std::string trimmed = ordToken.substr(first, last - first + 1);
            if (!trimmed.empty()) {
                parsedOrder.push_back(trimmed);
            }
        }
    }
    if (!parsedOrder.empty()) {
        config.metricOrder = parsedOrder;
    }

    // Parse Custom Matrix Grid rows from [Layout], [Position], or [Grid]
    int maxColsFound = 0;
    for (int r = 1; r <= 32; ++r) {
        std::string rowKey = "grid_row_" + std::to_string(r);
        std::string rowVal = doc.GetString("Layout", rowKey, "");
        if (rowVal.empty()) {
            rowVal = doc.GetString("Position", rowKey, "");
        }
        if (rowVal.empty()) {
            rowVal = doc.GetString("Grid", rowKey, "");
        }
        if (rowVal.empty()) {
            std::string altKey = "row_" + std::to_string(r);
            rowVal = doc.GetString("Layout", altKey, "");
            if (rowVal.empty()) {
                rowVal = doc.GetString("Position", altKey, "");
            }
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
            // Check for explicit span syntax: e.g. token:span(2), token:colspan(2), or token:2
            size_t spanPos = tok.rfind(":span(");
            if (spanPos == std::string::npos) {
                spanPos = tok.rfind(":colspan(");
            }
            if (spanPos == std::string::npos) {
                // Check if ends with purely numeric suffix after the last colon, e.g. "gpu0:2"
                size_t lastColon = tok.rfind(':');
                if (lastColon != std::string::npos && lastColon > 0) {
                    std::string suffix = tok.substr(lastColon + 1);
                    if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit)) {
                        spanPos = lastColon;
                    }
                }
            }

            if (spanPos != std::string::npos) {
                std::string spanPart = tok.substr(spanPos + 1);
                tok = tok.substr(0, spanPos);
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
        if (!config.metricOrder.empty()) {
            std::vector<std::string> nonGpuTokens;
            std::vector<std::string> gpuTokens;

            for (const auto& tok : config.metricOrder) {
                std::string t = tok;
                size_t first = t.find_first_not_of(" \t\r\n");
                size_t last = t.find_last_not_of(" \t\r\n");
                if (first != std::string::npos && last != std::string::npos) {
                    t = t.substr(first, last - first + 1);
                }
                for (char& c : t) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (t.empty()) continue;

                if (t == "gpu" || t == "graphics" || t == "video" || t.rfind("gpu", 0) == 0 || t.find(":gpu") != std::string::npos) {
                    gpuTokens.push_back(t);
                } else {
                    nonGpuTokens.push_back(t);
                }
            }

            if (!nonGpuTokens.empty() && !gpuTokens.empty()) {
                // Multi-row bridge: Non-GPU metrics on Row 0, GPU metrics on Row 1
                int cols = static_cast<int>(std::max(nonGpuTokens.size(), gpuTokens.size()));
                GridRow r0;
                int c0 = 0;
                for (const auto& t : nonGpuTokens) {
                    r0.cells.push_back({ t, c0++, 1 });
                }
                GridRow r1;
                int c1 = 0;
                int span = (gpuTokens.size() == 1 && cols > 1) ? cols : 1;
                for (const auto& t : gpuTokens) {
                    r1.cells.push_back({ t, c1, span });
                    c1 += span;
                }
                config.customGrid.rows.push_back(std::move(r0));
                config.customGrid.rows.push_back(std::move(r1));
                config.customGrid.numCols = std::max(c0, c1);
                config.customGrid.numRows = 2;
            } else if (!nonGpuTokens.empty() || !gpuTokens.empty()) {
                const auto& activeList = !nonGpuTokens.empty() ? nonGpuTokens : gpuTokens;
                GridRow r0;
                int col = 0;
                for (const auto& t : activeList) {
                    r0.cells.push_back({ t, col++, 1 });
                }
                config.customGrid.numCols = static_cast<int>(r0.cells.size());
                config.customGrid.rows.push_back(std::move(r0));
                config.customGrid.numRows = 1;
            }
        }

        // If metricOrder was also empty, fall back to default compact 2-row template
        if (config.customGrid.IsEmpty()) {
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

    config.symbolStyle = doc.GetString("Appearance", "symbol_style", "auto");
    for (char& c : config.symbolStyle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (doc.HasKey("Appearance", "separator")) {
        config.hasUserDefinedSeparator = true;
        std::string sepStr = doc.GetString("Appearance", "separator", "");
        if (sepStr == "none" || sepStr == "\"\"" || sepStr == "''") {
            config.separator = L"";
        } else {
            config.separator = Utf8ToWide(sepStr);
        }
    } else {
        config.hasUserDefinedSeparator = false;
        config.separator = L"\x2502";
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

    // Multi-GPU auto-enable logic:
    // If layout is Horizontal/Grid and metricOrder has "gpu" (or multiple "gpuN" tokens),
    // or if layout is CustomGrid / Islands and grid has gpu tokens,
    // ensure all GPUs are targeted so multi-GPU systems display out-of-the-box without manual config.
    if (!config.targetAllGpus) {
        if (config.layout == OverlayLayout::Horizontal || config.layout == OverlayLayout::Grid) {
            bool hasGpuToken = false;
            for (const auto& item : config.metricOrder) {
                std::string lowerItem = item;
                for (char& c : lowerItem) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (lowerItem == "gpu" || lowerItem == "graphics" || lowerItem == "video" || lowerItem.rfind("gpu", 0) == 0) {
                    hasGpuToken = true;
                    break;
                }
            }
            if (hasGpuToken) {
                config.targetAllGpus = true;
            }
        } else if (config.layout == OverlayLayout::CustomGrid) {
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
            if (hasGpuToken) {
                config.targetAllGpus = true;
            }
        }
    }

    std::string vramStr = doc.GetString("Telemetry", "vram_mode", "physical");
    for (char& c : vramStr) c = static_cast<char>(tolower(c));
    if (vramStr == "usable") {
        config.vramMode = VramMode::Usable;
    } else {
        config.vramMode = VramMode::Physical;
    }

    std::string igpuVramStr = doc.GetString("Telemetry", "igpu_vram_mode", doc.GetString("Hardware", "igpu_vram_mode", "usage"));
    for (char& c : igpuVramStr) c = static_cast<char>(tolower(c));
    if (igpuVramStr == "shared") {
        config.igpuVramMode = IgpuVramMode::Shared;
    } else if (igpuVramStr == "dedicated") {
        config.igpuVramMode = IgpuVramMode::Dedicated;
    } else {
        config.igpuVramMode = IgpuVramMode::Usage;
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

    // [LLM]
    config.llm.enabled = doc.GetBool("LLM", "enabled", config.llm.enabled);
    config.llm.endpoints = doc.GetString("LLM", "endpoints", config.llm.endpoints);
    config.llm.timeoutMs = static_cast<uint32_t>(doc.GetInt("LLM", "timeout_ms", static_cast<int>(config.llm.timeoutMs)));
    config.llm.pollIntervalMs = static_cast<uint32_t>(doc.GetInt("LLM", "poll_interval_ms", static_cast<int>(config.llm.pollIntervalMs)));
    config.llm.retryIntervalMs = static_cast<uint32_t>(doc.GetInt("LLM", "retry_interval_ms", static_cast<int>(config.llm.retryIntervalMs)));
    config.llm.decayHoldSec = static_cast<uint32_t>(doc.GetInt("LLM", "decay_hold_sec", static_cast<int>(config.llm.decayHoldSec)));
    config.llm.maxModelLen = static_cast<uint32_t>(doc.GetInt("LLM", "max_model_len", static_cast<int>(config.llm.maxModelLen)));
    config.llm.filterEmbeddings = doc.GetBool("LLM", "filter_embeddings", config.llm.filterEmbeddings);
    config.llm.minVramMb = static_cast<uint32_t>(doc.GetInt("LLM", "min_vram_mb", static_cast<int>(config.llm.minVramMb)));
    config.llm.targetModel = doc.GetString("LLM", "target_model", config.llm.targetModel);

    // [LLM.Aliases]
    auto aliasKeys = doc.GetKeys("LLM.Aliases");
    for (const auto& k : aliasKeys) {
        config.llm.aliases[k] = doc.GetString("LLM.Aliases", k);
    }

    // [Cluster]
    config.cluster.clusterKey = doc.GetString("Cluster", "cluster_key", config.cluster.clusterKey);
    config.cluster.listenPort = static_cast<uint16_t>(doc.GetInt("Cluster", "listen_port", static_cast<int>(config.cluster.listenPort)));
    config.cluster.allowedRigs = doc.GetString("Cluster", "allowed_rigs", config.cluster.allowedRigs);

    // [Cluster.Aliases]
    config.cluster.aliases.clear();
    auto clusterAliasKeys = doc.GetKeys("Cluster.Aliases");
    for (const auto& k : clusterAliasKeys) {
        config.cluster.aliases[k] = doc.GetString("Cluster.Aliases", k);
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

    doc.SetBool("General", "headless", config.headless);
    doc.SetBool("General", "show_tray_icon", config.showTrayIcon);
    doc.SetInt("Position", "monitor_index", config.monitorIndex);
    doc.SetString("Position", "preset", MonitorHelper::PresetToString(config.preset));
    doc.SetInt("Position", "custom_x", config.customX);
    doc.SetInt("Position", "custom_y", config.customY);
    doc.SetInt("Position", "custom_rel_x", config.customRelX);
    doc.SetInt("Position", "custom_rel_y", config.customRelY);
    doc.SetInt("Position", "margin", config.margin);
    std::string layoutModeStr = (config.layout == OverlayLayout::CustomGrid ? "custom_grid" : (config.layout == OverlayLayout::Grid ? "grid" : "horizontal"));
    std::string gridModeStr = (config.gridMode == GridMode::Islands ? "islands" : "unified");
    std::string orderStr = JoinMetricOrder(config.metricOrder);

    // Canonical [Layout] section
    doc.SetString("Layout", "mode", layoutModeStr);
    doc.RemoveKey("Layout", "layout");
    doc.SetString("Layout", "order", orderStr);
    doc.SetString("Layout", "grid_mode", gridModeStr);
    doc.SetInt("Layout", "cell_gap", config.cellGap);
    doc.SetInt("Layout", "row_gap", config.rowGap);
    doc.SetInt("Layout", "max_gpus_per_row", config.maxGpusPerRow);

    // Clean up any misplaced layout keys from [Position]
    doc.RemoveKey("Position", "layout");
    doc.RemoveKey("Position", "mode");
    doc.RemoveKey("Position", "grid_mode");
    doc.RemoveKey("Position", "cell_gap");
    doc.RemoveKey("Position", "row_gap");
    doc.RemoveKey("Position", "max_gpus_per_row");
    for (int r = 1; r <= 32; ++r) {
        doc.RemoveKey("Position", "grid_row_" + std::to_string(r));
        doc.RemoveKey("Position", "row_" + std::to_string(r));
    }

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
    doc.SetString("Appearance", "symbol_style", config.symbolStyle);
    if (config.hasUserDefinedSeparator) {
        doc.SetString("Appearance", "separator", config.separator.empty() ? "none" : WideToUtf8(config.separator));
    } else {
        doc.SetString("Appearance", "separator", WideToUtf8(config.separator));
    }
    doc.SetInt("Appearance", "opacity", config.opacity);
    doc.RemoveKey("Appearance", "show_tray_icon");

    doc.SetInt("Telemetry", "refresh_interval_ms", static_cast<int>(config.refreshIntervalMs));
    doc.SetString("Telemetry", "gpu_index", config.gpuTarget.empty() ? std::to_string(config.gpuIndex) : config.gpuTarget);
    doc.SetString("Telemetry", "vram_mode", config.vramMode == VramMode::Usable ? "usable" : "physical");
    std::string igpuStr = "usage";
    if (config.igpuVramMode == IgpuVramMode::Shared) igpuStr = "shared";
    else if (config.igpuVramMode == IgpuVramMode::Dedicated) igpuStr = "dedicated";
    doc.SetString("Telemetry", "igpu_vram_mode", igpuStr);
    doc.RemoveKey("Telemetry", "order");
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

    if (!config.cluster.clusterKey.empty()) {
        doc.SetString("Cluster", "cluster_key", config.cluster.clusterKey);
        doc.SetInt("Cluster", "listen_port", static_cast<int>(config.cluster.listenPort));
        if (!config.cluster.allowedRigs.empty()) {
            doc.SetString("Cluster", "allowed_rigs", config.cluster.allowedRigs);
        }
    }

    for (const auto& alias : config.cluster.aliases) {
        doc.SetString("Cluster.Aliases", alias.first, alias.second);
    }

    doc.SetBool("LLM", "enabled", config.llm.enabled);
    doc.SetString("LLM", "endpoints", config.llm.endpoints);
    doc.SetInt("LLM", "timeout_ms", static_cast<int>(config.llm.timeoutMs));
    doc.SetInt("LLM", "poll_interval_ms", static_cast<int>(config.llm.pollIntervalMs));
    doc.SetInt("LLM", "retry_interval_ms", static_cast<int>(config.llm.retryIntervalMs));
    doc.SetInt("LLM", "decay_hold_sec", static_cast<int>(config.llm.decayHoldSec));
    doc.SetInt("LLM", "max_model_len", static_cast<int>(config.llm.maxModelLen));
    doc.SetBool("LLM", "filter_embeddings", config.llm.filterEmbeddings);
    doc.SetInt("LLM", "min_vram_mb", static_cast<int>(config.llm.minVramMb));
    doc.SetString("LLM", "target_model", config.llm.targetModel);

    for (const auto& alias : config.llm.aliases) {
        doc.SetString("LLM.Aliases", alias.first, alias.second);
    }

    bool ok = doc.Save(iniPath);
    if (ok && !config.cluster.clusterKey.empty()) {
        HardenConfigFilePermissions(iniPath);
    }
    return ok;
}

bool ConfigManager::ExportSatelliteIni(const std::string& targetPath, const std::string& clusterKey, const std::string& rigId, const std::string& targetHost, uint16_t targetPort) {
    std::string effectiveRigId = rigId;
    std::string effectiveTargetHost = targetHost;
    uint16_t effectiveTargetPort = targetPort;

    std::error_code ec;
    if (std::filesystem::exists(targetPath, ec)) {
        // Safety backup: preserve pre-existing satellite.ini
        std::filesystem::copy_file(targetPath, targetPath + ".bak", std::filesystem::copy_options::overwrite_existing, ec);

        // Preserve custom user settings (e.g. customized rig_id or target_host)
        IniDocument existingDoc;
        if (existingDoc.Load(targetPath)) {
            std::string existingRig = existingDoc.GetString("Satellite", "rig_id", "");
            if (!existingRig.empty() && rigId == "rig1") {
                effectiveRigId = existingRig;
            }
            std::string existingHost = existingDoc.GetString("Satellite", "target_host", "");
            if (!existingHost.empty() && targetHost == "auto") {
                effectiveTargetHost = existingHost;
            }
            int existingPort = existingDoc.GetInt("Satellite", "target_port", 0);
            if (existingPort > 0 && targetPort == 9889) {
                effectiveTargetPort = static_cast<uint16_t>(existingPort);
            }
        }
    }

    std::ofstream ofs(targetPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;

    // Derive 256-bit Curve25519 public key from private clusterKey
    std::string pubKeyHex;
    uint8_t privKeyBytes[Crypto::KEY_SIZE] = {0};
    if (Crypto::HexToKey(clusterKey, privKeyBytes)) {
        uint8_t pubKeyBytes[Crypto::KEY_SIZE] = {0};
        Crypto::DerivePublicKey(privKeyBytes, pubKeyBytes);
        pubKeyHex = Crypto::KeyToHex(pubKeyBytes);
        Crypto::SecureWipe(privKeyBytes, sizeof(privKeyBytes));
    }

    ofs << "# BareStats Satellite Configuration\n"
        << "# Copy this file alongside BareStats binary on the remote rig.\n"
        << "# Run 'BareStats.exe --service-install' (or 'sudo ./barestats --service-install') to register the service.\n\n"
        << "[Satellite]\n"
        << "# Unique identifier for this remote rig (e.g. rig1, rig2, workstation)\n"
        << "rig_id = " << effectiveRigId << "\n\n"
        << "# Destination IP of your Command Center machine (or 'auto' for subnet UDP broadcast)\n"
        << "target_host = " << effectiveTargetHost << "\n\n"
        << "# Destination UDP port configured on Command Center (default: 9889)\n"
        << "target_port = " << effectiveTargetPort << "\n\n"
        << "# 256-bit Asymmetric Curve25519 Public Key of Command Center (Sealed Box encryption)\n"
        << "# Remote satellites seal telemetry using ephemeral ECDH; only the Command Center's private cluster_key can decrypt it.\n"
        << "public_key = " << pubKeyHex << "\n\n"
        << "# Sampling and telemetry push interval in milliseconds\n"
        << "refresh_interval_ms = 1000\n\n"
        << "# Multi-Target Architecture (Optional):\n"
        << "# If this rig reports to multiple Command Centers (e.g. CenterA and CenterB), define target blocks:\n"
        << "# [Target.CenterA]\n"
        << "# target_host = 192.168.0.10\n"
        << "# target_port = 9889\n"
        << "# public_key = " << pubKeyHex << "\n"
        << "#\n"
        << "# [Target.CenterB]\n"
        << "# target_host = auto\n"
        << "# target_port = 9889\n"
        << "# public_key = <CenterB_public_key_hex>\n";
    return ofs.good();
}

} // namespace BareStats
