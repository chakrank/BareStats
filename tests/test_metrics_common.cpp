#include "test_common.h"
#include "../src/metrics/cpu_collector.h"
#include "../src/metrics/memory_collector.h"
#include "../src/metrics/network_collector.h"
#include "../src/metrics/nvml_collector.h"
#include "../src/metrics/telemetry_engine.h"
#include <atomic>

// 1. CPU Collector verification
bool Test_CpuCollector_RealHardware() {
    BareStats::CpuCollector cpu;

    // Warm up collector
    BareStats::CpuMetrics m1 = cpu.Collect();
    (void)m1;

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    BareStats::CpuMetrics m2 = cpu.Collect();
    TEST_ASSERT(m2.utilizationPercent >= 0.0 && m2.utilizationPercent <= 100.0,
                "CPU utilization must be normalized within [0.0, 100.0]");

    return true;
}

// 2. Memory Collector verification
bool Test_MemoryCollector_RealHardware() {
    BareStats::MemoryCollector mem;
    BareStats::MemoryMetrics m = mem.Collect();

    TEST_ASSERT(m.totalBytes > 0, "System total memory must be greater than zero");
    TEST_ASSERT(m.usedBytes <= m.totalBytes, "Used memory cannot exceed total memory");
    double utilPct = (static_cast<double>(m.usedBytes) / m.totalBytes) * 100.0;
    TEST_ASSERT(utilPct >= 0.0 && utilPct <= 100.0,
                "RAM utilization percent must be in [0.0, 100.0]");

    return true;
}

// 3. Network Collector verification
bool Test_NetworkCollector_RealHardware() {
    BareStats::NetworkCollector net;

    BareStats::NetworkMetrics m1 = net.Collect();
    (void)m1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BareStats::NetworkMetrics m2 = net.Collect();

    if (m2.isAvailable) {
        TEST_ASSERT(m2.rxBytesPerSec < 1000000000000ULL, "Inbound speed sanity check");
        TEST_ASSERT(m2.txBytesPerSec < 1000000000000ULL, "Outbound speed sanity check");
    }

    return true;
}

// 4. NVIDIA NVML Mock Pipeline
bool Test_NvidiaNvml_MockPipeline() {
#ifdef _WIN32
    std::string mockDll = GetTestModuleDir() + "mock_nvml.dll";
#elif defined(__APPLE__)
    std::string mockDll = GetTestModuleDir() + "libmock_nvml.dylib";
#else
    std::string mockDll = GetTestModuleDir() + "libmock_nvml.so";
#endif
    BareStats::NvmlCollector nvml(mockDll.c_str());

    TEST_ASSERT(nvml.IsAvailable(), "NvmlCollector must load mock NVML library successfully");

    BareStats::GpuMetrics m{};
    bool collected = nvml.Collect(m, 0);
    TEST_ASSERT(collected, "NvmlCollector::Collect must succeed against mock NVML");
    TEST_ASSERT(m.isAvailable, "Metrics must be marked available");
    TEST_ASSERT(m.name == "NVIDIA GeForce RTX 4090", "GPU name mismatch in mock NVML");
    TEST_ASSERT(m.utilizationPercent.has_value() && *m.utilizationPercent == 85.0, "Utilization mismatch");
    TEST_ASSERT(m.temperatureC.has_value() && *m.temperatureC == 62, "Temperature mismatch");
    TEST_ASSERT(m.powerWatts.has_value() && *m.powerWatts == 285, "Power wattage mismatch");
    TEST_ASSERT(m.fanSpeedPercent.has_value() && *m.fanSpeedPercent == 55, "Fan speed % mismatch");
    TEST_ASSERT(m.memoryTotalBytes == 24ULL * 1024 * 1024 * 1024, "Total VRAM mismatch");
    TEST_ASSERT(m.memoryUsedBytes == 6ULL * 1024 * 1024 * 1024, "Used VRAM mismatch");
    TEST_ASSERT(m.memoryReservedBytes == 280ULL * 1024 * 1024, "Reserved VRAM mismatch");

    // Lifecycle testing: Shutdown and Recover
    nvml.Shutdown();
    TEST_ASSERT(!nvml.IsAvailable(), "NvmlCollector must be unavailable after Shutdown()");

    bool recovered = nvml.Recover();
    TEST_ASSERT(recovered && nvml.IsAvailable(), "NvmlCollector must recover successfully");

    return true;
}

// 5. TelemetryEngine Cross-Platform Callback Notification
bool Test_TelemetryEngine_CallbackNotification() {
    BareStats::AppConfig config;
    config.refreshIntervalMs = 50; // fast for test
    config.showCpu = true;
    config.showRam = true;
    config.showGpu = false;
    config.showNet = false;

    BareStats::TelemetryEngine engine(config);
    std::atomic<int> callbackCount{ 0 };

    engine.Start([&callbackCount]() {
        callbackCount.fetch_add(1);
    });

    for (int i = 0; i < 40 && callbackCount.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    engine.Stop();

    TEST_ASSERT(callbackCount.load() >= 2, "TelemetryEngine cross-platform callback must fire repeatedly");
    auto latest = engine.GetLatestMetrics();
    TEST_ASSERT(latest.cpu.utilizationPercent >= 0.0, "Latest metrics must be populated");

    return true;
}

// 6. TelemetryEngine Rapid Burst Stress and Dynamic Hot-Reload
bool Test_TelemetryEngine_RapidBurstStressAndReload() {
    BareStats::AppConfig cfg;
    cfg.showCpu = true;
    cfg.showRam = true;
    cfg.showGpu = false;
    cfg.showNet = true;
    cfg.refreshIntervalMs = 40;

    BareStats::TelemetryEngine engine(cfg);

    // 1. Rapid burst sampling without delay (50 continuous iterations)
    auto tStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        auto m = engine.CollectImmediate();
        TEST_ASSERT(m.cpu.utilizationPercent >= 0.0 && m.cpu.utilizationPercent <= 100.0,
                    "Burst CPU utilization must remain normalized in [0.0, 100.0]");
        TEST_ASSERT(m.memory.totalBytes > 0, "Burst RAM totalBytes must be non-zero");
        TEST_ASSERT(m.memory.usedBytes <= m.memory.totalBytes, "Burst RAM usedBytes must be <= totalBytes");
    }
    auto tEnd = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    TEST_ASSERT(elapsedMs < 10000, "50 rapid burst cycles must complete well within 10 seconds");

    // 2. Active Polling & Dynamic Hot-Reload
    std::atomic<int> updateCount{ 0 };
    engine.Start([&updateCount]() {
        updateCount.fetch_add(1);
    });

    // Wait for at least one background poll
    for (int i = 0; i < 50 && updateCount.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_ASSERT(updateCount.load() >= 1, "Engine must execute initial background poll");

    // Dynamic configuration update while background thread is actively looping
    BareStats::AppConfig updatedCfg = cfg;
    updatedCfg.refreshIntervalMs = 30;
    updatedCfg.showNet = false;
    engine.UpdateConfig(updatedCfg);
    engine.Wake();

    int countAfterReload = updateCount.load();
    for (int i = 0; i < 50 && updateCount.load() <= countAfterReload; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_ASSERT(updateCount.load() > countAfterReload,
                "Engine must continue polling seamlessly after dynamic UpdateConfig");

    // Test suspend and resume transitions
    engine.SetSuspended(true);
    TEST_ASSERT(engine.IsSuspended(), "Engine must report suspended state");
    engine.SetSuspended(false);
    TEST_ASSERT(!engine.IsSuspended(), "Engine must report active state after unsuspend");

    engine.Stop();
    return true;
}

