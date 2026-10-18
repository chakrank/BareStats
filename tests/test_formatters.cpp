#include "test_common.h"
#include "../src/util/formatter.h"
#include "../src/config/config.h"

// 1. Precision VRAM Formatting Math
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

// 2. Side-by-Side Multi-GPU Layout Formatting
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

// 3. Per-GPU Asymmetric Formatting & Overrides
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

// 4. FormatCell Modular Widgets and SubStats
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
