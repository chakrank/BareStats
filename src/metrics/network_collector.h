#pragma once

#include "metrics.h"
#include <memory>

namespace BareStats {

class NetworkCollector {
public:
    NetworkCollector();
    ~NetworkCollector();

    NetworkMetrics Collect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace BareStats
