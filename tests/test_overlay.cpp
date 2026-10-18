#include "test_common.h"
#include "../src/config/config.h"
#include "../src/util/formatter.h"
#include "../src/overlay/linux/linux_overlay.h"

// 1. Grid Layout and Multi-Row Wrapping
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
    TEST_ASSERT(rows.size() == 3, "Grid layout with 4 GPUs and maxGpusPerRow=2 must produce 3 rows");

    // Test Horizontal layout produces exactly 1 row
    cfg.layout = BareStats::OverlayLayout::Horizontal;
    auto horizRows = BareStats::Formatter::FormatRows(sys, &cfg);
    TEST_ASSERT(horizRows.size() == 1, "Horizontal layout must always produce exactly 1 row");

    // Test 3 GPUs with maxGpusPerRow = 2 produces 3 rows
    sys.gpus.pop_back(); // Now 3 GPUs
    cfg.layout = BareStats::OverlayLayout::Grid;
    auto rows3 = BareStats::Formatter::FormatRows(sys, &cfg);
    TEST_ASSERT(rows3.size() == 3, "Grid layout with 3 GPUs and maxGpusPerRow=2 must produce 3 rows");

    return true;
}

// 2. True Matrix Grid Parsing, Colspans & Mode Verification
bool Test_TrueMatrixGrid_ParsingAndColspans() {
    std::string tempIni = GetTestModuleDir() + "test_matrix_grid.ini";
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

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
    std::filesystem::remove(tempIni, ec);

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
        std::string tempSpanIni = GetTestModuleDir() + "test_span_syntax.ini";
        std::ofstream out(tempSpanIni);
        out << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_row_1 = cpu | . | gpu0:span(2)\n";
        out.close();

        BareStats::AppConfig spanCfg = BareStats::ConfigManager::Load(tempSpanIni);
        std::filesystem::remove(tempSpanIni, ec);

        TEST_ASSERT(spanCfg.customGrid.numCols == 4, "Span syntax row must have 4 total columns");
        const auto& row = spanCfg.customGrid.rows[0];
        TEST_ASSERT(row.cells.size() == 3, "Row must have 3 cells");
        TEST_ASSERT(row.cells[2].token == "gpu0" && row.cells[2].colSpan == 2, "Explicit span(2) must yield colSpan 2");
    }

    return true;
}

