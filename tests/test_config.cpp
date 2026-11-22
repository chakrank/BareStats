#include "test_common.h"
#include "../src/config/config.h"
#include "../src/config/ini_parser.h"
#include "../src/util/formatter.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/stat.h>
#endif

// 1. Config Parsing & Hot Reload
bool Test_Config_ParsingAndHotReload() {
    std::string tempIni = GetTestModuleDir() + "temp_test_config.ini";
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
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

    TEST_ASSERT(loaded.refreshIntervalMs == 750, "refresh_interval_ms parsing mismatch");
    TEST_ASSERT(loaded.opacity == 85, "opacity parsing mismatch");
    TEST_ASSERT(loaded.vramMode == BareStats::VramMode::Usable, "vram_mode parsing mismatch");
    TEST_ASSERT(loaded.showNet == true, "show_net parsing mismatch");
    TEST_ASSERT(loaded.targetGpuIndices.size() == 2, "gpu_index multi-target parsing mismatch");
    TEST_ASSERT(loaded.targetGpuIndices[0] == 0 && loaded.targetGpuIndices[1] == 1, "gpu indices mismatch");

    return true;
}

// 2. Hardware Introspection & Config Auto-Generation
bool Test_HardwareIntrospection_ConfigAutoGeneration() {
    std::string tempIni = GetTestModuleDir() + "test_autogen_config.ini";
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

    BareStats::ConfigManager::CreateDefaultConfigFile(tempIni);

    std::ifstream in(tempIni);
    TEST_ASSERT(in.is_open(), "Generated config file must be readable");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    TEST_ASSERT(content.find("# Hardware Detected:") != std::string::npos,
                "Auto-generated config must contain hardware introspection header");
    TEST_ASSERT(content.find("mode = horizontal") != std::string::npos || content.find("layout = horizontal") != std::string::npos,
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
    std::filesystem::remove(tempIni, ec);

    TEST_ASSERT(reloaded.perGpuConfigs.count(0) == 1, "Reloaded config must contain GPU0 override");
    TEST_ASSERT(reloaded.perGpuConfigs[0].label == "CustomWorkstation", "GPU0 custom label mismatch");
    TEST_ASSERT(reloaded.perGpuConfigs[0].showFan == false, "GPU0 show_fan mismatch");
    TEST_ASSERT(reloaded.perGpuConfigs[0].showPower == false, "GPU0 show_power mismatch");

    return true;
}

// 3. Non-Destructive In-Place Schema Migration & Backup Verification
bool Test_Config_SchemaMigration() {
    std::string tempIni = GetTestModuleDir() + "test_legacy_config.ini";
    std::string bakIni = tempIni + ".bak";
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);
    std::filesystem::remove(bakIni, ec);

    // Write a simulated legacy configuration
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

    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(tempIni);

    // 1. Verify backup file was created
    TEST_ASSERT(std::filesystem::exists(bakIni), "Safety backup file (.bak) must be created upon migration");

    // 2. Verify user customizations were preserved in memory
    TEST_ASSERT(loaded.preset == BareStats::PositionPreset::TopCenter, "User preset must be preserved");
    TEST_ASSERT(loaded.margin == 18, "User margin must be preserved");
    TEST_ASSERT(loaded.opacity == 82, "User opacity must be preserved");
    TEST_ASSERT(loaded.refreshIntervalMs == 450, "User refresh_interval_ms must be preserved");

    // 3. Verify newly migrated keys received fallback defaults
    TEST_ASSERT(loaded.layout == BareStats::OverlayLayout::Horizontal, "Migrated layout must default to Horizontal");
    TEST_ASSERT(loaded.maxGpusPerRow == 2, "Migrated maxGpusPerRow must default to 2");
    TEST_ASSERT(loaded.vramMode == BareStats::VramMode::Physical, "Migrated vramMode must default to Physical");

    // 4. Inspect file on disk
    std::ifstream in(tempIni);
    TEST_ASSERT(in.is_open(), "Migrated file must be readable");
    std::string diskContent((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    TEST_ASSERT(diskContent.find("schema_version = 140") != std::string::npos,
                "Migrated file must contain schema_version = 140");
    TEST_ASSERT(diskContent.find("[Cluster]") != std::string::npos,
                "Migrated file must contain injected [Cluster] section");
    TEST_ASSERT(diskContent.find("[LLM]") != std::string::npos,
                "Migrated file must contain injected [LLM] section");
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
    auto bakTimeBefore = std::filesystem::last_write_time(bakIni, ec);

    BareStats::AppConfig reloaded = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(reloaded.opacity == 82, "Fast-path reloaded opacity must match");

    auto bakTimeAfter = std::filesystem::last_write_time(bakIni, ec);
    TEST_ASSERT(bakTimeBefore == bakTimeAfter,
                "Fast-path load must bypass migration and not touch backup file");

    std::filesystem::remove(tempIni, ec);
    std::filesystem::remove(bakIni, ec);
    return true;
}

// 4. Cross-Platform INI Parser & Comment Preservation
bool Test_IniParser_RoundtripAndComments() {
    std::string testIni = GetTestModuleDir() + "test_iniparser.ini";
    {
        std::ofstream out(testIni);
        out << "# Top level comment\n"
            << "; Semicolon comment\n"
            << "[Appearance]\n"
            << "# Appearance notes\n"
            << "text_color = #FF8800\n"
            << "font_size = 14\n"
            << "drop_shadow = 1\n"
            << "opacity = 85%\n\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 500\n"
            << "order = CPU, RAM\n";
    }

    BareStats::IniDocument doc;
    TEST_ASSERT(doc.Load(testIni), "IniDocument must load successfully");
    TEST_ASSERT(doc.HasSection("appearance"), "HasSection case-insensitive must succeed");
    TEST_ASSERT(doc.HasSection("Appearance"), "HasSection must succeed");
    TEST_ASSERT(doc.HasKey("appearance", "text_color"), "HasKey must succeed");
    TEST_ASSERT(doc.GetString("Appearance", "text_color") == "#FF8800", "Hex color text_color must match #FF8800");
    TEST_ASSERT(doc.GetInt("Appearance", "font_size") == 14, "font_size must equal 14");
    TEST_ASSERT(doc.GetBool("Appearance", "drop_shadow") == true, "drop_shadow must be true");
    TEST_ASSERT(doc.GetInt("Telemetry", "refresh_interval_ms") == 500, "refresh_interval_ms must be 500");

    doc.SetInt("Appearance", "font_size", 16);
    doc.SetString("Appearance", "font_weight", "bold");
    doc.SetBool("Telemetry", "show_net", true);
    doc.SetString("NewSection", "custom_key", "value123");
    TEST_ASSERT(doc.Save(testIni), "IniDocument::Save must succeed");

    BareStats::IniDocument reloaded;
    TEST_ASSERT(reloaded.Load(testIni), "Reloading saved INI must succeed");
    TEST_ASSERT(reloaded.GetInt("Appearance", "font_size") == 16, "Updated font_size must be 16");
    TEST_ASSERT(reloaded.GetString("Appearance", "font_weight") == "bold", "New key font_weight must be bold");
    TEST_ASSERT(reloaded.GetBool("Telemetry", "show_net") == true, "New key show_net must be true");
    TEST_ASSERT(reloaded.GetString("NewSection", "custom_key") == "value123", "NewSection key must match");

    std::ifstream in(testIni);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    std::filesystem::remove(testIni, ec);

    TEST_ASSERT(content.find("# Top level comment") != std::string::npos, "Top comment must be preserved");
    TEST_ASSERT(content.find("; Semicolon comment") != std::string::npos, "Semicolon comment must be preserved");
    TEST_ASSERT(content.find("# Appearance notes") != std::string::npos, "Section comment must be preserved");

    return true;
}

// 5. Corrupt / Malformed INI and Boundary Clamping Resilience
bool Test_Config_MalformedAndBoundaryClamping() {
    std::string malformedIni = GetTestModuleDir() + "test_malformed_config.ini";
    std::error_code ec;
    std::filesystem::remove(malformedIni, ec);

    // Write malformed and extreme boundary values
    {
        std::ofstream out(malformedIni);
        out << "orphaned_key_without_section = 12345\n"
            << "= value_without_key\n"
            << "empty_key_with_equals = \n"
            << "[CorruptedSectionWithoutClosingBracket\n"
            << "random_binary_garbage \x01\x02\x03\x04\n"
            << "[Position]\n"
            << "margin = -50\n"
            << "max_gpus_per_row = -10\n"
            << "cell_gap = -99\n"
            << "row_gap = -25\n"
            << "preset = non_existent_preset\n"
            << "layout = imaginary_layout\n"
            << "grid_mode = invalid_mode\n"
            << "[Appearance]\n"
            << "opacity = -50\n"
            << "font_size = -15\n"
            << "text_color = invalid_hex_code\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 10\n"
            << "gpu_index = -5, -1, not_a_number, 3\n"
            << "vram_mode = imaginary_vram_mode\n"
            << "order = INVALID, CPU, NONEXISTENT, RAM\n";
    }

    BareStats::AppConfig config = BareStats::ConfigManager::Load(malformedIni);

    // Validate resilient fallback and boundary clamping
    TEST_ASSERT(config.opacity == 10, "Negative opacity (-50) must be clamped to minimum 10");
    TEST_ASSERT(config.maxGpusPerRow == 1, "Negative max_gpus_per_row (-10) must be clamped to minimum 1");
    TEST_ASSERT(config.cellGap == 0, "Negative cell_gap (-99) must be clamped to 0");
    TEST_ASSERT(config.rowGap == 0, "Negative row_gap (-25) must be clamped to 0");
    TEST_ASSERT(config.refreshIntervalMs == 1000, "Sub-100ms refresh interval (10ms) must fall back to default 1000ms");
    TEST_ASSERT(config.vramMode == BareStats::VramMode::Physical, "Invalid vram_mode must fall back to Physical");
    TEST_ASSERT(config.layout == BareStats::OverlayLayout::Horizontal, "Invalid layout must fall back to Horizontal");
    TEST_ASSERT(config.gridMode == BareStats::GridMode::Unified, "Invalid grid_mode must fall back to Unified");
    TEST_ASSERT(config.targetGpuIndices.size() == 1 && config.targetGpuIndices[0] == 3,
                "GPU index parsing must filter out negatives and non-numbers, keeping valid index 3");

    // Test upper boundaries
    {
        std::ofstream out(malformedIni);
        out << "[Appearance]\n"
            << "opacity = 500\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 9999999\n";
    }

    BareStats::AppConfig upperConfig = BareStats::ConfigManager::Load(malformedIni);
    TEST_ASSERT(upperConfig.opacity == 100, "Excessive opacity (500) must be clamped to maximum 100");
    TEST_ASSERT(upperConfig.refreshIntervalMs == 60000, "Excessive refresh interval must be clamped to 60000ms");

    // Test decimal opacity percentage (e.g. 0.75 -> 75%)
    {
        std::ofstream out(malformedIni);
        out << "[Appearance]\n"
            << "opacity = 0.75\n";
    }
    BareStats::AppConfig decimalConfig = BareStats::ConfigManager::Load(malformedIni);
    TEST_ASSERT(decimalConfig.opacity == 75, "Decimal opacity 0.75 must be parsed as 75%");

    std::filesystem::remove(malformedIni, ec);
    std::string bakFile = malformedIni + ".bak";
    std::filesystem::remove(bakFile, ec);
    return true;
}

// 6. Config Priority and Custom Path Support
bool Test_Config_PriorityAndCustomPath() {
    std::string nestedDir = GetTestModuleDir() + "test_nested_dir/";
    std::string customIni = nestedDir + "custom.ini";
    std::error_code ec;
    std::filesystem::remove_all(nestedDir, ec);

    BareStats::AppConfig cfg;
    cfg.opacity = 92;
    cfg.customX = 120;
    cfg.customY = 340;
    bool saved = BareStats::ConfigManager::Save(customIni, cfg);
    TEST_ASSERT(saved, "Saving to nested non-existent directory must create directory and succeed");
    TEST_ASSERT(std::filesystem::exists(customIni), "Custom ini file must exist on disk");

    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(customIni);
    TEST_ASSERT(loaded.opacity == 92, "Custom config opacity must match saved");
    TEST_ASSERT(loaded.customX == 120 && loaded.customY == 340, "Custom coordinates must match saved");

    std::filesystem::remove_all(nestedDir, ec);
    return true;
}

// VULN-06: Private cluster key restricted file permissions regression test
bool Test_Config_PrivateKeyRestrictedFileAcl() {
    std::string tempIni = GetTestModuleDir() + "temp_test_cluster_key.ini";
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

    BareStats::AppConfig cfg;
    cfg.cluster.clusterKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    cfg.cluster.listenPort = 9889;

    bool saved = BareStats::ConfigManager::Save(tempIni, cfg);
    TEST_ASSERT(saved, "Saving config with cluster key must succeed");
    TEST_ASSERT(std::filesystem::exists(tempIni), "Saved file must exist");

#if defined(_WIN32)
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PACL dacl = nullptr;
    DWORD res = GetNamedSecurityInfoA(
        tempIni.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &pSD
    );
    TEST_ASSERT(res == ERROR_SUCCESS, "GetNamedSecurityInfo must succeed");

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(pSD, &control, &revision);
    bool is_protected = (control & SE_DACL_PROTECTED) != 0;

    LocalFree(pSD);
    std::filesystem::remove(tempIni, ec);

    // Secure requirement: Config file containing private cluster key must have protected/hardened ACLs.
    // Pre-patch failure: is_protected is false (file inherits default world-readable directory ACLs).
    // Post-patch pass: is_protected is true (DACL is explicitly protected and hardened).
    TEST_ASSERT(is_protected, "config.ini containing private cluster key must have protected/hardened ACLs");
#else
    struct stat st{};
    int stat_res = stat(tempIni.c_str(), &st);
    std::filesystem::remove(tempIni, ec);
    TEST_ASSERT(stat_res == 0, "stat must succeed");
    // Secure requirement: Config file containing private cluster key must have restricted mode 0600.
    // Pre-patch failure: st.st_mode & 0077 != 0 (group/others have read permissions).
    // Post-patch pass: st.st_mode & 0077 == 0.
    TEST_ASSERT((st.st_mode & 0077) == 0, "config.ini containing private cluster key must be mode 0600");
#endif

    return true;
}

bool Test_Config_LayoutSectionAndClusterAliases() {
    std::string tempIni = GetTestModuleDir() + "temp_test_layout_aliases.ini";
    {
        std::ofstream out(tempIni);
        out << "[General]\n"
            << "headless = 1\n"
            << "show_tray_icon = 1\n\n"
            << "[Layout]\n"
            << "mode = grid\n"
            << "order = RAM, CPU, NET\n"
            << "max_gpus_per_row = 3\n"
            << "cell_gap = 20\n"
            << "row_gap = 8\n"
            << "grid_row_1 = cpu | ram | net\n"
            << "grid_row_2 = gpu\n\n"
            << "[Cluster.Aliases]\n"
            << "rig1 = MacMini1\n"
            << "rig2 = LinuxRig\n";
    }

    BareStats::AppConfig loaded = BareStats::ConfigManager::Load(tempIni);
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

    TEST_ASSERT(loaded.headless == true, "headless should be true");
    TEST_ASSERT(loaded.showTrayIcon == true, "show_tray_icon should be true");
    TEST_ASSERT(loaded.layout == BareStats::OverlayLayout::Grid, "layout should be Grid");
    TEST_ASSERT(loaded.maxGpusPerRow == 3, "max_gpus_per_row should be 3");
    TEST_ASSERT(loaded.cellGap == 20, "cell_gap should be 20");
    TEST_ASSERT(loaded.rowGap == 8, "row_gap should be 8");
    TEST_ASSERT(loaded.metricOrder.size() == 3, "metricOrder size should be 3");
    TEST_ASSERT(loaded.metricOrder[0] == "RAM" && loaded.metricOrder[1] == "CPU" && loaded.metricOrder[2] == "NET",
                "metricOrder must match RAM, CPU, NET");

    TEST_ASSERT(loaded.cluster.aliases.count("rig1") == 1, "Cluster alias rig1 must exist");
    TEST_ASSERT(loaded.cluster.aliases["rig1"] == "MacMini1", "Cluster alias rig1 must map to MacMini1");
    TEST_ASSERT(loaded.cluster.aliases["rig2"] == "LinuxRig", "Cluster alias rig2 must map to LinuxRig");

    // Test IsMetricInActiveLayout
    TEST_ASSERT(loaded.IsMetricInActiveLayout("cpu") == true, "CPU must be active in layout");
    TEST_ASSERT(loaded.IsMetricInActiveLayout("ram") == true, "RAM must be active in layout");
    TEST_ASSERT(loaded.IsMetricInActiveLayout("net") == true, "NET must be active in layout");
    TEST_ASSERT(loaded.IsMetricInActiveLayout("gpu") == true, "GPU must be active in Grid layout");

    return true;
}

bool Test_Formatter_GridOrderAndRigAliases() {
    BareStats::AppConfig cfg;
    cfg.layout = BareStats::OverlayLayout::Grid;
    cfg.metricOrder = { "RAM", "CPU", "NET" };
    cfg.showCpu = true;
    cfg.showRam = true;
    cfg.showNet = true;
    cfg.cluster.aliases["rig1"] = "MacMini1";

    BareStats::SystemMetrics metrics;
    metrics.cpu.utilizationPercent = 45.0;
    metrics.memory.usedBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    metrics.memory.totalBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
    metrics.network.isAvailable = true;
    metrics.network.rxBytesPerSec = 1024 * 500;
    metrics.network.txBytesPerSec = 1024 * 250;

    auto rows = BareStats::Formatter::FormatRows(metrics, &cfg);
    TEST_ASSERT(!rows.empty(), "FormatRows must return at least one row");

    // Convert row 0 to plain string
    std::wstring row0Text;
    for (const auto& seg : rows[0]) {
        row0Text += seg.text;
    }
    // Verify RAM appears before CPU in row 0
    size_t ramPos = row0Text.find(L"RAM");
    size_t cpuPos = row0Text.find(L"CPU");
    TEST_ASSERT(ramPos != std::wstring::npos, "RAM must be in row 0");
    TEST_ASSERT(cpuPos != std::wstring::npos, "CPU must be in row 0");
    TEST_ASSERT(ramPos < cpuPos, "RAM must precede CPU when order is RAM, CPU, NET");

    // Test remote rig aliasing in FormatCell
    metrics.cluster.rig_count = 1;
    std::strncpy(metrics.cluster.rigs[0].rig_id, "rig1", sizeof(metrics.cluster.rigs[0].rig_id));
    metrics.cluster.rigs[0].is_online = true;
    metrics.cluster.rigs[0].cpu_load_pct = 75;

    auto rigSegs = BareStats::Formatter::FormatCell("rig1:cpu", metrics, &cfg);
    std::wstring rigCellText;
    for (const auto& seg : rigSegs) {
        rigCellText += seg.text;
    }
    TEST_ASSERT(rigCellText.find(L"MacMini1") != std::wstring::npos,
                "FormatCell must display friendly alias MacMini1 instead of rig1");

    return true;
}

// AUDIT-P5-02: [General] show_tray_icon priority over legacy [Appearance] and migration protection
bool Test_Config_ShowTrayIconPriorityAndMigration() {
    std::string tempIni = GetTestModuleDir() + "temp_test_tray_priority.ini";
    std::error_code ec;

    // Test 1: [General] show_tray_icon = 1 takes priority over [Appearance] show_tray_icon = 0
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[General]\n"
            << "show_tray_icon = 1\n"
            << "[Appearance]\n"
            << "show_tray_icon = 0\n";
    }
    BareStats::AppConfig cfg1 = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(cfg1.showTrayIcon == true, "[General] show_tray_icon = 1 must override [Appearance] = 0");

    // Test 2: [General] show_tray_icon = 0 takes priority over [Appearance] show_tray_icon = 1
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[General]\n"
            << "show_tray_icon = 0\n"
            << "[Appearance]\n"
            << "show_tray_icon = 1\n";
    }
    BareStats::AppConfig cfg2 = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(cfg2.showTrayIcon == false, "[General] show_tray_icon = 0 must override [Appearance] = 1");

    // Test 3: Legacy fallback when only [Appearance] defines show_tray_icon
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[Appearance]\n"
            << "show_tray_icon = 1\n";
    }
    BareStats::AppConfig cfg3 = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(cfg3.showTrayIcon == true, "Legacy [Appearance] show_tray_icon = 1 must be honored when [General] is absent");

    // Test 4: Migration on legacy file with [General] show_tray_icon does not inject into [Appearance]
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 130\n"
            << "[General]\n"
            << "show_tray_icon = 1\n"
            << "[Appearance]\n"
            << "opacity = 90\n";
    }
    BareStats::ConfigManager::MigrateSchema(tempIni, 130, 140);
    std::ifstream in(tempIni);
    std::string migratedContent((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Check that appearance section does not contain duplicate show_tray_icon = 0
    size_t appPos = migratedContent.find("[Appearance]");
    if (appPos != std::string::npos) {
        size_t nextSec = migratedContent.find('[', appPos + 1);
        std::string appSec = migratedContent.substr(appPos, (nextSec != std::string::npos) ? (nextSec - appPos) : std::string::npos);
        TEST_ASSERT(appSec.find("show_tray_icon") == std::string::npos,
                    "MigrateSchema must not inject show_tray_icon into [Appearance]");
    }

    // Test 5: Legacy [Appearance] show_tray_icon = 0 without [General] recovers to true on Windows/macOS
#if defined(_WIN32) || defined(__APPLE__)
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[Appearance]\n"
            << "show_tray_icon = 0\n";
    }
    BareStats::AppConfig cfg5 = BareStats::ConfigManager::Load(tempIni);
    TEST_ASSERT(cfg5.showTrayIcon == true, "Legacy [Appearance] show_tray_icon = 0 without [General] must recover to true on Windows/macOS");
#endif

    // Test 6: Migration on legacy file without show_tray_icon injects show_tray_icon = 1 into [General]
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 130\n"
            << "[Appearance]\n"
            << "opacity = 90\n";
    }
    BareStats::ConfigManager::MigrateSchema(tempIni, 130, 140);
    std::ifstream in6(tempIni);
    std::string migrated6((std::istreambuf_iterator<char>(in6)), std::istreambuf_iterator<char>());
    in6.close();
    size_t genPos6 = migrated6.find("[General]");
    TEST_ASSERT(genPos6 != std::string::npos, "MigrateSchema must ensure [General] exists for show_tray_icon");
    if (genPos6 != std::string::npos) {
        size_t nextSec6 = migrated6.find('[', genPos6 + 1);
        std::string genSec6 = migrated6.substr(genPos6, (nextSec6 != std::string::npos) ? (nextSec6 - genPos6) : std::string::npos);
        TEST_ASSERT(genSec6.find("show_tray_icon = 1") != std::string::npos,
                    "MigrateSchema must inject show_tray_icon = 1 into [General]");
    }

    std::filesystem::remove(tempIni, ec);
    std::string bakFile = tempIni + ".bak";
    std::filesystem::remove(bakFile, ec);
    return true;
}

