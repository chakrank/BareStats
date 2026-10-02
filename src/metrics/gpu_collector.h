#pragma once

#include "metrics.h"
#include "nvml_collector.h"
#include <windows.h>
#include <memory>

namespace BareStats {

class GpuCollector {
public:
    GpuCollector();
    ~GpuCollector();

    GpuMetrics Collect(uint32_t deviceIndex = 0);
    void Recover();
    void Shutdown();

private:
    NvmlCollector nvmlCollector_;
    uint32_t retryCount_ = 0;
    bool TryCollectDxgi(GpuMetrics& metrics, uint32_t deviceIndex);
};

} // namespace BareStats
