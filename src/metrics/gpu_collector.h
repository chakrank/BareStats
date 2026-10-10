#pragma once

#include "metrics.h"
#include "nvml_collector.h"
#include "adl_collector.h"
#include "igcl_collector.h"
#include "d3dkmt_collector.h"
#include <windows.h>
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

    std::vector<GpuDeviceInfo> GetAvailableGpus();

private:
    NvmlCollector nvmlCollector_;
    AdlCollector adlCollector_;
    IgclCollector igclCollector_;
    D3dKmtCollector d3dKmtCollector_;
    uint32_t retryCount_ = 0;
};

} // namespace BareStats
