#include "../cpu_collector.h"

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <algorithm>

namespace BareStats {

CpuCollector::CpuCollector() {
    host_cpu_load_info_data_t cpuLoad;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
                                         reinterpret_cast<host_info64_t>(&cpuLoad), &count);
    if (kr == KERN_SUCCESS) {
        prevUserTime_ = cpuLoad.cpu_ticks[CPU_STATE_USER] + cpuLoad.cpu_ticks[CPU_STATE_NICE];
        prevKernelTime_ = cpuLoad.cpu_ticks[CPU_STATE_SYSTEM];
        prevIdleTime_ = cpuLoad.cpu_ticks[CPU_STATE_IDLE];
        initialized_ = true;
    }
}

CpuMetrics CpuCollector::Collect() {
    CpuMetrics metrics;

    host_cpu_load_info_data_t cpuLoad;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
                                         reinterpret_cast<host_info64_t>(&cpuLoad), &count);
    if (kr != KERN_SUCCESS) {
        return metrics;
    }

    uint64_t user = cpuLoad.cpu_ticks[CPU_STATE_USER] + cpuLoad.cpu_ticks[CPU_STATE_NICE];
    uint64_t kernel = cpuLoad.cpu_ticks[CPU_STATE_SYSTEM];
    uint64_t idle = cpuLoad.cpu_ticks[CPU_STATE_IDLE];

    if (!initialized_) {
        prevUserTime_ = user;
        prevKernelTime_ = kernel;
        prevIdleTime_ = idle;
        initialized_ = true;
        return metrics;
    }

    uint64_t diffUser = (user >= prevUserTime_) ? (user - prevUserTime_) : 0;
    uint64_t diffKernel = (kernel >= prevKernelTime_) ? (kernel - prevKernelTime_) : 0;
    uint64_t diffIdle = (idle >= prevIdleTime_) ? (idle - prevIdleTime_) : 0;

    prevUserTime_ = user;
    prevKernelTime_ = kernel;
    prevIdleTime_ = idle;

    uint64_t totalActive = diffUser + diffKernel;
    uint64_t totalSystem = totalActive + diffIdle;

    if (totalSystem > 0) {
        double pct = (static_cast<double>(totalActive) / static_cast<double>(totalSystem)) * 100.0;
        metrics.utilizationPercent = std::clamp(pct, 0.0, 100.0);
    }

    return metrics;
}

} // namespace BareStats

#endif // __APPLE__
