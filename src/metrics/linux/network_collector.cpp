#include "../network_collector.h"
#include "linux_proc_collector.h"

namespace BareStats {

struct NetworkCollector::Impl {
    LinuxProcNetworkCollector collector;
};

NetworkCollector::NetworkCollector() : impl_(std::make_unique<Impl>()) {
    impl_->collector.Collect();
}

NetworkCollector::~NetworkCollector() = default;

NetworkMetrics NetworkCollector::Collect() {
    return impl_->collector.Collect();
}

} // namespace BareStats
