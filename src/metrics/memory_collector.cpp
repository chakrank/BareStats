#include "memory_collector.h"

namespace BareStats {

MemoryMetrics MemoryCollector::Collect() {
    MemoryMetrics metrics;

    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memStatus)) {
        metrics.totalBytes = memStatus.ullTotalPhys;
        if (memStatus.ullTotalPhys >= memStatus.ullAvailPhys) {
            metrics.usedBytes = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        }
    }

    return metrics;
}

} // namespace BareStats
