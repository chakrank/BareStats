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

    if (m2.isAvailable) {
        TEST_ASSERT(m2.rxBytesPerSec < 1000000000000ULL, "Inbound speed sanity check");
        TEST_ASSERT(m2.txBytesPerSec < 1000000000000ULL, "Outbound speed sanity check");
    }

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

// 12. Per-GPU Asymmetric Formatting & Overrides
bool Test_PerGpu_AsymmetricFormatting() {
    BareStats::SystemMetrics sys{};

    BareStats::GpuMetrics gpu0{};
    gpu0.isAvailable = true;
    gpu0.utilizationPercent = 85.0;
    gpu0.temperatureC = 65;
    gpu0.fanRpm = 1500;
    gpu0.powerWatts = 250;
    gpu0.memoryTotalBytes = 24ULL * 1024 * 1024 * 1024;
    gpu0.memoryUsedBytes = 6ULL * 1024 * 1024 * 1024;

    BareStats::GpuMetrics gpu1{};
    gpu1.isAvailable = true;
    gpu1.utilizationPercent = 20.0;
    gpu1.temperatureC = 40;
    gpu1.fanRpm = 800;
    gpu1.powerWatts = 70;
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
    cfg.showGpuPower = true;
    cfg.showGpuFan = true;

    // Override GPU0: custom label RTX4090, hide power and fan
    BareStats::PerGpuConfig p0{};
    p0.label = "RTX4090";
    p0.showPower = false;
    p0.showFan = false;
    cfg.perGpuConfigs[0] = p0;

    // Override GPU1: custom label A2000, keep power and fan
    BareStats::PerGpuConfig p1{};
    p1.label = "A2000";
    p1.showPower = true;
    p1.showFan = true;
    cfg.perGpuConfigs[1] = p1;

    std::string formatted = BareStats::Formatter::FormatUtf8(sys, &cfg);
    TEST_ASSERT(formatted.find("RTX4090") != std::string::npos, "Custom label RTX4090 must appear");
    TEST_ASSERT(formatted.find("A2000") != std::string::npos, "Custom label A2000 must appear");

    // RTX4090 power (250W) and fan (1500RPM) must NOT appear
    TEST_ASSERT(formatted.find("250W") == std::string::npos, "GPU0 disabled power must not appear");
    TEST_ASSERT(formatted.find("1500RPM") == std::string::npos, "GPU0 disabled fan must not appear");

    // A2000 power (70W) and fan (800RPM) MUST appear
    TEST_ASSERT(formatted.find("70W") != std::string::npos, "GPU1 enabled power must appear");
    TEST_ASSERT(formatted.find("800RPM") != std::string::npos, "GPU1 enabled fan must appear");

    // Test selective exclusion: disable GPU0 completely
    cfg.perGpuConfigs[0].enabled = false;
    std::string formattedExcl = BareStats::Formatter::FormatUtf8(sys, &cfg);
    TEST_ASSERT(formattedExcl.find("RTX4090") == std::string::npos, "Disabled GPU0 must not be rendered");
    TEST_ASSERT(formattedExcl.find("A2000") != std::string::npos, "GPU1 must still be rendered when GPU0 is disabled");

    return true;
}

