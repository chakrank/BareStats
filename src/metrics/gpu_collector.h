#pragma once

#include "metrics.h"
#include <memory>
#include <vector>
#include <string>

namespace BareStats {

struct GpuDeviceInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t sharedSystemMemory = 0;
    bool isNvidia = false;
    uint32_t nvidiaDeviceIndex = 0;
    bool isAmd = false;
    uint32_t amdDeviceIndex = 0;
    bool isIntel = false;
    uint32_t intelDeviceIndex = 0;
};

class GpuCollector {
public:
    GpuCollector();
    ~GpuCollector();

    GpuMetrics Collect(uint32_t deviceIndex = 0);
    std::vector<GpuMetrics> CollectMultiple(const std::vector<uint32_t>& targetIndices, bool allGpus = false);
    void Recover();
    void Shutdown();

    static std::vector<GpuDeviceInfo> GetAvailableGpus();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace BareStats
