#pragma once

#include "metrics.h"
#include <windows.h>

namespace BareStats {

class MemoryCollector {
public:
    MemoryCollector() = default;
    ~MemoryCollector() = default;

    MemoryMetrics Collect();
};

} // namespace BareStats
