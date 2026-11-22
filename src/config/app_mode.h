#pragma once

#include <cstdint>
#include <filesystem>

namespace BareStats {

enum class AppMode : uint8_t {
    CommandCenter,      // GUI + Direct Pull + UDP Listener
    HeadlessSatellite,  // No GUI + Local Hardware/LLM + UDP Push
    Hybrid,             // GUI + Direct Pull + Local Hardware/LLM + UDP Push
    FirstRun            // Generates default config.ini, runs as CommandCenter
};

inline AppMode EvaluateAppMode(bool has_config, bool has_satellite, bool config_headless = false) {
    if (config_headless) {
        return AppMode::HeadlessSatellite;
    }
    if (has_config && !has_satellite) {
        return AppMode::CommandCenter;
    } else if (!has_config && has_satellite) {
        return AppMode::HeadlessSatellite;
    } else if (has_config && has_satellite) {
        return AppMode::Hybrid;
    } else {
        return AppMode::FirstRun;
    }
}

inline AppMode EvaluateAppMode(const std::filesystem::path& exe_dir, const std::string& config_path = "", bool config_headless = false) {
    std::error_code ec;
    std::filesystem::path cfg_dir = !config_path.empty() ? std::filesystem::path(config_path).parent_path() : std::filesystem::path();
    bool has_config = std::filesystem::exists(exe_dir / "config.ini", ec) ||
                      (!config_path.empty() && std::filesystem::exists(config_path, ec)) ||
                      std::filesystem::exists(std::filesystem::current_path(ec) / "config.ini", ec);
    bool has_satellite = std::filesystem::exists(exe_dir / "satellite.ini", ec) ||
                         (!cfg_dir.empty() && std::filesystem::exists(cfg_dir / "satellite.ini", ec)) ||
                         std::filesystem::exists(std::filesystem::current_path(ec) / "satellite.ini", ec) ||
                         std::filesystem::exists("/etc/barestats/satellite.ini", ec);
    return EvaluateAppMode(has_config, has_satellite, config_headless);
}

} // namespace BareStats
