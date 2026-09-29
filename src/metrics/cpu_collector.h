#pragma once

#include "metrics.h"
#include <windows.h>

namespace BareStats {

class CpuCollector {
public:
    CpuCollector();
    ~CpuCollector() = default;

    CpuMetrics Collect();

private:
    bool initialized_ = false;
    uint64_t prevIdleTime_ = 0;
    uint64_t prevKernelTime_ = 0;
    uint64_t prevUserTime_ = 0;

    static uint64_t FileTimeToUint64(const FILETIME& ft);
};

} // namespace BareStats
