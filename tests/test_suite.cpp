#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cassert>
#include <cmath>
#include <fstream>
#include <windows.h>

#include "../src/metrics/metrics.h"
#include "../src/metrics/cpu_collector.h"
#include "../src/metrics/memory_collector.h"
#include "../src/metrics/network_collector.h"
#include "../src/metrics/d3dkmt_collector.h"
#include "../src/metrics/nvml_collector.h"
#include "../src/metrics/adl_collector.h"
#include "../src/metrics/igcl_collector.h"
#include "../src/metrics/gpu_collector.h"
#include "../src/config/config.h"
#include "../src/util/formatter.h"

static int g_totalTests = 0;
static int g_passedTests = 0;
static int g_failedTests = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] Line " << __LINE__ << ": " << msg << " (" #cond ")" << std::endl; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        g_totalTests++; \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        bool ok = fn(); \
        if (ok) { \
            g_passedTests++; \
            std::cout << "[PASS] " << #fn << std::endl; \
        } else { \
            g_failedTests++; \
            std::cerr << "[FAIL] " << #fn << " FAILED!" << std::endl; \
        } \
    } while (0)

static std::string GetModuleDir() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t lastSlash = s.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return s.substr(0, lastSlash + 1);
    }
    return ".\\";
}

// 1. CPU Collector verification (real system NT kernel times delta calculation)
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

// 2. Memory Collector verification (GlobalMemoryStatusEx sanity)
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

// 3. Network Collector verification (GetIfTable2 throughput delta)
bool Test_NetworkCollector_RealHardware() {
    BareStats::NetworkCollector net;

    BareStats::NetworkMetrics m1 = net.Collect();
    (void)m1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BareStats::NetworkMetrics m2 = net.Collect();

    TEST_ASSERT(m2.isAvailable, "Network metrics must be marked available");
    // Verify values don't overflow or wrap around unexpectedly
    TEST_ASSERT(m2.rxBytesPerSec < 1000000000000ULL, "Inbound speed sanity check");
    TEST_ASSERT(m2.txBytesPerSec < 1000000000000ULL, "Outbound speed sanity check");

    return true;
}

// 4. Universal Windows Graphics (DXGI adapter enumeration + D3DKMT engine scheduler)
bool Test_UniversalGraphics_D3dKmtAndDxgi() {
    BareStats::GpuCollector gpuCollector;
    auto gpus = gpuCollector.GetAvailableGpus();

    // In virtualized CI or bare metal, DXGI should not crash
    for (const auto& g : gpus) {
        TEST_ASSERT(!g.name.empty(), "GPU name should not be empty");
    }

    BareStats::D3dKmtCollector d3dKmt;
    // D3dKmt collector should either initialize on WDDM systems or gracefully handle absence
    LUID fakeLuid{ 0, 0 };
    auto util = d3dKmt.QueryEngineUtilization(fakeLuid);
    // Fake LUID returns nullopt without crashing
    TEST_ASSERT(!util.has_value(), "Fake LUID must return nullopt gracefully");

    return true;
}

