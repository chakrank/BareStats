#pragma once

#include <filesystem>

namespace BareStats {

// Installs and starts the BareStats Headless Satellite background service/daemon
// using native OS facilities in 100% unprivileged user space:
// - Windows: Task Scheduler with TASK_RUNLEVEL_LUA under user account, with HKCU Run key fallback
// - Linux: systemd user service (~/.config/systemd/user/barestats.service), or DynamicUser sandboxing if root
// - macOS: launchd user LaunchAgent (~/Library/LaunchAgents/com.barestats.satellite.plist)
// Staged into per-user directories with restricted permissions (0700/0600 on POSIX).
int InstallOsService(const std::filesystem::path& source_satellite_ini);

// Uninstalls and removes the BareStats background service/daemon from the user environment.
int UninstallOsService();

// Resolves the canonical unprivileged staging directory for the current operating system.
std::filesystem::path GetServiceStagingDirectory();

// Generates the systemd service unit template. When is_root is true, DynamicUser=yes
// and ConfigurationDirectory=barestats are included (VULN-04).
std::string GenerateSystemdServiceUnit(const std::filesystem::path& exe_path, const std::filesystem::path& work_dir, bool is_root);

// Builds the launchctl bootstrap command with quoted file paths (VULN-05).
std::string BuildLaunchdBootstrapCommand(const std::string& lctl, uint32_t uid, const std::filesystem::path& plist_file);

// Builds the launchctl bootout command with quoted file paths (VULN-05).
std::string BuildLaunchdBootoutCommand(const std::string& lctl, uint32_t uid, const std::filesystem::path& plist_file);

} // namespace BareStats
