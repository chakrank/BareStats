#ifdef _WIN32

#include "test_common.h"
#include "../src/metrics/gpu_collector.h"
#include "../src/metrics/windows/d3dkmt_collector.h"
#include "../src/metrics/windows/adl_collector.h"
#include "../src/metrics/windows/igcl_collector.h"
#include "../src/metrics/nvml_collector.h"
#include <windows.h>

// 1. Universal Windows Graphics (DXGI adapter enumeration + D3DKMT engine scheduler)
bool Test_UniversalGraphics_D3dKmtAndDxgi() {
    BareStats::GpuCollector gpuCollector;
    auto gpus = gpuCollector.GetAvailableGpus();

    for (const auto& g : gpus) {
        TEST_ASSERT(!g.name.empty(), "GPU name should not be empty");
    }

    BareStats::D3dKmtCollector d3dKmt;
    LUID fakeLuid{ 0, 0 };
    auto util = d3dKmt.QueryEngineUtilization(fakeLuid);
    TEST_ASSERT(!util.has_value(), "Fake LUID must return nullopt gracefully");

    return true;
}

// 2. AMD ADL Mock Pipeline
bool Test_AmdAdl_MockPipeline() {
    std::string mockDll = GetTestModuleDir() + "mock_atiadlxx.dll";
    BareStats::AdlCollector adl(mockDll.c_str());

    TEST_ASSERT(adl.IsAvailable(), "AdlCollector must load mock_atiadlxx.dll successfully");

    BareStats::GpuMetrics m{};
    bool collected = adl.Collect(m, 0);
    TEST_ASSERT(collected, "AdlCollector::Collect must succeed against mock ADL");
    TEST_ASSERT(m.isAvailable, "Metrics must be marked available");
    TEST_ASSERT(m.temperatureC.has_value() && *m.temperatureC == 65, "Temperature mismatch (65C expected)");
    TEST_ASSERT(m.powerWatts.has_value() && *m.powerWatts == 185, "Power wattage mismatch (185W expected)");
    TEST_ASSERT(m.fanRpm.has_value() && *m.fanRpm == 1600, "Fan RPM mismatch (1600 expected)");
    TEST_ASSERT(m.utilizationPercent.has_value() && *m.utilizationPercent == 78.0, "Activity percent mismatch (78% expected)");
    TEST_ASSERT(m.memoryTotalBytes == 24ULL * 1024 * 1024 * 1024, "Total VRAM mismatch (24GB expected)");

    // Lifecycle testing: Shutdown and Recover
    adl.Shutdown();
    TEST_ASSERT(!adl.IsAvailable(), "AdlCollector must be unavailable after Shutdown()");

    bool recovered = adl.Recover();
    TEST_ASSERT(recovered && adl.IsAvailable(), "AdlCollector must recover successfully");

    return true;
}

// 3. Intel IGCL Mock Pipeline
bool Test_IntelIgcl_MockPipeline() {
    std::string mockDll = GetTestModuleDir() + "mock_ControlLib.dll";
    BareStats::IgclCollector igcl(mockDll.c_str());

    TEST_ASSERT(igcl.IsAvailable(), "IgclCollector must load mock_ControlLib.dll successfully");

    BareStats::GpuMetrics m{};
    bool collected = igcl.Collect(m, 0);
    TEST_ASSERT(collected, "IgclCollector::Collect must succeed against mock IGCL");
    TEST_ASSERT(m.isAvailable, "Metrics must be marked available");
    TEST_ASSERT(m.temperatureC.has_value() && *m.temperatureC == 58, "Temperature mismatch (58C expected)");
    TEST_ASSERT(m.powerWatts.has_value() && *m.powerWatts == 140, "Power wattage mismatch (140W expected)");
    TEST_ASSERT(m.fanRpm.has_value() && *m.fanRpm == 1200, "Fan RPM mismatch (1200 expected)");
    TEST_ASSERT(m.fanSpeedPercent.has_value() && *m.fanSpeedPercent == 45, "Fan percent mismatch (45% expected)");

    // Lifecycle testing: Shutdown and Recover
    igcl.Shutdown();
    TEST_ASSERT(!igcl.IsAvailable(), "IgclCollector must be unavailable after Shutdown()");

    bool recovered = igcl.Recover();
    TEST_ASSERT(recovered && igcl.IsAvailable(), "IgclCollector must recover successfully");

    return true;
}

