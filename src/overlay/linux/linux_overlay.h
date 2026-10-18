#pragma once

#include "../config/config.h"
#include "../metrics/metrics.h"
#include "../util/formatter.h"
#include "../util/geometry.h"
#include "monitor_helper.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

namespace BareStats {

class LinuxTray;

enum class LinuxOverlayBackendType {
    None,
    Wayland,
    X11,
    Headless
};

struct LinuxIslandSlot {
    int colIndex = 0;
    int rowIndex = 0;
    std::string token;
    std::wstring lastText;
    int relX = 0;
    int relY = 0;
    int width = 0;
    int height = 0;
    size_t lastHash = 0;
};

class LinuxOverlay {
public:
    explicit LinuxOverlay(AppConfig& config, const std::string& configPath = "");
    ~LinuxOverlay();

    bool Initialize();
    void Show();
    void Hide();
    void ToggleVisibility();
    bool IsVisible() const { return isVisible_; }

    void TogglePositioningMode();
    bool IsPositioningMode() const;
    void CyclePositionPreset();
    void SetPreset(PositionPreset preset);

    void ApplyConfig(const AppConfig& newConfig);
    void UpdateMetrics(const SystemMetrics& metrics);
    void TriggerLaunchAnimation(const SystemMetrics& targetMetrics);
    const SystemMetrics& GetLastMetrics() const;
    void ForceRepaint();

    void Run();
    void Stop();
    void PollEvents();

    // Async-signal safe requests (called from signal handlers)
    static void RequestStop();
    static void RequestToggleVisibility();
    static void RequestTogglePositioningMode();
    static void RequestReloadConfig();

    LinuxOverlayBackendType GetBackendType() const { return backendType_; }
    const AppConfig& GetConfig() const { return config_; }
    const std::string& GetConfigPath() const { return configPath_; }
    void SetConfigPath(const std::string& p) { configPath_ = p; }

    void SetTelemetryCallback(std::function<void()> cb) { onTelemetryReady_ = std::move(cb); }
    void SetConfigReloadCallback(std::function<void()> cb) { onConfigReload_ = std::move(cb); }

    // Dirty-frame hash bypass computation: returns true if frame changed and needs repaint
    static bool ComputeDirtyHash(const std::vector<std::vector<FormattedSegment>>& rows,
                                 size_t& inOutLastHash);

    // Matrix grid layout positioning helper
    static std::vector<LinuxIslandSlot> LayoutIslands(const AppConfig& config,
                                                      const SystemMetrics& metrics,
                                                      const MonitorInfo& monitor,
                                                      int slotPaddingX = 14,
                                                      int slotPaddingY = 6);

private:
    struct Impl;
    AppConfig& config_;
    std::string configPath_;
    std::unique_ptr<Impl> impl_;
    LinuxOverlayBackendType backendType_ = LinuxOverlayBackendType::None;
    bool isVisible_ = true;

    std::function<void()> onTelemetryReady_;
    std::function<void()> onConfigReload_;
    std::atomic<bool> isRunning_{false};

    std::unique_ptr<LinuxTray> tray_;
    void SetupTray();
};

} // namespace BareStats
