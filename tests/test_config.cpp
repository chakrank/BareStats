#include "test_common.h"
#include "../src/config/config.h"
#include "../src/config/ini_parser.h"

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

    TEST_ASSERT(diskContent.find("schema_version = 130") != std::string::npos,
                "Migrated file must contain schema_version = 130");
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