// 4. Driver Fallback & Graceful Recovery
bool Test_DriverFallback_GracefulRecovery() {
    BareStats::NvmlCollector badNvml("nonexistent_nvml_file.dll");
    TEST_ASSERT(!badNvml.IsAvailable(), "Nonexistent NVML DLL must fail gracefully");
    BareStats::GpuMetrics m1{};
    TEST_ASSERT(!badNvml.Collect(m1, 0), "Collect on invalid NVML must return false without crash");

    BareStats::AdlCollector badAdl("nonexistent_adl_file.dll");
    TEST_ASSERT(!badAdl.IsAvailable(), "Nonexistent ADL DLL must fail gracefully");
    BareStats::GpuMetrics m2{};
    TEST_ASSERT(!badAdl.Collect(m2, 0), "Collect on invalid ADL must return false without crash");

    BareStats::IgclCollector badIgcl("nonexistent_igcl_file.dll");
    TEST_ASSERT(!badIgcl.IsAvailable(), "Nonexistent IGCL DLL must fail gracefully");
    BareStats::GpuMetrics m3{};
    TEST_ASSERT(!badIgcl.Collect(m3, 0), "Collect on invalid IGCL must return false without crash");

    return true;
}

// 5. Windows Mocks Null and Error Injection Resilience
bool Test_WindowsMocks_NullAndErrorInjection() {
    std::string mockNvmlDll = GetTestModuleDir() + "mock_nvml.dll";
    std::string mockAdlDll = GetTestModuleDir() + "mock_atiadlxx.dll";
    std::string mockIgclDll = GetTestModuleDir() + "mock_ControlLib.dll";

    // 1. Rapid Reload Stress Testing (verify no resource leaks or DLL unload locks)
    for (int i = 0; i < 5; ++i) {
        BareStats::NvmlCollector nvml(mockNvmlDll.c_str());
        TEST_ASSERT(nvml.IsAvailable(), "Nvml reload iteration must succeed");
        nvml.Shutdown();

        BareStats::AdlCollector adl(mockAdlDll.c_str());
        TEST_ASSERT(adl.IsAvailable(), "Adl reload iteration must succeed");
        adl.Shutdown();

        BareStats::IgclCollector igcl(mockIgclDll.c_str());
        TEST_ASSERT(igcl.IsAvailable(), "Igcl reload iteration must succeed");
        igcl.Shutdown();
    }

    // 2. Out-of-bounds adapter index and shutdown state error handling
    BareStats::NvmlCollector nvml(mockNvmlDll.c_str());
    BareStats::GpuMetrics nvmlMetrics;
    bool nvmlOob = nvml.Collect(nvmlMetrics, 9999);
    TEST_ASSERT(!nvmlOob, "Out-of-bounds NVML index (9999) must fail gracefully");

    // Post-shutdown collect resilience
    nvml.Shutdown();
    TEST_ASSERT(!nvml.Collect(nvmlMetrics, 0), "Collect after NVML shutdown must return false");

    BareStats::AdlCollector adl(mockAdlDll.c_str());
    adl.Shutdown();
    BareStats::GpuMetrics adlMetrics;
    TEST_ASSERT(!adl.Collect(adlMetrics, 0), "Collect after ADL shutdown must return false");

    BareStats::IgclCollector igcl(mockIgclDll.c_str());
    igcl.Shutdown();
    BareStats::GpuMetrics igclMetrics;
    TEST_ASSERT(!igcl.Collect(igclMetrics, 0), "Collect after IGCL shutdown must return false");

    // 3. D3DKMT invalid LUID and uninitialized handle resilience
    BareStats::D3dKmtCollector d3dkmt;
    LUID invalidLuid{ 0xFFFFFFFF, 0x7FFFFFFF };
    auto util = d3dkmt.QueryEngineUtilization(invalidLuid);
    TEST_ASSERT(!util.has_value(), "Invalid D3DKMT LUID must return nullopt");

    return true;
}

#endif // _WIN32

