#pragma once

#include "metrics.h"
#include <string>
#include <chrono>

namespace BareStats {

class LinuxProcCpuCollector {
public:
    explicit LinuxProcCpuCollector(std::string procStatPath = "/proc/stat");

    CpuMetrics Collect();
    void Reset();

private:
    std::string procStatPath_;
    bool initialized_ = false;
    uint64_t prevIdleAll_ = 0;
    uint64_t prevTotal_ = 0;
};

class LinuxProcMemoryCollector {
public:
    explicit LinuxProcMemoryCollector(std::string procMeminfoPath = "/proc/meminfo");

    MemoryMetrics Collect();

private:
    std::string procMeminfoPath_;
};

class LinuxProcNetworkCollector {
public:
    explicit LinuxProcNetworkCollector(std::string procNetDevPath = "/proc/net/dev",
                                      std::string procNetRoutePath = "/proc/net/route");

    NetworkMetrics Collect();
    void Reset();

    static bool IsVirtualOrIgnoredInterface(const std::string& iface);
    static std::string DetectDefaultGatewayInterface(const std::string& routePath = "/proc/net/route");

private:
    std::string procNetDevPath_;
    std::string procNetRoutePath_;

    bool initialized_ = false;
    std::string activeInterface_;
    uint64_t prevRxBytes_ = 0;
    uint64_t prevTxBytes_ = 0;
    std::chrono::steady_clock::time_point prevTimePoint_{};
};

} // namespace BareStats
