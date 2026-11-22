#pragma once

#include <filesystem>

namespace BareStats {

// Resolves the physical directory containing the running executable.
// Guaranteed to bypass working directory anomalies from systemd, launchd, or Task Scheduler.
std::filesystem::path GetExecutableDirectory();

// Resolves a safe, writable destination path for exporting satellite.ini.
// Always co-locates satellite.ini in the exact same directory as config.ini.
std::filesystem::path ResolveSatelliteExportPath(const std::string& configPath = "");

// Safely issues desktop notifications on Linux without shell interpolation or command injection risk.
// Completely bypasses /bin/sh via direct fork() + execvp() argument vectors.
void SafeNotifySend(const std::string& title, const std::string& message, const std::string& urgency = "normal");

} // namespace BareStats
