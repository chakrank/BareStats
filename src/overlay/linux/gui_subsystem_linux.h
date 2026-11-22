#pragma once

#include "config/config.h"
#include <string>

namespace BareStats {

class TelemetryEngine;

// Checks whether an active display server (X11/Wayland) and required GUI libraries
// can be resolved dynamically via dlopen without crashing the headless process.
bool CheckGuiSubsystemAvailable();

// Initializes the GUI subsystem if display server is present.
// Returns false on headless server environments, displaying informative diagnostics.
bool InitializeGuiSubsystem(TelemetryEngine& telemetry, const AppConfig& config, const std::string& configPath);

} // namespace BareStats