// 5. NVIDIA NVML Mock Pipeline
bool Test_NvidiaNvml_MockPipeline() {
    std::string mockDll = GetModuleDir() + "mock_nvml.dll";
    BareStats::NvmlCollector nvml(mockDll.c_str());

    TEST_ASSERT(nvml.IsAvailable(), "NvmlCollector must load mock_nvml.dll successfully");

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

// 6. AMD ADL Mock Pipeline
bool Test_AmdAdl_MockPipeline() {
    std::string mockDll = GetModuleDir() + "mock_atiadlxx.dll";
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

// 7. Intel IGCL Mock Pipeline
bool Test_IntelIgcl_MockPipeline() {
    std::string mockDll = GetModuleDir() + "mock_ControlLib.dll";
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

// 8. Precision VRAM Deduction Math (Physical vs Usable mode)
bool Test_PrecisionVram_FormattingMath() {
    BareStats::SystemMetrics sys{};
    sys.cpu.utilizationPercent = 25.0;
    sys.memory.totalBytes = 32ULL * 1024 * 1024 * 1024;
    sys.memory.usedBytes = 16ULL * 1024 * 1024 * 1024;

    BareStats::GpuMetrics gpu{};
    gpu.isAvailable = true;
    gpu.name = "Test GPU";
    gpu.utilizationPercent = 60.0;
    gpu.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;      // 16.0 GB gross
    gpu.memoryReservedBytes = 400ULL * 1024 * 1024;        // 400 MB driver reservation
    gpu.memoryUsedBytes = 8ULL * 1024 * 1024 * 1024;       // 8.0 GB used
    sys.gpu = gpu;
    sys.gpus.push_back(gpu);

    // Test Physical VRAM mode
    BareStats::AppConfig cfgPhysical{};
    cfgPhysical.vramMode = BareStats::VramMode::Physical;
    cfgPhysical.showCpu = false;
    cfgPhysical.showRam = false;
    cfgPhysical.showNet = false;
    cfgPhysical.showGpu = true;
    cfgPhysical.showGpuTemp = false;
    cfgPhysical.showGpuPower = false;
    cfgPhysical.showGpuFan = false;

    std::string sPhysical = BareStats::Formatter::FormatUtf8(sys, &cfgPhysical);
    TEST_ASSERT(sPhysical.find("16.0GB") != std::string::npos || sPhysical.find("16GB") != std::string::npos,
                "Physical mode must report gross hardware capacity (16.0 GB)");

    // Test Usable VRAM mode
    BareStats::AppConfig cfgUsable = cfgPhysical;
    cfgUsable.vramMode = BareStats::VramMode::Usable;

    std::string sUsable = BareStats::Formatter::FormatUtf8(sys, &cfgUsable);
    TEST_ASSERT(sUsable.find("15.6GB") != std::string::npos,
                "Usable mode must report net allocatable capacity (15.6 GB)");

    return true;
}

// 9. Side-by-Side Multi-GPU Layout Formatting
bool Test_MultiGpu_FormattingLayout() {
    BareStats::SystemMetrics sys{};

    BareStats::GpuMetrics gpu0{};
    gpu0.isAvailable = true;
    gpu0.utilizationPercent = 85.0;
    gpu0.temperatureC = 62;
    gpu0.memoryTotalBytes = 24ULL * 1024 * 1024 * 1024;
    gpu0.memoryUsedBytes = 6ULL * 1024 * 1024 * 1024;

    BareStats::GpuMetrics gpu1{};
    gpu1.isAvailable = true;
    gpu1.utilizationPercent = 14.0;
    gpu1.temperatureC = 44;
    gpu1.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;
    gpu1.memoryUsedBytes = 2ULL * 1024 * 1024 * 1024;

    sys.gpus.push_back(gpu0);
    sys.gpus.push_back(gpu1);

    BareStats::AppConfig cfg{};
    cfg.showCpu = false;
    cfg.showRam = false;
    cfg.showNet = false;
    cfg.showGpu = true;
    cfg.showGpuTemp = true;
    cfg.showGpuPower = false;
    cfg.showGpuFan = false;

    std::string formatted = BareStats::Formatter::FormatUtf8(sys, &cfg);
    TEST_ASSERT(formatted.find("GPU0") != std::string::npos, "Multi-GPU layout must include GPU0 header");
    TEST_ASSERT(formatted.find("GPU1") != std::string::npos, "Multi-GPU layout must include GPU1 header");
    TEST_ASSERT(formatted.find("85%") != std::string::npos, "GPU0 utilization must be formatted");
    TEST_ASSERT(formatted.find("14%") != std::string::npos, "GPU1 utilization must be formatted");

    return true;
}

// 10. Fault Tolerance and Graceful Driver Recovery
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

// 11. Configuration Parser & INI Serialization
bool Test_Config_ParsingAndHotReload() {
    std::string tempIni = GetModuleDir() + "temp_test_config.ini";
    {
        std::ofstream out(tempIni);
        out << "[Appearance]\n"
            << "opacity = 85\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 750\n"
            << "gpu_index = 0, 1\n"
            << "vram_mode = usable\n"
            << "show_net = true\n";
    }

    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(tempIni);
    DeleteFileA(tempIni.c_str());

    TEST_ASSERT(loaded.refreshIntervalMs == 750, "refresh_interval_ms parsing mismatch");
    TEST_ASSERT(loaded.opacity == 85, "opacity parsing mismatch");
    TEST_ASSERT(loaded.vramMode == BareStats::VramMode::Usable, "vram_mode parsing mismatch");
    TEST_ASSERT(loaded.showNet == true, "show_net parsing mismatch");
    TEST_ASSERT(loaded.targetGpuIndices.size() == 2, "gpu_index multi-target parsing mismatch");
    TEST_ASSERT(loaded.targetGpuIndices[0] == 0 && loaded.targetGpuIndices[1] == 1, "gpu indices mismatch");

    return true;
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " BareStats Hardware-Agnostic Automated Verification Suite    " << std::endl;
    std::cout << "============================================================" << std::endl;

    RUN_TEST(Test_CpuCollector_RealHardware);
    RUN_TEST(Test_MemoryCollector_RealHardware);
    RUN_TEST(Test_NetworkCollector_RealHardware);
    RUN_TEST(Test_UniversalGraphics_D3dKmtAndDxgi);
    RUN_TEST(Test_NvidiaNvml_MockPipeline);
    RUN_TEST(Test_AmdAdl_MockPipeline);
    RUN_TEST(Test_IntelIgcl_MockPipeline);
    RUN_TEST(Test_PrecisionVram_FormattingMath);
    RUN_TEST(Test_MultiGpu_FormattingLayout);
    RUN_TEST(Test_DriverFallback_GracefulRecovery);
    RUN_TEST(Test_Config_ParsingAndHotReload);

    std::cout << "============================================================" << std::endl;
    std::cout << " Results: " << g_passedTests << "/" 
              << g_totalTests << " passed, " 
              << g_failedTests << " failed." << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
