#include "telemetry_engine.h"
#include <chrono>

namespace BareStats {

TelemetryEngine::TelemetryEngine(const AppConfig& config)
    : config_(config) {
}

TelemetryEngine::~TelemetryEngine() {
    Stop();
}

SystemMetrics TelemetryEngine::CollectImmediate() {
    SystemMetrics metrics;
    if (config_.showCpu) metrics.cpu = cpuCollector_.Collect();
    if (config_.showRam) metrics.memory = memoryCollector_.Collect();
    if (config_.showGpu) metrics.gpu = gpuCollector_.Collect(config_.gpuIndex);
    if (config_.showNet) metrics.network = netCollector_.Collect();
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        latestMetrics_ = metrics;
    }
    return metrics;
}

void TelemetryEngine::Start(HWND notifyHwnd, UINT notifyMsg) {
    if (isRunning_.load()) return;

    notifyHwnd_ = notifyHwnd;
    notifyMsg_ = notifyMsg;
    isRunning_.store(true);
    isSuspended_.store(false);

    workerThread_ = std::thread(&TelemetryEngine::WorkerLoop, this);
}

void TelemetryEngine::Stop() {
    if (!isRunning_.load()) return;

    isRunning_.store(false);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void TelemetryEngine::SetSuspended(bool suspended) {
    if (isSuspended_.load() == suspended) return;

    isSuspended_.store(suspended);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        isSuspendedChanged_ = true;
        cv_.notify_all();
    }
}

SystemMetrics TelemetryEngine::GetLatestMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    return latestMetrics_;
}

void TelemetryEngine::Recover() {
    gpuCollector_.Recover();
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        isSuspendedChanged_ = true;
        cv_.notify_all();
    }
}

void TelemetryEngine::Shutdown() {
    Stop();
    gpuCollector_.Shutdown();
}

void TelemetryEngine::Wake() {
    std::lock_guard<std::mutex> lock(cvMutex_);
    cv_.notify_all();
}

void TelemetryEngine::WorkerLoop() {
    // Initial warmup
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    while (isRunning_.load()) {
        if (!isSuspended_.load()) {
            SystemMetrics sample;
            if (config_.showCpu) sample.cpu = cpuCollector_.Collect();
            if (config_.showRam) sample.memory = memoryCollector_.Collect();
            if (config_.showGpu) sample.gpu = gpuCollector_.Collect(config_.gpuIndex);
            if (config_.showNet) sample.network = netCollector_.Collect();

            {
                std::lock_guard<std::mutex> lock(metricsMutex_);
                latestMetrics_ = sample;
            }

            if (notifyHwnd_ && notifyMsg_) {
                PostMessageW(notifyHwnd_, notifyMsg_, 0, 0);
            }
        }

        uint32_t interval = (config_.refreshIntervalMs > 0) ? config_.refreshIntervalMs : 1000;
        std::unique_lock<std::mutex> lock(cvMutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(interval), [this] {
            return !isRunning_.load() || (!isSuspended_.load() && isSuspendedChanged_);
        });
        isSuspendedChanged_ = false;
    }
}

} // namespace BareStats