// 13. Grid Layout and Multi-Row Wrapping
bool Test_GridLayout_MultiRowWrapping() {
    BareStats::SystemMetrics sys{};
    sys.cpu.utilizationPercent = 25.0;
    sys.memory.totalBytes = 32ULL * 1024 * 1024 * 1024;
    sys.memory.usedBytes = 16ULL * 1024 * 1024 * 1024;

    for (int i = 0; i < 4; ++i) {
        BareStats::GpuMetrics g{};
        g.isAvailable = true;
        g.utilizationPercent = 10.0 * (i + 1);
        g.memoryTotalBytes = 8ULL * 1024 * 1024 * 1024;
        g.memoryUsedBytes = 2ULL * 1024 * 1024 * 1024;
        sys.gpus.push_back(g);
    }

    BareStats::AppConfig cfg{};
    cfg.showCpu = true;
    cfg.showRam = true;
    cfg.showNet = false;
    cfg.showGpu = true;
    cfg.layout = BareStats::OverlayLayout::Grid;
    cfg.maxGpusPerRow = 2;

    auto rows = BareStats::Formatter::FormatRows(sys, &cfg);
    // With 4 GPUs and maxGpusPerRow = 2, we expect:
    // Row 0: System (CPU, RAM)
    // Row 1: GPU0, GPU1
    // Row 2: GPU2, GPU3
    TEST_ASSERT(rows.size() == 3, "Grid layout with 4 GPUs and maxGpusPerRow=2 must produce 3 rows");

    // Test Horizontal layout produces exactly 1 row
    cfg.layout = BareStats::OverlayLayout::Horizontal;
    auto horizRows = BareStats::Formatter::FormatRows(sys, &cfg);
    TEST_ASSERT(horizRows.size() == 1, "Horizontal layout must always produce exactly 1 row");

    // Test 3 GPUs with maxGpusPerRow = 2 produces 3 rows (Row 0: sys, Row 1: 2 gpus, Row 2: 1 gpu)
    sys.gpus.pop_back(); // Now 3 GPUs
    cfg.layout = BareStats::OverlayLayout::Grid;
    auto rows3 = BareStats::Formatter::FormatRows(sys, &cfg);
    TEST_ASSERT(rows3.size() == 3, "Grid layout with 3 GPUs and maxGpusPerRow=2 must produce 3 rows");

    return true;
}

// 14. Hardware Introspection & Config Auto-Generation
bool Test_HardwareIntrospection_ConfigAutoGeneration() {
    std::string tempIni = GetModuleDir() + "test_autogen_config.ini";
    DeleteFileA(tempIni.c_str());

    BareStats::ConfigManager::CreateDefaultConfigFile(tempIni);

    std::ifstream in(tempIni);
    TEST_ASSERT(in.is_open(), "Generated config file must be readable");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    TEST_ASSERT(content.find("# Hardware Detected:") != std::string::npos,
                "Auto-generated config must contain hardware introspection header");
    TEST_ASSERT(content.find("layout = horizontal") != std::string::npos,
                "Auto-generated config must include layout setting");
    TEST_ASSERT(content.find("max_gpus_per_row = 2") != std::string::npos,
                "Auto-generated config must include max_gpus_per_row setting");
    TEST_ASSERT(content.find("# [GPU0]") != std::string::npos,
                "Auto-generated config must include per-GPU template comments");

    // Load auto-generated config
    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(loaded.layout == BareStats::OverlayLayout::Horizontal, "Loaded layout must be Horizontal");
    TEST_ASSERT(loaded.maxGpusPerRow == 2, "Loaded maxGpusPerRow must be 2");

    // Append an active [GPU0] override block
    {
        std::ofstream out(tempIni, std::ios::app);
        out << "\n[GPU0]\n"
            << "label = CustomWorkstation\n"
            << "show_fan = false\n"
            << "show_power = false\n";
    }

    BareStats::AppConfig reloaded = BareStats::ConfigManager::Load(tempIni);
    DeleteFileA(tempIni.c_str());

    TEST_ASSERT(reloaded.perGpuConfigs.count(0) == 1, "Reloaded config must contain GPU0 override");
    TEST_ASSERT(reloaded.perGpuConfigs[0].label == "CustomWorkstation", "GPU0 custom label mismatch");
    TEST_ASSERT(reloaded.perGpuConfigs[0].showFan == false, "GPU0 show_fan mismatch");
    TEST_ASSERT(reloaded.perGpuConfigs[0].showPower == false, "GPU0 show_power mismatch");

    return true;
}

