#pragma once

#include "metrics.h"
#include "cpu_collector.h"
#include "memory_collector.h"
#include "gpu_collector.h"
#include "network_collector.h"
#include "network/udp_streamer.h"
#include "metrics/llm/llm_collector.h"
#include "metrics/llm/llm_resolver.h"
#include "metrics/llm/llm_seqlock.h"
#include "overlay/layout_engine.h"
#include "../config/config.h"
#include "../config/app_mode.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BareStats {

class TelemetryEngine {
public:
    TelemetryEngine();
    explicit TelemetryEngine(const AppConfig& config);
    ~TelemetryEngine();

    // Start background sampling thread with cross-platform callback
    void Start(std::function<void()> onUpdateCallback = nullptr);

    // Single-binary AppMode bootstrap with optional UI notification callbacks
    void StartBackgroundWorkers(AppMode mode);
    void StartBackgroundWorkers(AppMode mode, std::function<void()> onUpdateCallback);
#ifdef _WIN32
    void StartBackgroundWorkers(AppMode mode, HWND notifyHwnd, UINT notifyMsg);
#endif

    void StopBackgroundWorkers();
    void RunDaemonLoopForever();
    void RequestShutdown();

    AppMode GetAppMode() const { return appMode_; }
    void SetAppMode(AppMode mode) { appMode_ = mode; }

    RigClusterSnapshot GetClusterSnapshot() const;
    void UpdateClusterSnapshot(const RigClusterSnapshot& cluster);
    bool IsRunning() const { return isRunning_.load(); }
    bool IsReceiverActive() const { return receiverActive_.load(); }
    bool IsTransmitterActive() const { return transmitterActive_.load(); }

    // Direct Pull LLM Scraping & Feed Deduplication
    size_t GetLlmCollectorCount() const;
    bool IsUdpFeedActive(const std::string& rigId) const;
    void RecordUdpFeedActivity(const std::string& rigId, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
    bool CheckUdpFeedTimeouts();
    void UpdateRigInCluster(const std::string& rigId, const LlmSnapshot& snapshot);
    void RebuildLlmCollectors();
    static uint8_t ComputeMaxGpuUtilization(const SystemMetrics& sample);

#ifdef _WIN32
    // Windows Win32 message notification overload
    void Start(HWND notifyHwnd, UINT notifyMsg);
#endif

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

    // Hot-update telemetry configuration
    void UpdateConfig(const AppConfig& config);

private:
    void WorkerLoop();
    void InitializeModeWorkers();

    AppConfig config_;
    std::mutex configMutex_;
    CpuCollector cpuCollector_;
    MemoryCollector memoryCollector_;
    GpuCollector gpuCollector_;
    NetworkCollector netCollector_;

    std::function<void()> onUpdateCallback_;

#ifdef _WIN32
    HWND notifyHwnd_ = nullptr;
    UINT notifyMsg_ = 0;
#endif

    std::thread workerThread_;
    std::mutex metricsMutex_;
    std::mutex cvMutex_;
    std::condition_variable cv_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isSuspended_{false};
    bool isSuspendedChanged_ = false;

    AppMode appMode_ = AppMode::FirstRun;
    std::atomic<bool> daemonRunning_{false};

    // Lock-Free Publication via Seqlock
    mutable ClusterSeqlock clusterSeqlock_;
    mutable std::mutex clusterUpdateMutex_;

    SystemMetrics latestMetrics_{};

    // Direct Pull HTTP scrapers (one per configured remote/local endpoint)
    std::vector<std::unique_ptr<LlmCollector>> llmCollectors_;
    mutable std::mutex llmCollectorsMutex_;

    // Feed Deduplication Tracker: rig_id -> expiration timestamp
    // If an authenticated UDP satellite feed is actively streaming for rig1,
    // the worker loop suppresses Direct Pull HTTP scraping for rig1.
    mutable std::mutex feedDedupMutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> activeUdpFeeds_;

    UdpTransmitter transmitter_;
    std::atomic<bool> transmitterActive_{false};
    std::string transmitterRigId_;
    UdpReceiver receiver_;
    std::atomic<bool> receiverActive_{false};
    std::thread udpRxThread_;
    void UdpRxLoop();
};

} // namespace BareStats
