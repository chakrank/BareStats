#include "test_common.h"

#ifdef __APPLE__
#include "../src/metrics/cpu_collector.h"
#include "../src/metrics/memory_collector.h"
#include "../src/metrics/network_collector.h"
#include "../src/metrics/macos/macos_gpu_collector.h"
#include "../src/overlay/macos/macos_overlay.h"

bool Test_Macos_MachCpuCollector() {
    BareStats::CpuCollector cpu;

    // Warm up collector
    BareStats::CpuMetrics m1 = cpu.Collect();
    (void)m1;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    BareStats::CpuMetrics m2 = cpu.Collect();
    TEST_ASSERT(m2.utilizationPercent >= 0.0 && m2.utilizationPercent <= 100.0,
                "Mach CPU utilization must be normalized within [0.0, 100.0]");

    return true;
}

bool Test_Macos_MachMemoryCollector() {
    BareStats::MemoryCollector mem;
    BareStats::MemoryMetrics m = mem.Collect();

    TEST_ASSERT(m.totalBytes > 0, "macOS system memory must be greater than zero");
    TEST_ASSERT(m.usedBytes <= m.totalBytes, "macOS used memory cannot exceed total physical RAM");
    double utilPct = (static_cast<double>(m.usedBytes) / m.totalBytes) * 100.0;
    TEST_ASSERT(utilPct >= 0.0 && utilPct <= 100.0,
                "macOS RAM utilization percent must be in [0.0, 100.0]");

    return true;
}

bool Test_Macos_BsdNetworkCollector() {
    BareStats::NetworkCollector net;

    BareStats::NetworkMetrics m1 = net.Collect();
    (void)m1;

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    BareStats::NetworkMetrics m2 = net.Collect();
    if (m2.isAvailable) {
        TEST_ASSERT(!m2.primaryAdapterName.empty(), "Primary adapter name should not be empty");
        TEST_ASSERT(m2.rxBytesPerSec < 1000000000000ULL, "Inbound throughput sanity check");
        TEST_ASSERT(m2.txBytesPerSec < 1000000000000ULL, "Outbound throughput sanity check");
    }

    return true;
}

bool Test_Macos_MetalAndIOKitGpu() {
    auto gpus = BareStats::MacosGpuCollector::GetAvailableGpus();
    // Metal is available on all modern macOS machines
    if (!gpus.empty()) {
        TEST_ASSERT(!gpus[0].name.empty(), "Metal GPU device name must not be empty");

        BareStats::MacosGpuCollector collector;
        BareStats::GpuMetrics m = collector.Collect(0);
        if (m.isAvailable) {
            TEST_ASSERT(m.memoryTotalBytes > 0, "GPU VRAM / working set quota must be > 0");
            if (m.utilizationPercent.has_value()) {
                TEST_ASSERT(*m.utilizationPercent >= 0.0 && *m.utilizationPercent <= 100.0,
                            "GPU device utilization must be in [0.0, 100.0]");
            }
        }
    }

    return true;
}

#endif // __APPLE__
