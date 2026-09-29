#include "config.h"
#include <sstream>
#include <iomanip>
#include <vector>

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

std::string ConfigManager::GetDefaultConfigPath() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = exePath;
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return path.substr(0, lastSlash + 1) + "config.ini";
    }
    return "config.ini";
}

AppConfig ConfigManager::Load(const std::string& iniPath) {
    AppConfig config;

    char buf[256] = {0};

    // [Position]
    config.monitorIndex = GetPrivateProfileIntA("Position", "monitor_index", 0, iniPath.c_str());
    
    GetPrivateProfileStringA("Position", "preset", "bottom-center", buf, sizeof(buf), iniPath.c_str());
    config.preset = MonitorHelper::StringToPreset(buf);

    config.customX = GetPrivateProfileIntA("Position", "custom_x", -1, iniPath.c_str());
    config.customY = GetPrivateProfileIntA("Position", "custom_y", -1, iniPath.c_str());
    config.margin = GetPrivateProfileIntA("Position", "margin", 12, iniPath.c_str());

    // [Appearance]
    GetPrivateProfileStringA("Appearance", "font_name", "Consolas", buf, sizeof(buf), iniPath.c_str());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wfont(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, &wfont[0], wlen);
        config.fontFace = wfont;
    }

    config.fontSize = GetPrivateProfileIntA("Appearance", "font_size", 14, iniPath.c_str());

    GetPrivateProfileStringA("Appearance", "text_color", "#DCDCDC", buf, sizeof(buf), iniPath.c_str());
    config.textColor = HexToColor(buf, RGB(220, 220, 220));

    GetPrivateProfileStringA("Appearance", "color_high", "#FFD700", buf, sizeof(buf), iniPath.c_str());
    config.colorHigh = HexToColor(buf, RGB(255, 215, 0));

    GetPrivateProfileStringA("Appearance", "color_critical", "#FF4D4D", buf, sizeof(buf), iniPath.c_str());
    config.colorCritical = HexToColor(buf, RGB(255, 77, 77));

    config.enableDynamicColors = (GetPrivateProfileIntA("Appearance", "enable_dynamic_colors", 1, iniPath.c_str()) != 0);

    config.dropShadow = (GetPrivateProfileIntA("Appearance", "drop_shadow", 1, iniPath.c_str()) != 0);

    // [Telemetry]
    config.refreshIntervalMs = static_cast<uint32_t>(GetPrivateProfileIntA("Telemetry", "refresh_interval_ms", 1000, iniPath.c_str()));
    if (config.refreshIntervalMs < 100) config.refreshIntervalMs = 1000;

    config.showCpu = (GetPrivateProfileIntA("Telemetry", "show_cpu", 1, iniPath.c_str()) != 0);
    config.showGpu = (GetPrivateProfileIntA("Telemetry", "show_gpu", 1, iniPath.c_str()) != 0);
    config.showRam = (GetPrivateProfileIntA("Telemetry", "show_ram", 1, iniPath.c_str()) != 0);
    config.showGpuTemp = (GetPrivateProfileIntA("Telemetry", "show_gpu_temp", 1, iniPath.c_str()) != 0);
    config.showGpuPower = (GetPrivateProfileIntA("Telemetry", "show_gpu_power", 1, iniPath.c_str()) != 0);
    config.showGpuFan = (GetPrivateProfileIntA("Telemetry", "show_gpu_fan", 1, iniPath.c_str()) != 0);

    return config;
}

bool ConfigManager::Save(const std::string& iniPath, const AppConfig& config) {
    WritePrivateProfileStringA("Position", "monitor_index", std::to_string(config.monitorIndex).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "preset", MonitorHelper::PresetToString(config.preset).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_x", std::to_string(config.customX).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "custom_y", std::to_string(config.customY).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Position", "margin", std::to_string(config.margin).c_str(), iniPath.c_str());

    // Convert fontFace to UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, 0, config.fontFace.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulen > 0) {
        std::string ufont(ulen - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, config.fontFace.c_str(), -1, &ufont[0], ulen, nullptr, nullptr);
        WritePrivateProfileStringA("Appearance", "font_name", ufont.c_str(), iniPath.c_str());
    }

    WritePrivateProfileStringA("Appearance", "font_size", std::to_string(config.fontSize).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "text_color", ColorToHex(config.textColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "color_high", ColorToHex(config.colorHigh).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "color_critical", ColorToHex(config.colorCritical).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "enable_dynamic_colors", config.enableDynamicColors ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "drop_shadow", config.dropShadow ? "1" : "0", iniPath.c_str());

    WritePrivateProfileStringA("Telemetry", "refresh_interval_ms", std::to_string(config.refreshIntervalMs).c_str(), iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_cpu", config.showCpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu", config.showGpu ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_ram", config.showRam ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_temp", config.showGpuTemp ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_power", config.showGpuPower ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("Telemetry", "show_gpu_fan", config.showGpuFan ? "1" : "0", iniPath.c_str());

    return true;
}

} // namespace BareStats