// 7. Unified Layout, Case-Sensitivity & Configuration Deduplication
bool Test_UnifiedLayout_CaseSensitivityAndDeduplication() {
    std::string tempIni = GetTestModuleDir() + "test_unified_layout.ini";
    std::error_code ec;
    std::filesystem::remove(tempIni, ec);

    // Test 1: Case-insensitivity & Synonym Aliases in Horizontal Mode
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[Layout]\n"
            << "mode = horizontal\n"
            << "order = processor, graphics, memory, wifi\n"
            << "[Telemetry]\n"
            << "show_net = 1\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        TEST_ASSERT(cfg.layout == BareStats::OverlayLayout::Horizontal, "Layout must be Horizontal");
        TEST_ASSERT(cfg.targetAllGpus == true, "Generic graphics alias must auto-target all GPUs");
        TEST_ASSERT(cfg.IsMetricInActiveLayout("cpu"), "Processor synonym must activate CPU");
        TEST_ASSERT(cfg.IsMetricInActiveLayout("gpu"), "Graphics synonym must activate GPU");
        TEST_ASSERT(cfg.IsMetricInActiveLayout("ram"), "Memory synonym must activate RAM");
        TEST_ASSERT(cfg.IsMetricInActiveLayout("net"), "Wifi synonym must activate NET");

        BareStats::SystemMetrics metrics{};
        metrics.cpu.utilizationPercent = 25.0;
        metrics.memory.totalBytes = 16ULL * 1024 * 1024 * 1024;
        metrics.memory.usedBytes = 8ULL * 1024 * 1024 * 1024;
        metrics.network.isAvailable = true;
        metrics.network.rxBytesPerSec = 1024 * 1024;
        metrics.gpu.isAvailable = true;
        metrics.gpu.utilizationPercent = 50.0;

        auto segs = BareStats::Formatter::FormatSegments(metrics, &cfg);
        TEST_ASSERT(!segs.empty(), "Segments must not be empty with synonyms in order");
        bool hasCpu = false, hasGpu = false, hasRam = false, hasNet = false;
        for (const auto& s : segs) {
            if (s.text.find(L"CPU") != std::wstring::npos) hasCpu = true;
            if (s.text.find(L"GPU") != std::wstring::npos) hasGpu = true;
            if (s.text.find(L"RAM") != std::wstring::npos) hasRam = true;
            if (s.text.find(L"NET") != std::wstring::npos) hasNet = true;
        }
        TEST_ASSERT(hasCpu && hasGpu && hasRam && hasNet, "All synonym segments (CPU, GPU, RAM, NET) must be rendered");
    }

    // Test 2: Specific GPU indexing in Horizontal Mode
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[Layout]\n"
            << "mode = horizontal\n"
            << "order = CPU, GPU1, RAM\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        TEST_ASSERT(cfg.targetAllGpus == true, "GPU1 in order must auto-enable multi-GPU collection");

        BareStats::SystemMetrics metrics{};
        metrics.cpu.utilizationPercent = 10.0;
        metrics.memory.totalBytes = 8ULL * 1024 * 1024 * 1024;
        metrics.memory.usedBytes = 4ULL * 1024 * 1024 * 1024;
        BareStats::GpuMetrics g0{};
        g0.isAvailable = true;
        g0.utilizationPercent = 15.0;
        BareStats::GpuMetrics g1{};
        g1.isAvailable = true;
        g1.utilizationPercent = 75.0;
        metrics.gpus.push_back(g0);
        metrics.gpus.push_back(g1);

        auto segs = BareStats::Formatter::FormatSegments(metrics, &cfg);
        bool hasGpu0 = false, hasGpu1 = false;
        for (const auto& s : segs) {
            if (s.text.find(L"GPU0") != std::wstring::npos) hasGpu0 = true;
            if (s.text.find(L"GPU1") != std::wstring::npos) hasGpu1 = true;
        }
        TEST_ASSERT(!hasGpu0, "GPU0 must not be rendered when order specifies GPU1");
        TEST_ASSERT(hasGpu1, "GPU1 must be rendered when order specifies GPU1");
    }

    // Test 3: Auto-Bridge from order to CustomGrid
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[Layout]\n"
            << "mode = custom_grid\n"
            << "order = cpu, ram, net, gpu0, gpu1\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        TEST_ASSERT(cfg.layout == BareStats::OverlayLayout::CustomGrid, "Layout must be CustomGrid");
        TEST_ASSERT(!cfg.customGrid.IsEmpty(), "CustomGrid must be auto-bridged from order");
        TEST_ASSERT(cfg.customGrid.numRows == 2, "Bridged CustomGrid must have 2 rows");
        TEST_ASSERT(cfg.customGrid.rows[0].cells.size() == 3, "Row 0 must contain non-GPU metrics (cpu, ram, net)");
        TEST_ASSERT(cfg.customGrid.rows[1].cells.size() == 2, "Row 1 must contain GPU metrics (gpu0, gpu1)");
    }

    // Test 4: Deduplication and Canonical Save
    {
        std::ofstream out(tempIni);
        out << "[BareStats]\n"
            << "schema_version = 140\n"
            << "[General]\n"
            << "headless = 0\n"
            << "show_tray_icon = 1\n"
            << "[Position]\n"
            << "preset = custom\n"
            << "custom_x = 100\n"
            << "custom_y = 200\n"
            << "layout = custom_grid\n"
            << "grid_mode = unified\n"
            << "cell_gap = 16\n"
            << "row_gap = 4\n"
            << "max_gpus_per_row = 2\n"
            << "[Layout]\n"
            << "mode = horizontal\n"
            << "order = CPU, GPU, RAM, NET\n"
            << "[Appearance]\n"
            << "opacity = 100\n"
            << "show_tray_icon = 1\n"
            << "[Telemetry]\n"
            << "refresh_interval_ms = 1000\n"
            << "order = CPU, GPU, RAM, NET\n";
        out.close();

        BareStats::AppConfig cfg = BareStats::ConfigManager::Load(tempIni);
        bool saveOk = BareStats::ConfigManager::Save(tempIni, cfg);
        TEST_ASSERT(saveOk, "ConfigManager::Save must succeed");

        BareStats::IniDocument doc;
        doc.Load(tempIni);

        // Verify [Position] does NOT contain layout keys
        TEST_ASSERT(!doc.HasKey("Position", "layout"), "[Position] must not retain 'layout'");
        TEST_ASSERT(!doc.HasKey("Position", "mode"), "[Position] must not retain 'mode'");
        TEST_ASSERT(!doc.HasKey("Position", "grid_mode"), "[Position] must not retain 'grid_mode'");
        TEST_ASSERT(!doc.HasKey("Position", "cell_gap"), "[Position] must not retain 'cell_gap'");
        TEST_ASSERT(!doc.HasKey("Position", "row_gap"), "[Position] must not retain 'row_gap'");
        TEST_ASSERT(!doc.HasKey("Position", "max_gpus_per_row"), "[Position] must not retain 'max_gpus_per_row'");

        // Verify [Appearance] does NOT contain show_tray_icon
        TEST_ASSERT(!doc.HasKey("Appearance", "show_tray_icon"), "[Appearance] must not retain 'show_tray_icon'");

        // Verify [Telemetry] does NOT contain order
        TEST_ASSERT(!doc.HasKey("Telemetry", "order"), "[Telemetry] must not retain 'order'");

        // Verify canonical [Layout] has keys
        TEST_ASSERT(doc.HasKey("Layout", "mode"), "[Layout] must have 'mode'");
        TEST_ASSERT(doc.HasKey("Layout", "order"), "[Layout] must have 'order'");
        TEST_ASSERT(doc.HasKey("Layout", "grid_mode"), "[Layout] must have 'grid_mode'");
        TEST_ASSERT(doc.HasKey("Layout", "cell_gap"), "[Layout] must have 'cell_gap'");
        TEST_ASSERT(doc.HasKey("Layout", "row_gap"), "[Layout] must have 'row_gap'");

        // Verify [General] retains show_tray_icon
        TEST_ASSERT(doc.HasKey("General", "show_tray_icon"), "[General] must retain 'show_tray_icon'");
    }

    std::filesystem::remove(tempIni, ec);
    return true;
}



