#pragma once

#include "../gpu_collector.h"
#include <vector>
#include <memory>

namespace BareStats {

class MacosGpuCollector {
public:
    MacosGpuCollector();
    ~MacosGpuCollector();

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