// 3. Island Mode Auto-Targeting & Layout Grid Seamlessness Verification
bool Test_IslandsGrid_AutoTargetingAndModeSeamlessness() {
    std::error_code ec;

    // Sub-test A: layout = grid with grid_mode = islands automatically promoted to CustomGrid
    {
        std::string tempIni = GetTestModuleDir() + "test_island_grid_seamless.ini";
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = grid\n"
            << "grid_mode = islands\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        std::filesystem::remove(tempIni, ec);

        TEST_ASSERT(cfg.gridMode == BareStats::GridMode::Islands, "grid_mode must be Islands");
        TEST_ASSERT(cfg.layout == BareStats::OverlayLayout::CustomGrid, "layout = grid with grid_mode = islands must seamlessly activate CustomGrid");
        TEST_ASSERT(!cfg.customGrid.IsEmpty(), "Synthesized customGrid must not be empty");
        TEST_ASSERT(cfg.customGrid.numRows >= 2, "Default synthesized grid must have at least 2 rows");
    }

    // Sub-test B: CustomGrid referencing gpu1 auto-targets all GPUs
    {
        std::string tempGpuIni = GetTestModuleDir() + "test_gpu1_autotarget.ini";
        std::ofstream out(tempGpuIni);
        out << "[BareStats]\n"
            << "schema_version = 120\n\n"
            << "[Position]\n"
            << "layout = custom_grid\n"
            << "grid_mode = islands\n"
            << "grid_row_1 = cpu | ram\n"
            << "grid_row_2 = gpu1\n\n"
            << "[Telemetry]\n"
            << "gpu_index = 0\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempGpuIni);
        std::filesystem::remove(tempGpuIni, ec);

        TEST_ASSERT(cfg.targetAllGpus == true, "Referencing gpu1 in CustomGrid must auto-target all GPUs to query GPU1");
    }

    // Sub-test C: CustomGrid referencing generic "gpu" auto-targets all GPUs
    {
        std::string tempGpuIni = GetTestModuleDir() + "test_gpu_autotarget.ini";
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
        std::filesystem::remove(tempGpuIni, ec);

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

        auto segsGpu = BareStats::Formatter::FormatCell("gpu", metrics, &cfg);
        std::wstring textGpu;
        for (const auto& s : segsGpu) textGpu += s.text;
        TEST_ASSERT(textGpu.find(L"GPU0") != std::wstring::npos, "FormatCell('gpu') must contain GPU0");
        TEST_ASSERT(textGpu.find(L"GPU1") != std::wstring::npos, "FormatCell('gpu') must contain GPU1");

        auto segsGpu0 = BareStats::Formatter::FormatCell("gpu0", metrics, &cfg);
        std::wstring textGpu0;
        for (const auto& s : segsGpu0) textGpu0 += s.text;
        TEST_ASSERT(textGpu0.find(L"GPU0") != std::wstring::npos, "FormatCell('gpu0') must contain GPU0");
        TEST_ASSERT(textGpu0.find(L"GPU1") == std::wstring::npos, "FormatCell('gpu0') must NOT contain GPU1");

        auto segsGpu1 = BareStats::Formatter::FormatCell("gpu1", metrics, &cfg);
        std::wstring textGpu1;
        for (const auto& s : segsGpu1) textGpu1 += s.text;
        TEST_ASSERT(textGpu1.find(L"GPU1") != std::wstring::npos, "FormatCell('gpu1') must contain GPU1");
        TEST_ASSERT(textGpu1.find(L"GPU0") == std::wstring::npos, "FormatCell('gpu1') must NOT contain GPU0");
    }

    // Sub-test E: All-spacer grid fallback, text token handling, and interval boundary clamps
    {
        std::string tempIni = GetTestModuleDir() + "test_spacer_fallback.ini";
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
        std::filesystem::remove(tempIni, ec);

        TEST_ASSERT(!cfg.customGrid.IsEmpty(), "All-spacer grid must trigger fallback and not remain empty");
        TEST_ASSERT(cfg.customGrid.rows.size() >= 2, "Fallback synthesized grid must have at least 2 rows");
        TEST_ASSERT(cfg.refreshIntervalMs == 1000, "Interval below 100ms must reset to 1000ms default");

        std::string tempUpperIni = GetTestModuleDir() + "test_interval_upper.ini";
        std::ofstream outUpper(tempUpperIni);
        outUpper << "[BareStats]\n"
                 << "schema_version = 130\n\n"
                 << "[Telemetry]\n"
                 << "refresh_interval_ms = 999999\n";
        outUpper.close();
        BareStats::AppConfig cfgUpper = BareStats::ConfigManager::Load(tempUpperIni);
        std::filesystem::remove(tempUpperIni, ec);
        TEST_ASSERT(cfgUpper.refreshIntervalMs == 60000, "Interval above 60000ms must clamp to 60000ms");

        BareStats::SystemMetrics dummyMetrics{};
        auto segsStatic = BareStats::Formatter::FormatCell("text:Rig", dummyMetrics, nullptr);
        TEST_ASSERT(!segsStatic.empty() && segsStatic[0].text == L"Rig", "FormatCell('text:Rig') must output 'Rig'");

        auto segsFallback = BareStats::Formatter::FormatCell("test", dummyMetrics, nullptr);
        TEST_ASSERT(!segsFallback.empty() && segsFallback[0].text == L"[test]", "Unrecognized token 'test' must output '[test]'");
    }

    return true;
}

// 4. Linux Overlay Sub-Microsecond Dirty-Frame Hash Bypass
bool Test_LinuxOverlay_DirtyHashBypass() {
    std::vector<std::vector<BareStats::FormattedSegment>> frame1 = {
        { { L"CPU", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Label, false, false },
          { L"25", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Number, true, false },
          { L"%", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Unit, false, false } }
    };

    std::vector<std::vector<BareStats::FormattedSegment>> frame2 = frame1;

    std::vector<std::vector<BareStats::FormattedSegment>> frame3 = {
        { { L"CPU", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Label, false, false },
          { L"26", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Number, true, false },
          { L"%", BareStats::MetricSeverity::Normal, BareStats::SegmentType::Unit, false, false } }
    };

    size_t lastHash = 0;
    bool pass1 = BareStats::LinuxOverlay::ComputeDirtyHash(frame1, lastHash);
    TEST_ASSERT(pass1 == true, "First frame pass must compute hash and mark dirty");
    TEST_ASSERT(lastHash != 0, "Last hash must be populated");

    size_t hashAfterPass1 = lastHash;
    bool pass2 = BareStats::LinuxOverlay::ComputeDirtyHash(frame2, lastHash);
    TEST_ASSERT(pass2 == false, "Identical second frame must bypass rendering (clean / not dirty)");
    TEST_ASSERT(lastHash == hashAfterPass1, "Hash must remain identical after clean bypass");

    bool pass3 = BareStats::LinuxOverlay::ComputeDirtyHash(frame3, lastHash);
    TEST_ASSERT(pass3 == true, "Mutated metric value must trigger dirty frame update");
    TEST_ASSERT(lastHash != hashAfterPass1, "New hash must differ from previous hash");

    return true;
}

