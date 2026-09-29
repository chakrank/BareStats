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

private:
    NvmlCollector nvmlCollector_;
    bool TryCollectDxgi(GpuMetrics& metrics, uint32_t deviceIndex);
};

} // namespace BareStats