// 15. Non-Destructive In-Place Schema Migration & Backup Verification
bool Test_Config_SchemaMigration() {
    std::string tempIni = GetModuleDir() + "test_legacy_config.ini";
    std::string bakIni = tempIni + ".bak";
    DeleteFileA(tempIni.c_str());
    DeleteFileA(bakIni.c_str());

    // Write a simulated legacy v1.0 configuration (no [BareStats], no schema_version, no layout, no vram_mode)
    {
        std::ofstream out(tempIni);
        out << "# User Custom Legacy Config\n"
            << "[Position]\n"
            << "preset = top-center\n"
            << "margin = 18\n"
            << "[Appearance]\n"
            << "opacity = 82\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 450\n"
            << "show_cpu = 1\n"
            << "show_gpu = 1\n";
    }

    // Load via ConfigManager which should detect schema_version missing (< 120) and trigger MigrateSchema
    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(tempIni);

    // 1. Verify backup file was created
    DWORD bakAttr = GetFileAttributesA(bakIni.c_str());
    TEST_ASSERT(bakAttr != INVALID_FILE_ATTRIBUTES, "Safety backup file (.bak) must be created upon migration");

    // 2. Verify user customizations were preserved in memory
    TEST_ASSERT(loaded.preset == BareStats::PositionPreset::TopCenter, "User preset must be preserved");
    TEST_ASSERT(loaded.margin == 18, "User margin must be preserved");
    TEST_ASSERT(loaded.opacity == 82, "User opacity must be preserved");
    TEST_ASSERT(loaded.refreshIntervalMs == 450, "User refresh_interval_ms must be preserved");

    // 3. Verify newly migrated keys received fallback defaults
    TEST_ASSERT(loaded.layout == BareStats::OverlayLayout::Horizontal, "Migrated layout must default to Horizontal");
    TEST_ASSERT(loaded.maxGpusPerRow == 2, "Migrated maxGpusPerRow must default to 2");
    TEST_ASSERT(loaded.vramMode == BareStats::VramMode::Physical, "Migrated vramMode must default to Physical");

    // 4. Inspect file on disk: verify schema_version = 120 and newly injected keys were added
    std::ifstream in(tempIni);
    TEST_ASSERT(in.is_open(), "Migrated file must be readable");
    std::string diskContent((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    TEST_ASSERT(diskContent.find("schema_version = 120") != std::string::npos,
                "Migrated file must contain schema_version = 120");
    TEST_ASSERT(diskContent.find("layout = horizontal") != std::string::npos,
                "Migrated file must contain injected layout key");
    TEST_ASSERT(diskContent.find("max_gpus_per_row = 2") != std::string::npos,
                "Migrated file must contain injected max_gpus_per_row key");
    TEST_ASSERT(diskContent.find("vram_mode = physical") != std::string::npos,
                "Migrated file must contain injected vram_mode key");
    TEST_ASSERT(diskContent.find("order = CPU, GPU, RAM, NET") != std::string::npos,
                "Migrated file must contain injected order key");

    // 5. Verify user's original custom values remain in disk content
    TEST_ASSERT(diskContent.find("preset = top-center") != std::string::npos,
                "User's custom preset must remain on disk");
    TEST_ASSERT(diskContent.find("margin = 18") != std::string::npos,
                "User's custom margin must remain on disk");
    TEST_ASSERT(diskContent.find("opacity = 82") != std::string::npos,
                "User's custom opacity must remain on disk");
    TEST_ASSERT(diskContent.find("refresh_interval_ms = 450") != std::string::npos,
                "User's custom refresh_interval_ms must remain on disk");

    // 6. Fast Path Verification: loading an already migrated file should not alter the backup time
    WIN32_FILE_ATTRIBUTE_DATA bakDataBefore{};
    GetFileAttributesExA(bakIni.c_str(), GetFileExInfoStandard, &bakDataBefore);

    BareStats::AppConfig reloaded = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(reloaded.opacity == 82, "Fast-path reloaded opacity must match");

    WIN32_FILE_ATTRIBUTE_DATA bakDataAfter{};
    GetFileAttributesExA(bakIni.c_str(), GetFileExInfoStandard, &bakDataAfter);

    TEST_ASSERT(bakDataBefore.ftLastWriteTime.dwLowDateTime == bakDataAfter.ftLastWriteTime.dwLowDateTime &&
                bakDataBefore.ftLastWriteTime.dwHighDateTime == bakDataAfter.ftLastWriteTime.dwHighDateTime,
                "Fast-path load must bypass migration and not touch backup file");

    DeleteFileA(tempIni.c_str());
    DeleteFileA(bakIni.c_str());
    return true;
}

// 16. True Matrix Grid Parsing, Colspans & Mode Verification
bool Test_TrueMatrixGrid_ParsingAndColspans() {
    std::string tempIni = GetModuleDir() + "test_matrix_grid.ini";
    DeleteFileA(tempIni.c_str());

    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_mode = islands\n"
            << "cell_gap = 20\n"
            << "row_gap = 6\n"
            << "grid_row_1 = cpu | null | null | ram\n"
            << "grid_row_2 = null | null | gpu0 | gpu0\n"
            << "grid_row_3 = gpu1 | gpu1 | gpu1 | gpu1\n"
            << "grid_row_4 = null | net | net | net\n";
    }

    BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
    DeleteFileA(tempIni.c_str());

    TEST_ASSERT(cfg.layout == BareStats::OverlayLayout::CustomGrid, "Layout must be parsed as CustomGrid");
    TEST_ASSERT(cfg.gridMode == BareStats::GridMode::Islands, "GridMode must be parsed as Islands");
    TEST_ASSERT(cfg.cellGap == 20, "cell_gap must be 20");
    TEST_ASSERT(cfg.rowGap == 6, "row_gap must be 6");
    TEST_ASSERT(cfg.customGrid.numCols == 4, "Matrix must have 4 columns");
    TEST_ASSERT(cfg.customGrid.numRows == 4, "Matrix must have 4 rows");

    // Row 0: cpu | null | null | ram
    const auto& r0 = cfg.customGrid.rows[0];
    TEST_ASSERT(r0.cells.size() == 4, "Row 0 must contain 4 cells");
    TEST_ASSERT(r0.cells[0].token == "cpu" && r0.cells[0].colSpan == 1, "Row 0 Cell 0 must be cpu span 1");
    TEST_ASSERT(r0.cells[1].token == "" && r0.cells[1].colSpan == 1, "Row 0 Cell 1 must be null span 1");
    TEST_ASSERT(r0.cells[2].token == "" && r0.cells[2].colSpan == 1, "Row 0 Cell 2 must be null span 1");
    TEST_ASSERT(r0.cells[3].token == "ram" && r0.cells[3].colSpan == 1, "Row 0 Cell 3 must be ram span 1");

    // Row 1: null | null | gpu0 | gpu0 (gpu0 merged to colSpan 2)
    const auto& r1 = cfg.customGrid.rows[1];
    TEST_ASSERT(r1.cells.size() == 3, "Row 1 must contain 3 cell items (null, null, merged gpu0)");
    TEST_ASSERT(r1.cells[0].token == "" && r1.cells[0].colSpan == 1, "Row 1 Cell 0 must be null span 1");
    TEST_ASSERT(r1.cells[1].token == "" && r1.cells[1].colSpan == 1, "Row 1 Cell 1 must be null span 1");
    TEST_ASSERT(r1.cells[2].token == "gpu0" && r1.cells[2].colSpan == 2, "Row 1 Cell 2 must be gpu0 merged span 2");

    // Row 2: gpu1 | gpu1 | gpu1 | gpu1 (gpu1 merged across all 4 cols)
    const auto& r2 = cfg.customGrid.rows[2];
    TEST_ASSERT(r2.cells.size() == 1, "Row 2 must contain 1 merged cell item");
    TEST_ASSERT(r2.cells[0].token == "gpu1" && r2.cells[0].colSpan == 4, "Row 2 Cell 0 must be gpu1 merged span 4");

    // Row 3: null | net | net | net (net merged across 3 cols)
    const auto& r3 = cfg.customGrid.rows[3];
    TEST_ASSERT(r3.cells.size() == 2, "Row 3 must contain 2 cell items (null, merged net)");
    TEST_ASSERT(r3.cells[0].token == "" && r3.cells[0].colSpan == 1, "Row 3 Cell 0 must be null span 1");
    TEST_ASSERT(r3.cells[1].token == "net" && r3.cells[1].colSpan == 3, "Row 3 Cell 1 must be net merged span 3");

    // Explicit span syntax verification: gpu0:span(2)
    {
        std::string tempSpanIni = GetModuleDir() + "test_span_syntax.ini";
        std::ofstream out(tempSpanIni);
        out << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_row_1 = cpu | . | gpu0:span(2)\n";
        out.close();

        BareStats::AppConfig spanCfg = BareStats::ConfigManager::Load(tempSpanIni);
        DeleteFileA(tempSpanIni.c_str());

        TEST_ASSERT(spanCfg.customGrid.numCols == 4, "Span syntax row must have 4 total columns");
        const auto& row = spanCfg.customGrid.rows[0];
        TEST_ASSERT(row.cells.size() == 3, "Row must have 3 cells");
        TEST_ASSERT(row.cells[2].token == "gpu0" && row.cells[2].colSpan == 2, "Explicit span(2) must yield colSpan 2");
    }

    return true;
}

// 17. Modular FormatCell and Sub-Stats Verification
bool Test_FormatCell_ModularWidgetsAndSubStats() {
    BareStats::SystemMetrics sys{};
    sys.cpu.utilizationPercent = 25.0;
    sys.cpu.temperatureC = 55.0;
    sys.cpu.powerWatts = 65.0;
    sys.cpu.fanRpm = 1200;

    sys.memory.totalBytes = 32ULL * 1024 * 1024 * 1024;
    sys.memory.usedBytes = 16ULL * 1024 * 1024 * 1024;

    BareStats::GpuMetrics g0{};
    g0.isAvailable = true;
    g0.utilizationPercent = 75.0;
    g0.temperatureC = 62;
    g0.powerWatts = 180;
    g0.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;
    g0.memoryUsedBytes = 6ULL * 1024 * 1024 * 1024;
    sys.gpus.push_back(g0);
    sys.gpu = g0;

    sys.network.isAvailable = true;
    sys.network.rxBytesPerSec = 2ULL * 1024 * 1024;
    sys.network.txBytesPerSec = 512ULL * 1024;

    BareStats::AppConfig cfg{};

    // Null tokens must yield empty segment vectors
    auto nullSegs = BareStats::Formatter::FormatCell("null", sys, &cfg);
    TEST_ASSERT(nullSegs.empty(), "FormatCell(null) must return empty segments");
    auto dotSegs = BareStats::Formatter::FormatCell(".", sys, &cfg);
    TEST_ASSERT(dotSegs.empty(), "FormatCell(.) must return empty segments");

    // Full CPU
    auto cpuSegs = BareStats::Formatter::FormatCell("cpu", sys, &cfg);
    TEST_ASSERT(!cpuSegs.empty(), "FormatCell(cpu) must return non-empty segments");
    bool foundCpuLabel = false;
    for (const auto& s : cpuSegs) {
        if (s.text.find(L"CPU") != std::wstring::npos) foundCpuLabel = true;
    }
    TEST_ASSERT(foundCpuLabel, "FormatCell(cpu) must contain CPU label");

    // CPU Util sub-stat
    auto cpuUtilSegs = BareStats::Formatter::FormatCell("cpu_util", sys, &cfg);
    TEST_ASSERT(!cpuUtilSegs.empty(), "FormatCell(cpu_util) must return segments");

    // RAM
    auto ramSegs = BareStats::Formatter::FormatCell("ram", sys, &cfg);
    TEST_ASSERT(!ramSegs.empty(), "FormatCell(ram) must return segments");

    // GPU0 & GPU0 VRAM
    auto gpuSegs = BareStats::Formatter::FormatCell("gpu0", sys, &cfg);
    TEST_ASSERT(!gpuSegs.empty(), "FormatCell(gpu0) must return segments");
    auto vramSegs = BareStats::Formatter::FormatCell("gpu0_vram", sys, &cfg);
    TEST_ASSERT(!vramSegs.empty(), "FormatCell(gpu0_vram) must return segments");

    // NET
    auto netSegs = BareStats::Formatter::FormatCell("net", sys, &cfg);
    TEST_ASSERT(!netSegs.empty(), "FormatCell(net) must return segments");

    // Test FormatRows with CustomGrid layout produces rows matching grid height
    cfg.layout = BareStats::OverlayLayout::CustomGrid;
    BareStats::GridRow r0;
    r0.cells.push_back({ "cpu", 0, 1 });
    r0.cells.push_back({ "ram", 1, 1 });
    BareStats::GridRow r1;
    r1.cells.push_back({ "gpu0", 0, 2 });
    cfg.customGrid.rows.push_back(r0);
    cfg.customGrid.rows.push_back(r1);
    cfg.customGrid.numCols = 2;
    cfg.customGrid.numRows = 2;

    auto gridRows = BareStats::Formatter::FormatRows(sys, &cfg);
    TEST_ASSERT(gridRows.size() == 2, "FormatRows for 2-row CustomGrid must return exactly 2 rows");

    return true;
}

// 18. Island Mode Auto-Targeting & Layout Grid Seamlessness Verification
bool Test_IslandsGrid_AutoTargetingAndModeSeamlessness() {
    // Sub-test A: layout = grid with grid_mode = islands automatically promoted to CustomGrid
    {
        std::string tempIni = GetModuleDir() + "test_island_grid_seamless.ini";
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = grid\n"
            << "grid_mode = islands\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        DeleteFileA(tempIni.c_str());

        TEST_ASSERT(cfg.gridMode == BareStats::GridMode::Islands, "grid_mode must be Islands");
        TEST_ASSERT(cfg.layout == BareStats::OverlayLayout::CustomGrid, "layout = grid with grid_mode = islands must seamlessly activate CustomGrid");
        TEST_ASSERT(!cfg.customGrid.IsEmpty(), "Synthesized customGrid must not be empty");
        TEST_ASSERT(cfg.customGrid.numRows >= 2, "Default synthesized grid must have at least 2 rows");
    }

    // Sub-test B: CustomGrid referencing gpu1 auto-targets all GPUs
    {
        std::string tempGpuIni = GetModuleDir() + "test_gpu1_autotarget.ini";
        std::ofstream out(tempGpuIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_mode = islands\n"
            << "grid_row_1 = cpu | ram\n"
            << "grid_row_2 = gpu1\n\n"
            << "[Telemetry]\n"
            << "gpu_index = 0\n"; // user only configured GPU0 explicitly
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempGpuIni);
        DeleteFileA(tempGpuIni.c_str());

        TEST_ASSERT(cfg.targetAllGpus == true, "Referencing gpu1 in CustomGrid must auto-target all GPUs to query GPU1");
    }

    // Sub-test C: CustomGrid referencing generic "gpu" auto-targets all GPUs
    {
        std::string tempGpuIni = GetModuleDir() + "test_gpu_autotarget.ini";
        std::ofstream out(tempGpuIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_mode = islands\n"
            << "grid_row_1 = cpu | ram\n"
            << "grid_row_2 = gpu\n\n"
            << "[Telemetry]\n"
            << "gpu_index = 0\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempGpuIni);
        DeleteFileA(tempGpuIni.c_str());

        TEST_ASSERT(cfg.targetAllGpus == true, "Referencing generic 'gpu' in CustomGrid must auto-target all GPUs to query multi-GPU setups");
    }

    // Sub-test D: Multi-GPU FormatCell formatting
    {
        BareStats::SystemMetrics metrics{};
        BareStats::GpuMetrics g0{};
        g0.isAvailable = true;
        g0.utilizationPercent = 25.0;
        g0.memoryUsedBytes = 2ULL * 1024 * 1024 * 1024;
        g0.memoryTotalBytes = 8ULL * 1024 * 1024 * 1024;
        g0.temperatureC = 55;

        BareStats::GpuMetrics g1{};
        g1.isAvailable = true;
        g1.utilizationPercent = 65.0;
        g1.memoryUsedBytes = 6ULL * 1024 * 1024 * 1024;
        g1.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;
        g1.temperatureC = 68;

        metrics.gpus = { g0, g1 };
        metrics.gpu = g0;

        BareStats::AppConfig cfg{};
        cfg.separator = L"\x2502";

        // 1. Generic "gpu" token must output both GPUs separated by divider
        auto segsGpu = BareStats::Formatter::FormatCell("gpu", metrics, &cfg);
        std::wstring textGpu;
        for (const auto& s : segsGpu) textGpu += s.text;
        TEST_ASSERT(textGpu.find(L"GPU0") != std::wstring::npos, "FormatCell('gpu') must contain GPU0");
        TEST_ASSERT(textGpu.find(L"GPU1") != std::wstring::npos, "FormatCell('gpu') must contain GPU1");
        TEST_ASSERT(textGpu.find(L"25%") != std::wstring::npos, "FormatCell('gpu') must contain GPU0 util 25%");
        TEST_ASSERT(textGpu.find(L"65%") != std::wstring::npos, "FormatCell('gpu') must contain GPU1 util 65%");

        // 2. Specific "gpu0" token must output GPU0 only
        auto segsGpu0 = BareStats::Formatter::FormatCell("gpu0", metrics, &cfg);
        std::wstring textGpu0;
        for (const auto& s : segsGpu0) textGpu0 += s.text;
        TEST_ASSERT(textGpu0.find(L"GPU0") != std::wstring::npos, "FormatCell('gpu0') must contain GPU0");
        TEST_ASSERT(textGpu0.find(L"GPU1") == std::wstring::npos, "FormatCell('gpu0') must NOT contain GPU1");

        // 3. Specific "gpu1" token must output GPU1 only
        auto segsGpu1 = BareStats::Formatter::FormatCell("gpu1", metrics, &cfg);
        std::wstring textGpu1;
        for (const auto& s : segsGpu1) textGpu1 += s.text;
        TEST_ASSERT(textGpu1.find(L"GPU1") != std::wstring::npos, "FormatCell('gpu1') must contain GPU1");
        TEST_ASSERT(textGpu1.find(L"GPU0") == std::wstring::npos, "FormatCell('gpu1') must NOT contain GPU0");

        // 4. Specific "gpu1_vram" token
        auto segsGpu1Vram = BareStats::Formatter::FormatCell("gpu1_vram", metrics, &cfg);
        std::wstring textGpu1Vram;
        for (const auto& s : segsGpu1Vram) textGpu1Vram += s.text;
        TEST_ASSERT(textGpu1Vram.find(L"6.0/16GB") != std::wstring::npos || textGpu1Vram.find(L"6.0/16.0GB") != std::wstring::npos, "FormatCell('gpu1_vram') must format GPU1 VRAM");
    }

    // Sub-test E: All-spacer grid fallback, text token handling, and interval boundary clamps
    {
        std::string tempIni = GetModuleDir() + "test_spacer_fallback.ini";
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_mode = islands\n"
            << "grid_row_1 = . | . | null\n"
            << "grid_row_2 = empty | - | .\n\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 50\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        DeleteFileA(tempIni.c_str());

        // Must synthesize default template instead of rendering empty window
        TEST_ASSERT(!cfg.customGrid.IsEmpty(), "All-spacer grid must trigger fallback and not remain empty");
        TEST_ASSERT(cfg.customGrid.rows.size() >= 2, "Fallback synthesized grid must have at least 2 rows");
        TEST_ASSERT(cfg.refreshIntervalMs == 1000, "Interval below 100ms must reset to 1000ms default");

        // Upper limit clamp test
        std::string tempUpperIni = GetModuleDir() + "test_interval_upper.ini";
        std::ofstream outUpper(tempUpperIni);
        outUpper << "[BareStats]\n"
                 << "schema_version = 120\n\n"
                 << "[Telemetry]\n"
                 << "refresh_interval_ms = 999999\n";
        outUpper.close();
        BareStats::AppConfig cfgUpper = BareStats::ConfigManager::Load(tempUpperIni);
        DeleteFileA(tempUpperIni.c_str());
        TEST_ASSERT(cfgUpper.refreshIntervalMs == 60000, "Interval above 60000ms must clamp to 60000ms");

        // Static text tokens vs fallback debug tokens
        BareStats::SystemMetrics dummyMetrics{};
        auto segsStatic = BareStats::Formatter::FormatCell("text:Rig", dummyMetrics, nullptr);
        TEST_ASSERT(!segsStatic.empty() && segsStatic[0].text == L"Rig", "FormatCell('text:Rig') must output 'Rig'");

        auto segsFallback = BareStats::Formatter::FormatCell("test", dummyMetrics, nullptr);
        TEST_ASSERT(!segsFallback.empty() && segsFallback[0].text == L"[test]", "Unrecognized token 'test' must output '[test]'");
    }

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
    RUN_TEST(Test_PerGpu_AsymmetricFormatting);
    RUN_TEST(Test_GridLayout_MultiRowWrapping);
    RUN_TEST(Test_HardwareIntrospection_ConfigAutoGeneration);
    RUN_TEST(Test_Config_SchemaMigration);
    RUN_TEST(Test_TrueMatrixGrid_ParsingAndColspans);
    RUN_TEST(Test_FormatCell_ModularWidgetsAndSubStats);
    RUN_TEST(Test_IslandsGrid_AutoTargetingAndModeSeamlessness);

    std::cout << "============================================================" << std::endl;
    std::cout << " Results: " << g_passedTests << "/" 
              << g_totalTests << " passed, " 
              << g_failedTests << " failed." << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
