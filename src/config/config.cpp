#include "config.h"
#include <shlobj.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>

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

static bool CreateDefaultConfigFile(const std::string& iniPath, const AppConfig& config) {
    std::ostringstream oss;
    oss << "# BareStats Configuration\n\n"
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
        << "# Update interval in milliseconds\n"
        << "refresh_interval_ms = " << config.refreshIntervalMs << "\n\n"
        << "# Selected GPU device index (0 = primary/discrete GPU, 1 = secondary/integrated GPU)\n"
        << "gpu_index = " << config.gpuIndex << "\n\n"
        << "# Metric segment display ordering (comma-separated: CPU, GPU, RAM)\n"
        << "order = " << JoinMetricOrder(config.metricOrder) << "\n\n"
        << "# Toggle specific metrics (1 = show, 0 = hide)\n"
        << "show_cpu = " << (config.showCpu ? "1" : "0") << "\n"
        << "show_gpu = " << (config.showGpu ? "1" : "0") << "\n"
        << "show_ram = " << (config.showRam ? "1" : "0") << "\n"
        << "show_gpu_temp = " << (config.showGpuTemp ? "1" : "0") << "\n"
        << "show_gpu_power = " << (config.showGpuPower ? "1" : "0") << "\n"
        << "show_gpu_fan = " << (config.showGpuFan ? "1" : "0") << "\n"
        << "# Show max memory total (1 = 11.7/16GB, 0 = 11.7GB current only)\n"
        << "show_max_memory = " << (config.showMaxMemory ? "1" : "0") << "\n"
        << "# Automatically hide offline/unavailable metrics after launch (1 = hide, 0 = show N/A)\n"
        << "hide_offline_metrics = " << (config.hideOfflineMetrics ? "1" : "0") << "\n";

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

AppConfig ConfigManager::Load(const std::string& iniPath) {
    AppConfig config; // Built-in binary fallback defaults

    DWORD attrib = GetFileAttributesA(iniPath.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        // Missing config file! Auto-generate with all defaults and return built-in fallback.
        CreateDefaultConfigFile(iniPath, config);
        return config;
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
    if (config.refreshIntervalMs < 100) config.refreshIntervalMs = 1000;

    config.gpuIndex = GetPrivateProfileIntA("Telemetry", "gpu_index", config.gpuIndex, iniPath.c_str());
    if (config.gpuIndex < 0) config.gpuIndex = 0;

    // Metric ordering
    char orderBuf[128] = {0};
    GetPrivateProfileStringA("Telemetry", "order", "CPU, GPU, RAM", orderBuf, sizeof(orderBuf), iniPath.c_str());
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
            if (trimmed == "CPU" || trimmed == "GPU" || trimmed == "RAM") {
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
    config.showGpuTemp = ReadIniBool("Telemetry", "show_gpu_temp", config.showGpuTemp, iniPath);
    config.showGpuPower = ReadIniBool("Telemetry", "show_gpu_power", config.showGpuPower, iniPath);
    config.showGpuFan = ReadIniBool("Telemetry", "show_gpu_fan", config.showGpuFan, iniPath);
    config.showMaxMemory = ReadIniBool("Telemetry", "show_max_memory", config.showMaxMemory, iniPath);
    config.hideOfflineMetrics = ReadIniBool("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics, iniPath);

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
    WritePrivateProfileStringA("Telemetry", "gpu_index", std::to_string(config.gpuIndex).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "order", JoinMetricOrder(config.metricOrder).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_cpu", config.showCpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu", config.showGpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_ram", config.showRam ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_temp", config.showGpuTemp ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_power", config.showGpuPower ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_fan", config.showGpuFan ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_max_memory", config.showMaxMemory ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "hide_offline_metrics", config.hideOfflineMetrics ? "1" : "0", iniPath.c_str());

    return true;
}

} // namespace BareStats
