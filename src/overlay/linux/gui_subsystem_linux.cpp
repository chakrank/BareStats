#include "gui_subsystem_linux.h"
#include "metrics/telemetry_engine.h"
#include <iostream>
#include <cstdlib>
#include <filesystem>

#if defined(__linux__)
#include <dlfcn.h>

namespace BareStats {

bool CheckGuiSubsystemAvailable() {
    // 1. Check environment variables for display server presence
    const char* disp = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if (!disp && !wayland) {
        // Also check if an active X11/Xwayland socket exists in /tmp/.X11-unix
        std::error_code ec;
        bool hasX11Socket = false;
        if (std::filesystem::exists("/tmp/.X11-unix", ec)) {
            for (const auto& entry : std::filesystem::directory_iterator("/tmp/.X11-unix", ec)) {
                std::string fname = entry.path().filename().string();
                if (fname.rfind("X", 0) == 0 && fname.size() > 1) {
                    hasX11Socket = true;
                    break;
                }
            }
        }
        if (!hasX11Socket) {
            return false;
        }
    }

    // 2. Dynamically test dlopen on libX11.so.6
    void* x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_lib) {
        x11_lib = dlopen("libX11.so", RTLD_LAZY);
    }
    if (!x11_lib) {
        return false;
    }
    dlclose(x11_lib);
    return true;
}

bool InitializeGuiSubsystem(TelemetryEngine& telemetry, const AppConfig& config, const std::string& configPath) {
    (void)telemetry;
    (void)config;
    (void)configPath;
    if (!CheckGuiSubsystemAvailable()) {
        std::cerr << "Command Center requires a display server (X11/Wayland).\n"
                  << "If running as a headless satellite, ensure 'satellite.ini' exists in the executable folder.\n";
        return false;
    }
    return true;
}

} // namespace BareStats

#else

namespace BareStats {

bool CheckGuiSubsystemAvailable() {
    return true;
}

bool InitializeGuiSubsystem(TelemetryEngine& telemetry, const AppConfig& config, const std::string& configPath) {
    (void)telemetry;
    (void)config;
    (void)configPath;
    return true;
}

} // namespace BareStats

#endif
