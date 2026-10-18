#pragma once

#include "metrics.h"

namespace BareStats {

class MemoryCollector {
public:
    MemoryCollector() = default;
    ~MemoryCollector() = default;

    MemoryMetrics Collect();
};

} // namespace BareStats
