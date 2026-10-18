#include "../memory_collector.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

#endif // _WIN32
