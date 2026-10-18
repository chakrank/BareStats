#include "../memory_collector.h"

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <algorithm>

namespace BareStats {

MemoryMetrics MemoryCollector::Collect() {
    MemoryMetrics metrics;

    // 1. Total Physical RAM capacity via sysctl
    uint64_t totalMem = 0;
    size_t len = sizeof(totalMem);
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(mib, 2, &totalMem, &len, nullptr, 0) == 0) {
        metrics.totalBytes = totalMem;
    }

    // 2. Mach Virtual Memory statistics
    vm_size_t pageSize = 0;
    if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS || pageSize == 0) {
        long scPage = sysconf(_SC_PAGESIZE);
        pageSize = (scPage > 0) ? static_cast<vm_size_t>(scPage) : 4096;
    }

    vm_statistics64_data_t vmStat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                         reinterpret_cast<host_info64_t>(&vmStat), &count);
    if (kr == KERN_SUCCESS) {
        // macOS Activity Monitor standard memory model:
        // Memory Used = App Memory + Wired Memory + Compressed Memory
        uint64_t appMemory = 0;
        if (vmStat.internal_page_count >= vmStat.purgeable_count) {
            appMemory = static_cast<uint64_t>(vmStat.internal_page_count - vmStat.purgeable_count) * pageSize;
        } else {
            appMemory = static_cast<uint64_t>(vmStat.internal_page_count) * pageSize;
        }

        uint64_t wiredMemory = static_cast<uint64_t>(vmStat.wire_count) * pageSize;
        uint64_t compressedMemory = static_cast<uint64_t>(vmStat.compressor_page_count) * pageSize;
        uint64_t usedMemory = appMemory + wiredMemory + compressedMemory;

        if (totalMem > 0 && usedMemory > totalMem) {
            uint64_t freeMemory = static_cast<uint64_t>(vmStat.free_count + vmStat.speculative_count) * pageSize;
            usedMemory = (totalMem > freeMemory) ? (totalMem - freeMemory) : totalMem;
        }

        metrics.usedBytes = usedMemory;
    }

    return metrics;
}

} // namespace BareStats

#endif // __APPLE__
