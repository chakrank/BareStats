#include "../cpu_collector.h"
#include "linux_proc_collector.h"

namespace BareStats {

static LinuxProcCpuCollector g_linuxProcCpuCollector;

CpuCollector::CpuCollector() {
    g_linuxProcCpuCollector.Collect();
}

CpuMetrics CpuCollector::Collect() {
    return g_linuxProcCpuCollector.Collect();
}

} // namespace BareStats