// 5. Linux Overlay Matrix Grid Islands Layout Math
bool Test_LinuxOverlay_IslandsLayoutMath() {
    BareStats::AppConfig config;
    config.layout = BareStats::OverlayLayout::CustomGrid;
    config.gridMode = BareStats::GridMode::Islands;
    config.cellGap = 12;
    config.rowGap = 8;
    config.preset = BareStats::PositionPreset::TopLeft;
    config.margin = 10;

    BareStats::GridRow row0;
    row0.cells.push_back({ "CPU", 0, 1 });
    row0.cells.push_back({ "RAM", 1, 1 });
    BareStats::GridRow row1;
    row1.cells.push_back({ "GPU", 0, 1 });
    row1.cells.push_back({ "NET", 1, 1 });
    config.customGrid.rows.push_back(row0);
    config.customGrid.rows.push_back(row1);

    BareStats::SystemMetrics metrics;
    metrics.cpu.utilizationPercent = 15.0;
    metrics.memory.usedBytes = 8ULL * 1024 * 1024 * 1024;
    metrics.memory.totalBytes = 16ULL * 1024 * 1024 * 1024;
    metrics.gpu.isAvailable = true;
    metrics.gpu.utilizationPercent = 40.0;
    metrics.network.isAvailable = true;
    metrics.network.rxBytesPerSec = 1024 * 1024;

    BareStats::MonitorInfo monitor;
    monitor.rcWork = { 0, 0, 1920, 1080 };
    monitor.rcMonitor = { 0, 0, 1920, 1080 };

    auto slots = BareStats::LinuxOverlay::LayoutIslands(config, metrics, monitor);
    TEST_ASSERT(slots.size() == 4, "Must generate exactly 4 island slots for 2x2 grid");

    TEST_ASSERT(slots[0].token == "CPU" && slots[0].rowIndex == 0 && slots[0].colIndex == 0, "Slot 0 must be CPU (0,0)");
    TEST_ASSERT(slots[1].token == "RAM" && slots[1].rowIndex == 0 && slots[1].colIndex == 1, "Slot 1 must be RAM (0,1)");
    TEST_ASSERT(slots[2].token == "GPU" && slots[2].rowIndex == 1 && slots[2].colIndex == 0, "Slot 2 must be GPU (1,0)");
    TEST_ASSERT(slots[3].token == "NET" && slots[3].rowIndex == 1 && slots[3].colIndex == 1, "Slot 3 must be NET (1,1)");

    TEST_ASSERT(slots[1].relX >= slots[0].relX + slots[0].width, "RAM must be positioned to the right of CPU");
    TEST_ASSERT(slots[2].relY >= slots[0].relY + slots[0].height, "GPU must be positioned below CPU");

    return true;
}

// 6. Linux Overlay Custom Grid with Empty Cells ('.') and Custom Coordinates
bool Test_LinuxOverlay_CustomGridWithEmptyCells() {
    BareStats::AppConfig config;
    config.layout = BareStats::OverlayLayout::CustomGrid;
    config.gridMode = BareStats::GridMode::Islands;
    config.preset = BareStats::PositionPreset::Custom;
    config.customX = 800;
    config.customY = 600;
    config.cellGap = 160;
    config.rowGap = 200;

    BareStats::GridRow row0;
    row0.cells.push_back({ "cpu", 0, 1 });
    row0.cells.push_back({ ".", 1, 1 });
    row0.cells.push_back({ ".", 2, 1 });
    row0.cells.push_back({ "ram", 3, 1 });
    config.customGrid.rows.push_back(row0);

    BareStats::SystemMetrics metrics;
    metrics.cpu.utilizationPercent = 25.0;
    metrics.memory.usedBytes = 4ULL * 1024 * 1024 * 1024;
    metrics.memory.totalBytes = 16ULL * 1024 * 1024 * 1024;

    BareStats::MonitorInfo monitor;
    monitor.rcWork = { 0, 0, 1920, 1080 };
    monitor.rcMonitor = { 0, 0, 1920, 1080 };

    auto slots = BareStats::LinuxOverlay::LayoutIslands(config, metrics, monitor);
    TEST_ASSERT(slots.size() == 2, "Only non-empty cells ('cpu' and 'ram') should create island slots");
    TEST_ASSERT(slots[0].token == "cpu", "First slot must be cpu");
    TEST_ASSERT(slots[1].token == "ram", "Second slot must be ram");
    TEST_ASSERT(slots[0].relX == 800, "First slot X coordinate must anchor at customX (800)");
    TEST_ASSERT(slots[0].relY == 600, "First slot Y coordinate must anchor at customY (600)");
    TEST_ASSERT(slots[1].relX > 800 + slots[0].width + 2 * 160, "RAM slot must skip empty cells and gaps");
    return true;
}
