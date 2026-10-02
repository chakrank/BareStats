#pragma once

#include "metrics.h"
#include "cpu_collector.h"
#include "memory_collector.h"
#include "gpu_collector.h"
#include "../config/config.h"
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace BareStats {

class TelemetryEngine {
public:
    explicit TelemetryEngine(const AppConfig& config);
    ~TelemetryEngine();

    // Start background sampling thread
    void Start(HWND notifyHwnd, UINT notifyMsg);

    // Stop background sampling thread
    void Stop();

    // Pause/resume sampling (for display sleep / screen-off / modern standby)
    void SetSuspended(bool suspended);
    bool IsSuspended() const { return isSuspended_.load(); }

    // Thread-safe copy of latest metrics
    SystemMetrics GetLatestMetrics();

    // Direct synchronous collection (used for initial warm-up / CLI mode)
    SystemMetrics CollectImmediate();

    // Health check / driver reset recovery (TDR / sleep resume)
    void Recover();

    // Clean shutdown of worker thread and all collectors
    void Shutdown();

    // Wake worker thread immediately to pick up configuration updates
    void Wake();

private:
    void WorkerLoop();

    const AppConfig& config_;
    CpuCollector cpuCollector_;
    MemoryCollector memoryCollector_;
    GpuCollector gpuCollector_;

    HWND notifyHwnd_ = nullptr;
    UINT notifyMsg_ = 0;

    std::thread workerThread_;
    std::mutex metricsMutex_;
    std::mutex cvMutex_;
    std::condition_variable cv_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isSuspended_{false};
    bool isSuspendedChanged_ = false;

    SystemMetrics latestMetrics_{};
};

} // namespace BareStats
