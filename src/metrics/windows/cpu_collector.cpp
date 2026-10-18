#include "../cpu_collector.h"
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace BareStats {

static uint64_t FileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

CpuCollector::CpuCollector() {
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        prevIdleTime_ = FileTimeToUint64(idleTime);
        prevKernelTime_ = FileTimeToUint64(kernelTime);
        prevUserTime_ = FileTimeToUint64(userTime);
        initialized_ = true;
    }
}

CpuMetrics CpuCollector::Collect() {
    CpuMetrics metrics;

    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return metrics;
    }

    uint64_t idle = FileTimeToUint64(idleTime);
    uint64_t kernel = FileTimeToUint64(kernelTime);
    uint64_t user = FileTimeToUint64(userTime);

    if (!initialized_) {
        prevIdleTime_ = idle;
        prevKernelTime_ = kernel;
        prevUserTime_ = user;
        initialized_ = true;
        return metrics;
    }

    uint64_t diffIdle = idle - prevIdleTime_;
    uint64_t diffKernel = kernel - prevKernelTime_;
    uint64_t diffUser = user - prevUserTime_;

    prevIdleTime_ = idle;
    prevKernelTime_ = kernel;
    prevUserTime_ = user;

    // In Windows, kernelTime includes idleTime
    uint64_t totalSystem = diffKernel + diffUser;
    if (totalSystem > 0) {
        uint64_t totalActive = (totalSystem > diffIdle) ? (totalSystem - diffIdle) : 0;
        double pct = (static_cast<double>(totalActive) / static_cast<double>(totalSystem)) * 100.0;
        metrics.utilizationPercent = std::clamp(pct, 0.0, 100.0);
    }

    return metrics;
}

} // namespace BareStats

#endif // _WIN32
