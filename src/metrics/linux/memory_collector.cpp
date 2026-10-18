#include "../memory_collector.h"
#include "linux_proc_collector.h"

namespace BareStats {

static LinuxProcMemoryCollector g_linuxProcMemoryCollector;

MemoryMetrics MemoryCollector::Collect() {
    return g_linuxProcMemoryCollector.Collect();
}

} // namespace BareStats
