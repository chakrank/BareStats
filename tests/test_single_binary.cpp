#include "test_common.h"
#include "util/os_utils.h"
#include "config/app_mode.h"
#include "config/config.h"
#include "metrics/telemetry_engine.h"
#include "metrics/network/udp_streamer.h"
#include "util/crypto/crypto_box.h"
#include "util/logger.h"
#include "util/formatter.h"
#include "metrics/llm/llm_resolver.h"
#include "metrics/llm/model_name_condenser.h"
#include "overlay/linux/gui_subsystem_linux.h"
#include <filesystem>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
using namespace BareStats;

bool Test_SingleBinary_ExecutableDirectoryResolution() {
    fs::path exe_dir = GetExecutableDirectory();

    TEST_ASSERT(!exe_dir.empty(), "Executable directory must not be empty");
    TEST_ASSERT(fs::exists(exe_dir), "Executable directory must exist on disk");
    TEST_ASSERT(fs::is_directory(exe_dir), "Executable directory path must be a directory");

    return true;
}

bool Test_SingleBinary_EvaluateAppMode() {
    // Mode Matrix:
    // config.ini present, satellite.ini absent -> CommandCenter
    TEST_ASSERT(EvaluateAppMode(true, false) == AppMode::CommandCenter,
                "config only must resolve to CommandCenter");

    // config.ini absent, satellite.ini present -> HeadlessSatellite
    TEST_ASSERT(EvaluateAppMode(false, true) == AppMode::HeadlessSatellite,
                "satellite only must resolve to HeadlessSatellite");

    // config.ini present, satellite.ini present -> Hybrid
    TEST_ASSERT(EvaluateAppMode(true, true) == AppMode::Hybrid,
                "both config and satellite present must resolve to Hybrid");

    // config.ini absent, satellite.ini absent -> FirstRun
    TEST_ASSERT(EvaluateAppMode(false, false) == AppMode::FirstRun,
                "neither present must resolve to FirstRun");

    // config.headless == true overrides to HeadlessSatellite
    TEST_ASSERT(EvaluateAppMode(true, false, true) == AppMode::HeadlessSatellite,
                "config with headless=true must override to HeadlessSatellite");

    return true;
}

bool Test_SingleBinary_TelemetryEngineAppMode() {
    TelemetryEngine engine;

    engine.SetAppMode(AppMode::HeadlessSatellite);
    TEST_ASSERT(engine.GetAppMode() == AppMode::HeadlessSatellite, "SetAppMode must persist mode");

    engine.StartBackgroundWorkers(AppMode::CommandCenter);
    TEST_ASSERT(engine.GetAppMode() == AppMode::CommandCenter, "StartBackgroundWorkers must update mode");
    TEST_ASSERT(engine.IsRunning(), "Engine must be running after StartBackgroundWorkers");

    // Snapshot update and read
    RigClusterSnapshot cluster{};
    cluster.rig_count = 1;
    std::strncpy(cluster.rigs[0].rig_id, "rig1", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;

    engine.UpdateClusterSnapshot(cluster);
    RigClusterSnapshot read_back = engine.GetClusterSnapshot();
    TEST_ASSERT(read_back.rig_count == 1, "Cluster snapshot must retain 1 rig");
    TEST_ASSERT(std::string(read_back.rigs[0].rig_id) == "rig1", "Rig id must match rig1");
    TEST_ASSERT(read_back.rigs[0].snapshot.is_online, "Rig snapshot must be online");

    engine.StopBackgroundWorkers();
    TEST_ASSERT(!engine.IsRunning(), "Engine must not be running after StopBackgroundWorkers");

    return true;
}

bool Test_SingleBinary_StartBackgroundWorkers_CommandCenter() {
    uint8_t key[Crypto::KEY_SIZE] = {0};
    Crypto::GenerateKey(key);
    std::string hexKey = Crypto::KeyToHex(key);

    AppConfig config;
    config.cluster.clusterKey = hexKey;
    config.cluster.listenPort = 19889; // Unique test port
    config.llm.enabled = true;
    config.llm.endpoints = "local=http://127.0.0.1:11434,rig1=http://192.168.1.100:8000";

    TelemetryEngine engine(config);
    TEST_ASSERT(!engine.IsReceiverActive(), "Receiver must not be active before start");
    TEST_ASSERT(engine.GetLlmCollectorCount() == 2, "Engine must initialize 2 LLM collectors");

    engine.StartBackgroundWorkers(AppMode::CommandCenter);
    TEST_ASSERT(engine.GetAppMode() == AppMode::CommandCenter, "AppMode must be CommandCenter");
    TEST_ASSERT(engine.IsRunning(), "Engine must be running");
    TEST_ASSERT(engine.IsReceiverActive(), "Receiver must be actively bound in CommandCenter mode");

    engine.StopBackgroundWorkers();
    TEST_ASSERT(!engine.IsRunning(), "Engine must stop");
    TEST_ASSERT(!engine.IsReceiverActive(), "Receiver must close after stop");

    return true;
}

bool Test_SingleBinary_FeedDeduplication() {
    TelemetryEngine engine;

    TEST_ASSERT(!engine.IsUdpFeedActive("rig1"), "rig1 UDP feed must initially be inactive");

    engine.RecordUdpFeedActivity("rig1");
    TEST_ASSERT(engine.IsUdpFeedActive("rig1"), "rig1 UDP feed must be active after activity recorded");
    TEST_ASSERT(!engine.IsUdpFeedActive("rig2"), "rig2 UDP feed must be inactive");

    LlmSnapshot snap{};
    snap.is_online = true;
    snap.llm_count = 1;
    std::strncpy(snap.llms[0].model_name, "deepseek-7b", sizeof(snap.llms[0].model_name) - 1);
    snap.llms[0].tps = 42.5f;

    engine.UpdateRigInCluster("rig1", snap);

    RigClusterSnapshot cluster = engine.GetClusterSnapshot();
    TEST_ASSERT(cluster.rig_count == 1, "Cluster must have 1 rig");
    TEST_ASSERT(std::string(cluster.rigs[0].rig_id) == "rig1", "Rig id must be rig1");
    TEST_ASSERT(cluster.rigs[0].snapshot.is_online, "Rig snapshot must be online");
    TEST_ASSERT(cluster.rigs[0].snapshot.llm_count == 1, "Model count must be 1");
    TEST_ASSERT(std::string(cluster.rigs[0].snapshot.llms[0].model_name) == "deepseek-7b", "Model name must match");
    TEST_ASSERT(cluster.rigs[0].snapshot.llms[0].tps > 42.0f && cluster.rigs[0].snapshot.llms[0].tps < 43.0f, "TPS must match");

    return true;
}

bool Test_SingleBinary_DynamicLoaderIsolation() {
    bool available = CheckGuiSubsystemAvailable();
    (void)available;

    TelemetryEngine engine;
    AppConfig cfg;
    bool ok = InitializeGuiSubsystem(engine, cfg, "config.ini");
    (void)ok;
    TEST_ASSERT(true, "Dynamic loader check must complete without SIGSEGV or crash");

    return true;
}

bool Test_SingleBinary_InMemoryDefaultsAndNonDestructiveLoad() {
    fs::path tempNonExistent = fs::temp_directory_path() / "barestats_test_nonexistent_config.ini";
    std::error_code ec;
    fs::remove(tempNonExistent, ec);

    // 1. Verify loading non-existent path returns in-memory defaults
    AppConfig cfg = ConfigManager::Load(tempNonExistent.string());
    TEST_ASSERT(cfg.preset == PositionPreset::TopRight, "In-memory default preset must be TopRight");
    TEST_ASSERT(cfg.margin == 24, "In-memory default margin must be 24");
    TEST_ASSERT(!cfg.headless, "In-memory default headless must be false");

    // 2. Critical Check: config.ini must NOT have been created on disk!
    TEST_ASSERT(!fs::exists(tempNonExistent), "Load() must never write config.ini when file does not exist");

    return true;
}

bool Test_SingleBinary_StrictBinaryHeadlessParsing() {
    fs::path tempIni = fs::temp_directory_path() / "barestats_test_headless.ini";
    std::error_code ec;

    auto testValue = [&](const std::string& val, bool expected) {
        std::ofstream ofs(tempIni, std::ios::trunc);
        ofs << "[BareStats]\nschema_version = 140\n[General]\nheadless = " << val << "\n";
        ofs.close();

        AppConfig cfg = ConfigManager::Load(tempIni.string());
        fs::remove(tempIni, ec);
        return cfg.headless == expected;
    };

    // Truthy values
    TEST_ASSERT(testValue("1", true), "headless = 1 must evaluate to true");
    TEST_ASSERT(testValue("true", true), "headless = true must evaluate to true");
    TEST_ASSERT(testValue("yes", true), "headless = yes must evaluate to true");
    TEST_ASSERT(testValue("ON", true), "headless = ON must evaluate to true");

    // Falsy / Default values
    TEST_ASSERT(testValue("0", false), "headless = 0 must evaluate to false");
    TEST_ASSERT(testValue("false", false), "headless = false must evaluate to false");
    TEST_ASSERT(testValue("no", false), "headless = no must evaluate to false");
    TEST_ASSERT(testValue("invalid_string", false), "headless = invalid must evaluate to false");
    TEST_ASSERT(testValue("", false), "empty headless must evaluate to false");

    return true;
}

bool Test_SingleBinary_LoggerRotation() {
    fs::path tempLogDir = fs::temp_directory_path() / "barestats_test_log";
    std::error_code ec;
    fs::remove_all(tempLogDir, ec);
    fs::create_directories(tempLogDir, ec);
    fs::path testLog = tempLogDir / "barestats_test.log";

    Logger::Shutdown();
    Logger::SetLogPath(testLog);
    Logger::SetMaxSizeBytes(150); // Small limit to trigger rotation

    Logger::Info("TEST", "Message 1 for logger rotation verification");
    TEST_ASSERT(fs::exists(testLog), "Active log file must be created on write");

    // Write enough messages to exceed 150 bytes
    for (int i = 0; i < 5; ++i) {
        Logger::Info("TEST", "Message " + std::to_string(i + 2) + " to trigger log rotation threshold");
    }

    Logger::Flush();
    Logger::Shutdown();

    // Check directory for rotated file (barestats_test.log.YYYY-MM-DD)
    bool foundRotated = false;
    for (const auto& entry : fs::directory_iterator(tempLogDir)) {
        std::string filename = entry.path().filename().string();
        if (filename.find("barestats_test.log.") == 0) {
            foundRotated = true;
            break;
        }
    }

    TEST_ASSERT(foundRotated, "Logger must rotate to date-based filename when size exceeds limit");

    // Cleanup
    fs::remove_all(tempLogDir, ec);
    Logger::SetMaxSizeBytes(2 * 1024 * 1024); // restore default 2MB
    return true;
}

// BS-VULN-08: Maximum rotated log archive retention enforcement
bool Test_Logger_MaxRotatedArchivesRetention() {
    fs::path tempLogDir = fs::temp_directory_path() / "barestats_retention_log";
    std::error_code ec;
    fs::remove_all(tempLogDir, ec);
    fs::create_directories(tempLogDir, ec);
    fs::path testLog = tempLogDir / "barestats_retention.log";

    Logger::Shutdown();
    Logger::SetLogPath(testLog);
    Logger::SetMaxSizeBytes(80); // Small limit to trigger rotation frequently
    Logger::SetMaxRotatedArchives(3); // Retain at most 3 archives

    // Trigger rotation 8 times by logging messages that exceed 80 bytes
    for (int rot = 0; rot < 8; ++rot) {
        Logger::Info("ROT", "Trigger rotation cycle number " + std::to_string(rot) + " with sufficient length");
        Logger::Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Logger::Flush();
    Logger::Shutdown();

    // Count rotated archive files matching barestats_retention.log.*
    size_t rotatedCount = 0;
    for (const auto& entry : fs::directory_iterator(tempLogDir, ec)) {
        std::string filename = entry.path().filename().string();
        if (filename.find("barestats_retention.log.") == 0) {
            rotatedCount++;
        }
    }

    // Cleanup
    fs::remove_all(tempLogDir, ec);
    Logger::SetMaxSizeBytes(2 * 1024 * 1024);
    Logger::SetMaxRotatedArchives(5);

    TEST_ASSERT(rotatedCount > 0, "At least one rotation must have occurred");
    TEST_ASSERT(rotatedCount <= 3, "Rotated archives must be capped at max 3");

    return true;
}

bool Test_SingleBinary_NoCrossRigLlmContamination() {
    // 1. Construct cluster with 'local' running Ollama, and 'rig1' having no LLM
    RigClusterSnapshot cluster{};
    cluster.rig_count = 2;

    // Node 0: local (Command Center local Ollama)
    std::strncpy(cluster.rigs[0].rig_id, "local", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;
    cluster.rigs[0].snapshot.llm_count = 1;
    auto& localModel = cluster.rigs[0].snapshot.llms[0];
    std::strncpy(localModel.model_name, "Qwen2.5-Coder:7b", sizeof(localModel.model_name) - 1);
    localModel.tps = 24.5f;
    localModel.vram_used_mb = 4096;
    localModel.state = Wire::LlmState::Idle;

    // Node 1: rig1 (Remote satellite with physical GPU/CPU/RAM, but NO LLM running)
    std::strncpy(cluster.rigs[1].rig_id, "rig1", sizeof(cluster.rigs[1].rig_id) - 1);
    cluster.rigs[1].cpu_load_pct = 18;
    cluster.rigs[1].ram_used_mb = 4000;
    cluster.rigs[1].ram_total_mb = 16000;
    cluster.rigs[1].gpu_count = 1;
    cluster.rigs[1].gpus[0].utilization_pct = 45;
    cluster.rigs[1].gpus[0].vram_used_mb = 2048;
    cluster.rigs[1].gpus[0].vram_total_mb = 8192;
    cluster.rigs[1].snapshot.is_online = true;
    cluster.rigs[1].snapshot.llm_count = 0; // Rig1 is NOT running an LLM!

    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    // 2. Targeted lookup tests:
    // rig1:llm must return nullptr and format as "Idle" (NEVER bleed local's Qwen into rig1)
    const Wire::LlmMetrics* rig1Model = ResolveRigModel(cluster, "rig1", -1);
    TEST_ASSERT(rig1Model == nullptr, "ResolveRigModel('rig1') must return nullptr when rig1 has no LLMs");

    GridToken rig1Token = ParseGridToken("rig1:llm");
    std::string rig1Text = FormatGridCellText(rig1Token, cluster, condenser);
    TEST_ASSERT(rig1Text == "Idle", "FormatGridCellText('rig1:llm') must return 'Idle'");

    // local:llm must return local's Qwen model
    const Wire::LlmMetrics* localM = ResolveRigModel(cluster, "local", -1);
    TEST_ASSERT(localM != nullptr, "ResolveRigModel('local') must find local model");
    TEST_ASSERT(std::string(localM->model_name).find("Qwen") != std::string::npos, "Local model must be Qwen");

    GridToken localToken = ParseGridToken("local:llm");
    std::string localText = FormatGridCellText(localToken, cluster, condenser);
    TEST_ASSERT(localText.find("Qwen") != std::string::npos, "local:llm must contain Qwen");

    // Generic "llm" (Command Center) must resolve to local's model via generic fallback
    const Wire::LlmMetrics* genericM = ResolveGenericLlm(cluster);
    TEST_ASSERT(genericM != nullptr, "ResolveGenericLlm must find active model in cluster");
    TEST_ASSERT(std::string(genericM->model_name).find("Qwen") != std::string::npos, "Generic model must be Qwen");

    // 3. High-level Formatter::FormatCell validation
    SystemMetrics sys{};
    sys.cluster = cluster;
    AppConfig cfg;
    cfg.hideOfflineMetrics = false;

    // Formatting "rig1:llm"
    auto r1Segs = Formatter::FormatCell("rig1:llm", sys, &cfg);
    TEST_ASSERT(!r1Segs.empty(), "FormatCell('rig1:llm') must return segments");
    bool foundRig1Label = false;
    bool foundIdle = false;
    bool foundQwenInRig1 = false;
    for (const auto& s : r1Segs) {
        if (s.text.find(L"rig1") != std::wstring::npos) foundRig1Label = true;
        if (s.text.find(L"Idle") != std::wstring::npos) foundIdle = true;
        if (s.text.find(L"Qwen") != std::wstring::npos) foundQwenInRig1 = true;
    }
    TEST_ASSERT(foundRig1Label, "rig1:llm output must contain rig1 label");
    TEST_ASSERT(foundIdle, "rig1:llm output must contain Idle");
    TEST_ASSERT(!foundQwenInRig1, "rig1:llm output must NOT contain local Qwen model!");

    // Formatting "local:llm"
    auto locSegs = Formatter::FormatCell("local:llm", sys, &cfg);
    TEST_ASSERT(!locSegs.empty(), "FormatCell('local:llm') must return segments");
    bool foundLocalLabel = false;
    bool foundQwenInLocal = false;
    for (const auto& s : locSegs) {
        if (s.text.find(L"local") != std::wstring::npos) foundLocalLabel = true;
        if (s.text.find(L"Qwen") != std::wstring::npos) foundQwenInLocal = true;
    }
    TEST_ASSERT(foundLocalLabel, "local:llm output must contain local label");
    TEST_ASSERT(foundQwenInLocal, "local:llm output must contain Qwen model");

    // Formatting "LLM" (generic)
    auto genSegs = Formatter::FormatCell("llm", sys, &cfg);
    TEST_ASSERT(!genSegs.empty(), "FormatCell('llm') must return segments");
    bool foundQwenInGen = false;
    for (const auto& s : genSegs) {
        if (s.text.find(L"Qwen") != std::wstring::npos) foundQwenInGen = true;
    }
    TEST_ASSERT(foundQwenInGen, "Generic 'llm' must contain Qwen model");

    return true;
}

bool Test_SingleBinary_UdpSatelliteTimeoutAndOfflineTransition() {
    TelemetryEngine engine;
    engine.SetAppMode(AppMode::CommandCenter);

    // 1. Populate cluster with an active remote rig "rig1"
    RigClusterSnapshot cluster{};
    cluster.rig_count = 1;
    std::strncpy(cluster.rigs[0].rig_id, "rig1", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;
    cluster.rigs[0].cpu_load_pct = 75;
    cluster.rigs[0].ram_used_mb = 8192;
    cluster.rigs[0].ram_total_mb = 16384;
    cluster.rigs[0].gpu_count = 1;
    cluster.rigs[0].gpus[0].utilization_pct = 85;
    cluster.rigs[0].gpus[0].vram_used_mb = 4000;
    cluster.rigs[0].gpus[0].vram_total_mb = 8000;
    cluster.rigs[0].snapshot.llm_count = 1;
    std::strncpy(cluster.rigs[0].snapshot.llms[0].model_name, "Llama-3-8B", sizeof(cluster.rigs[0].snapshot.llms[0].model_name) - 1);
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Idle;

    engine.UpdateClusterSnapshot(cluster);

    // Register active UDP feed with very short timeout (50ms)
    engine.RecordUdpFeedActivity("rig1", std::chrono::milliseconds(50));
    TEST_ASSERT(engine.IsUdpFeedActive("rig1"), "rig1 UDP feed must be active immediately after packet");

    // Format metrics while online
    SystemMetrics liveMetrics{};
    liveMetrics.cluster = engine.GetClusterSnapshot();
    AppConfig cfg;
    cfg.hideOfflineMetrics = false;

    auto onlineCpu = Formatter::FormatCell("rig1:cpu", liveMetrics, &cfg);
    TEST_ASSERT(!onlineCpu.empty(), "rig1:cpu must return segments when online");
    bool has75 = false;
    for (const auto& s : onlineCpu) { if (s.text.find(L"75") != std::wstring::npos) has75 = true; }
    TEST_ASSERT(has75, "rig1:cpu must display 75% load");

    auto onlineRam = Formatter::FormatCell("rig1:ram", liveMetrics, &cfg);
    TEST_ASSERT(!onlineRam.empty(), "rig1:ram must return segments when online");

    auto onlineNet = Formatter::FormatCell("rig1:net", liveMetrics, &cfg);
    TEST_ASSERT(!onlineNet.empty(), "rig1:net must return segments when online");
    bool hasNetNA = false;
    for (const auto& s : onlineNet) { if (s.text.find(L"N/A") != std::wstring::npos) hasNetNA = true; }
    TEST_ASSERT(hasNetNA, "rig1:net must show N/A when online");

    auto onlineGpuMissing = Formatter::FormatCell("rig1:gpu1", liveMetrics, &cfg);
    TEST_ASSERT(!onlineGpuMissing.empty(), "rig1:gpu1 must return segments when GPU1 missing");
    bool hasGpuNA = false;
    for (const auto& s : onlineGpuMissing) { if (s.text.find(L"N/A") != std::wstring::npos) hasGpuNA = true; }
    TEST_ASSERT(hasGpuNA, "rig1:gpu1 must show N/A when rig only has 1 GPU");

    // 2. Wait for heartbeat timeout (70ms > 50ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    TEST_ASSERT(!engine.IsUdpFeedActive("rig1"), "rig1 UDP feed must be inactive after 70ms");

    // Watchdog check triggers offline transition
    bool changed = engine.CheckUdpFeedTimeouts();
    TEST_ASSERT(changed, "CheckUdpFeedTimeouts must report state change on heartbeat timeout");

    RigClusterSnapshot deadCluster = engine.GetClusterSnapshot();
    TEST_ASSERT(!deadCluster.rigs[0].snapshot.is_online, "rig1 snapshot must be marked offline");
    TEST_ASSERT(deadCluster.rigs[0].cpu_load_pct == 0, "rig1 cpu_load_pct must be zeroed");
    TEST_ASSERT(deadCluster.rigs[0].ram_used_mb == 0, "rig1 ram_used_mb must be zeroed");
    TEST_ASSERT(deadCluster.rigs[0].gpu_count == 0, "rig1 gpu_count must be zeroed");

    // Second call must return false (no re-trigger / no log spam)
    bool changedAgain = engine.CheckUdpFeedTimeouts();
    TEST_ASSERT(!changedAgain, "CheckUdpFeedTimeouts must return false when already offline");

    // Format metrics while offline
    SystemMetrics deadMetrics{};
    deadMetrics.cluster = deadCluster;

    auto deadCpu = Formatter::FormatCell("rig1:cpu", deadMetrics, &cfg);
    TEST_ASSERT(!deadCpu.empty(), "rig1:cpu must return segments when offline");
    bool cpuOffline = false;
    for (const auto& s : deadCpu) { if (s.text.find(L"Offline") != std::wstring::npos) cpuOffline = true; }
    TEST_ASSERT(cpuOffline, "rig1:cpu must display Offline");

    auto deadGpu = Formatter::FormatCell("rig1:gpu0", deadMetrics, &cfg);
    bool gpuOffline = false;
    for (const auto& s : deadGpu) { if (s.text.find(L"Offline") != std::wstring::npos) gpuOffline = true; }
    TEST_ASSERT(gpuOffline, "rig1:gpu0 must display Offline");

    auto deadLlm = Formatter::FormatCell("rig1:llm", deadMetrics, &cfg);
    bool llmOffline = false;
    for (const auto& s : deadLlm) { if (s.text.find(L"Offline") != std::wstring::npos) llmOffline = true; }
    TEST_ASSERT(llmOffline, "rig1:llm must display Offline");

    auto deadNet = Formatter::FormatCell("rig1:net", deadMetrics, &cfg);
    bool netOffline = false;
    for (const auto& s : deadNet) { if (s.text.find(L"Offline") != std::wstring::npos) netOffline = true; }
    TEST_ASSERT(netOffline, "rig1:net must display Offline");

    // Verify hide_offline_metrics suppression
    AppConfig hideCfg;
    hideCfg.hideOfflineMetrics = true;
    auto hiddenCpu = Formatter::FormatCell("rig1:cpu", deadMetrics, &hideCfg);
    TEST_ASSERT(hiddenCpu.empty(), "rig1:cpu must be hidden when hideOfflineMetrics=true");
    auto hiddenLlm = Formatter::FormatCell("rig1:llm", deadMetrics, &hideCfg);
    TEST_ASSERT(hiddenLlm.empty(), "rig1:llm must be hidden when hideOfflineMetrics=true");

    // 3. Simulate remote rig recovery (rig turns back on)
    engine.RecordUdpFeedActivity("rig1", std::chrono::milliseconds(3500));
    RigClusterSnapshot revived = deadCluster;
    revived.rigs[0].snapshot.is_online = true;
    revived.rigs[0].cpu_load_pct = 40;
    engine.UpdateClusterSnapshot(revived);

    SystemMetrics revivedMetrics{};
    revivedMetrics.cluster = engine.GetClusterSnapshot();
    auto revivedCpu = Formatter::FormatCell("rig1:cpu", revivedMetrics, &cfg);
    bool has40 = false;
    for (const auto& s : revivedCpu) { if (s.text.find(L"40") != std::wstring::npos) has40 = true; }
    TEST_ASSERT(has40, "rig1:cpu must immediately recover to 40% on revival");

    return true;
}

bool Test_SingleBinary_MultiEndpointAggregation() {
    AppConfig config;
    config.llm.enabled = true;
    config.llm.endpoints = "local=http://127.0.0.1:11434, local=http://127.0.0.1:8080, local=http://127.0.0.1:8000, local=http://127.0.0.1:1234";

    TelemetryEngine engine(config);
    TEST_ASSERT(engine.GetLlmCollectorCount() == 4, "Engine must initialize all 4 candidate collectors");

    // Simulate multi-framework model aggregation under the shared 'local' rig
    LlmSnapshot deadSnap{};
    deadSnap.is_online = false;

    LlmSnapshot liveQwen{};
    liveQwen.is_online = true;
    liveQwen.llm_count = 1;
    std::strncpy(liveQwen.llms[0].model_name, "Qwen2.5-Coder-7B", sizeof(liveQwen.llms[0].model_name) - 1);
    liveQwen.llms[0].state = Wire::LlmState::Idle;
    liveQwen.llms[0].vram_used_mb = 5400;

    LlmSnapshot liveLlama{};
    liveLlama.is_online = true;
    liveLlama.llm_count = 1;
    std::strncpy(liveLlama.llms[0].model_name, "Llama-3.1-8B", sizeof(liveLlama.llms[0].model_name) - 1);
    liveLlama.llms[0].state = Wire::LlmState::Generating;
    liveLlama.llms[0].tps = 38.2f;

    // 1. One online, three offline -> 'local' is online with the single active model
    std::vector<LlmSnapshot> mix1 = { deadSnap, liveQwen, deadSnap, deadSnap };
    LlmSnapshot merged1{};
    merged1.is_online = false;
    merged1.llm_count = 0;
    for (const auto& s : mix1) {
        if (s.is_online) {
            merged1.is_online = true;
            for (uint8_t m = 0; m < s.llm_count; ++m) {
                if (merged1.llm_count < Wire::WIRE_MAX_MODELS) {
                    merged1.llms[merged1.llm_count++] = s.llms[m];
                }
            }
        }
    }
    engine.UpdateRigInCluster("local", merged1);

    RigClusterSnapshot cluster1 = engine.GetClusterSnapshot();
    TEST_ASSERT(cluster1.rig_count == 1, "Cluster must have 1 rig");
    TEST_ASSERT(cluster1.rigs[0].snapshot.is_online, "Rig 'local' must be online");
    TEST_ASSERT(cluster1.rigs[0].snapshot.llm_count == 1, "Model count must be 1");
    TEST_ASSERT(std::string(cluster1.rigs[0].snapshot.llms[0].model_name) == "Qwen2.5-Coder-7B", "Model must be Qwen");

    // 2. Two online (e.g. Ollama and llama.cpp simultaneously) -> both models co-reside under 'local'
    std::vector<LlmSnapshot> mix2 = { liveLlama, liveQwen, deadSnap, deadSnap };
    LlmSnapshot merged2{};
    merged2.is_online = false;
    merged2.llm_count = 0;
    for (const auto& s : mix2) {
        if (s.is_online) {
            merged2.is_online = true;
            for (uint8_t m = 0; m < s.llm_count; ++m) {
                if (merged2.llm_count < Wire::WIRE_MAX_MODELS) {
                    merged2.llms[merged2.llm_count++] = s.llms[m];
                }
            }
        }
    }
    engine.UpdateRigInCluster("local", merged2);

    RigClusterSnapshot cluster2 = engine.GetClusterSnapshot();
    TEST_ASSERT(cluster2.rigs[0].snapshot.is_online, "Rig 'local' must be online");
    TEST_ASSERT(cluster2.rigs[0].snapshot.llm_count == 2, "Model count must be 2");

    // Active model resolution selects generating model (Llama) over idle model (Qwen)
    const Wire::LlmMetrics* resolved = ResolveRigModel(cluster2, "local", -1);
    TEST_ASSERT(resolved != nullptr, "ResolveRigModel must find active model");
    TEST_ASSERT(std::string(resolved->model_name) == "Llama-3.1-8B", "Active model must be Llama-3.1-8B");

    // 3. All offline -> 'local' is offline
    std::vector<LlmSnapshot> mix3 = { deadSnap, deadSnap, deadSnap, deadSnap };
    LlmSnapshot merged3{};
    merged3.is_online = false;
    merged3.llm_count = 0;
    engine.UpdateRigInCluster("local", merged3);

    RigClusterSnapshot cluster3 = engine.GetClusterSnapshot();
    TEST_ASSERT(!cluster3.rigs[0].snapshot.is_online, "Rig 'local' must be offline when all candidates offline");

    return true;
}

// VULN-07: Unauthenticated header log sanitization regression test
bool Test_UdpStreamer_UnauthenticatedHeaderLogSanitization() {
    std::string tempLog = GetTestModuleDir() + "temp_test_security_log.log";
    std::error_code ec;
    std::filesystem::remove(tempLog, ec);

    Logger::Init(tempLog);

    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint16_t test_port = 19897;

    UdpReceiver receiver;
    TEST_ASSERT(receiver.Bind(test_port, priv_key), "Receiver bind");

    // Transmitter configured with WRONG key and malicious rig_id containing CRLF
    uint8_t wrong_key[32] = {0};
    Crypto::GenerateRandomBytes(wrong_key, sizeof(wrong_key));
    uint8_t wrong_pub[32] = {0};
    Crypto::DerivePublicKey(wrong_key, wrong_pub);

    UdpTransmitter transmitter;
    std::string malicious_rig_id = "rig1\r\nINJECT\n";
    TEST_ASSERT(transmitter.Open("127.0.0.1", test_port, wrong_pub, malicious_rig_id), "Transmitter open");

    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 99;
    transmitter.SendPayload(payload);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    Wire::SatellitePayload out_payload{};
    bool res = receiver.ReceivePacket(out_payload);
    TEST_ASSERT(!res, "Mismatched key packet must fail decryption");

    transmitter.Close();
    receiver.Close();

    Logger::Flush();
    Logger::Shutdown();

    std::ifstream logFile(tempLog);
    std::string logContent((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    logFile.close();
    std::filesystem::remove(tempLog, ec);

    // Secure requirement: Unauthenticated header fields must sanitize CRLF control characters before logging.
    // Pre-patch failure: logContent contains injected newline with "INJECT".
    // Post-patch pass: control characters are stripped/escaped, preventing CRLF log injection.
    bool has_injected_line = (logContent.find("\nINJECT") != std::string::npos ||
                              logContent.find("\r\nINJECT") != std::string::npos);

    TEST_ASSERT(!has_injected_line, "Unauthenticated rig_id header must not permit CRLF log injection");
    return true;
}

// BS-VULN-02: Working directory satellite discovery regression test
bool Test_ServiceInstaller_WorkingDirectorySatelliteDiscovery() {
    fs::path tempBase = fs::temp_directory_path() / "barestats_test_vuln02";
    std::error_code ec;
    fs::remove_all(tempBase, ec);
    fs::create_directories(tempBase / "bin", ec);
    fs::create_directories(tempBase / "work", ec);

    // exe_dir has no satellite.ini
    fs::path exe_dir = tempBase / "bin";
    // work dir has satellite.ini
    fs::path work_dir = tempBase / "work";
    std::ofstream ofs(work_dir / "satellite.ini");
    ofs << "[Satellite]\nrig_id = test_node\n";
    ofs.close();

    fs::path orig_cwd = fs::current_path(ec);
    fs::current_path(work_dir, ec);

    AppMode mode = EvaluateAppMode(exe_dir);

    // Restore cwd
    fs::current_path(orig_cwd, ec);
    fs::remove_all(tempBase, ec);

    TEST_ASSERT(mode == AppMode::HeadlessSatellite,
                "EvaluateAppMode must recognize satellite.ini in current working directory");
    return true;
}

bool Test_Cluster_CaseInsensitiveRigAndRemoteGpuCompleteness() {
    SystemMetrics sys{};
    AppConfig cfg{};
    cfg.separator = L"|";

    RigNodeSnapshot rig{};
    std::strncpy(rig.rig_id, "WorkstationA", sizeof(rig.rig_id) - 1);
    rig.is_online = true;
    rig.cpu_load_pct = 55;
    rig.ram_used_mb = 16384;
    rig.ram_total_mb = 32768;
    rig.gpu_count = 2;

    // GPU 0
    rig.gpus[0].utilization_pct = 45;
    rig.gpus[0].vram_used_mb = 8192;
    rig.gpus[0].vram_total_mb = 16384;
    rig.gpus[0].temp_c = 65;
    rig.gpus[0].power_w = 150;
    rig.gpus[0].fan_rpm = 1800;

    // GPU 1
    rig.gpus[1].utilization_pct = 90;
    rig.gpus[1].vram_used_mb = 12288;
    rig.gpus[1].vram_total_mb = 24576;
    rig.gpus[1].temp_c = 72;
    rig.gpus[1].power_w = 280;
    rig.gpus[1].fan_rpm = 2200;

    // LLM model on WorkstationA
    rig.snapshot.is_online = true;
    rig.snapshot.llm_count = 1;
    std::strncpy(rig.snapshot.llms[0].model_name, "llama3:70b", sizeof(rig.snapshot.llms[0].model_name) - 1);
    rig.snapshot.llms[0].state = Wire::LlmState::Generating;
    rig.snapshot.llms[0].tps = 24.5f;

    sys.cluster.rig_count = 1;
    sys.cluster.rigs[0] = rig;

    // 1. Verify case-insensitive ResolveRigModel
    const Wire::LlmMetrics* mLower = ResolveRigModel(sys.cluster, "workstationa", -1);
    TEST_ASSERT(mLower != nullptr, "ResolveRigModel with lowercase target must match WorkstationA");
    TEST_ASSERT(std::string(mLower->model_name).find("llama3") != std::string::npos, "Model name must match");

    const Wire::LlmMetrics* mUpper = ResolveRigModel(sys.cluster, "WORKSTATIONA", -1);
    TEST_ASSERT(mUpper != nullptr, "ResolveRigModel with UPPERCASE target must match WorkstationA");

    // 2. Verify case-insensitive FormatGridCellText
    GridToken gt;
    gt.rig_target = "workstationa";
    gt.model_index = -1;
    gt.metric = LlmMetricType::ModelName;
    ModelNameCondenser condenser;
    std::string modelCell = FormatGridCellText(gt, sys.cluster, condenser);
    TEST_ASSERT(modelCell.find("llama3") != std::string::npos, "FormatGridCellText with lowercase target must match WorkstationA model");

    // 3. Verify case-insensitive Formatter::FormatCell
    auto cpuSegs = Formatter::FormatCell("workstationa:cpu", sys, &cfg);
    TEST_ASSERT(!cpuSegs.empty(), "FormatCell(workstationa:cpu) must match WorkstationA");
    bool hasCpuUtil = false;
    for (const auto& s : cpuSegs) {
        if (s.text.find(L"55") != std::wstring::npos) hasCpuUtil = true;
    }
    TEST_ASSERT(hasCpuUtil, "FormatCell(workstationa:cpu) must display 55% utilization");

    auto ramSegs = Formatter::FormatCell("WORKSTATIONA:ram", sys, &cfg);
    TEST_ASSERT(!ramSegs.empty(), "FormatCell(WORKSTATIONA:ram) must match WorkstationA");

    // 4. Verify Multi-GPU Remote Satellite Formatting (generic "workstationa:gpu" iterates across all GPUs)
    auto multiGpuSegs = Formatter::FormatCell("workstationa:gpu", sys, &cfg);
    TEST_ASSERT(!multiGpuSegs.empty(), "FormatCell(workstationa:gpu) must return combined multi-gpu segments");

    std::wstring combinedGpuText;
    for (const auto& s : multiGpuSegs) {
        combinedGpuText += s.text;
    }
    // Must contain GPU0 and GPU1 indicators
    TEST_ASSERT(combinedGpuText.find(L"GPU0") != std::wstring::npos, "Must contain GPU0");
    TEST_ASSERT(combinedGpuText.find(L"GPU1") != std::wstring::npos, "Must contain GPU1");
    TEST_ASSERT(combinedGpuText.find(L"|") != std::wstring::npos, "Must contain separator between GPUs");

    // Must include util, temp (°C), power (W), fan (RPM)
    TEST_ASSERT(combinedGpuText.find(L"45%") != std::wstring::npos, "Must include GPU0 util 45%");
    TEST_ASSERT(combinedGpuText.find(L"90%") != std::wstring::npos, "Must include GPU1 util 90%");
    TEST_ASSERT(combinedGpuText.find(L"65") != std::wstring::npos, "Must include GPU0 temp 65");
    TEST_ASSERT(combinedGpuText.find(L"72") != std::wstring::npos, "Must include GPU1 temp 72");
    TEST_ASSERT(combinedGpuText.find(L"150W") != std::wstring::npos, "Must include GPU0 power 150W");
    TEST_ASSERT(combinedGpuText.find(L"280W") != std::wstring::npos, "Must include GPU1 power 280W");
    TEST_ASSERT(combinedGpuText.find(L"1800RPM") != std::wstring::npos, "Must include GPU0 fan 1800RPM");
    TEST_ASSERT(combinedGpuText.find(L"2200RPM") != std::wstring::npos, "Must include GPU1 fan 2200RPM");

    // 5. Verify Sub-Metrics
    auto tempSegs = Formatter::FormatCell("workstationa:gpu0_temp", sys, &cfg);
    TEST_ASSERT(!tempSegs.empty(), "FormatCell(workstationa:gpu0_temp) must return segments");
    std::wstring tempText;
    for (const auto& s : tempSegs) tempText += s.text;
    TEST_ASSERT(tempText.find(L"65") != std::wstring::npos, "gpu0_temp must show 65");

    auto powerSegs = Formatter::FormatCell("workstationa:gpu0_power", sys, &cfg);
    TEST_ASSERT(!powerSegs.empty(), "FormatCell(workstationa:gpu0_power) must return segments");
    std::wstring pwrText;
    for (const auto& s : powerSegs) pwrText += s.text;
    TEST_ASSERT(pwrText.find(L"150W") != std::wstring::npos, "gpu0_power must show 150W");

    auto fanSegs = Formatter::FormatCell("workstationa:gpu0_fan", sys, &cfg);
    TEST_ASSERT(!fanSegs.empty(), "FormatCell(workstationa:gpu0_fan) must return segments");
    std::wstring fanText;
    for (const auto& s : fanSegs) fanText += s.text;
    TEST_ASSERT(fanText.find(L"1800RPM") != std::wstring::npos, "gpu0_fan must show 1800RPM");

    // 6. Verify Sub-Metrics Unavailable / Sensor 0 fallback renders "N/A"
    sys.cluster.rigs[0].gpus[0].temp_c = 0;
    sys.cluster.rigs[0].gpus[0].power_w = 0;
    sys.cluster.rigs[0].gpus[0].fan_rpm = 0;

    auto naTempSegs = Formatter::FormatCell("workstationa:gpu0_temp", sys, &cfg);
    TEST_ASSERT(!naTempSegs.empty(), "FormatCell(gpu0_temp) must not be empty on zero temp");
    std::wstring naTempText;
    for (const auto& s : naTempSegs) naTempText += s.text;
    TEST_ASSERT(naTempText.find(L"N/A") != std::wstring::npos, "gpu0_temp must render N/A when temp is 0");

    auto naPwrSegs = Formatter::FormatCell("workstationa:gpu0_power", sys, &cfg);
    TEST_ASSERT(!naPwrSegs.empty(), "FormatCell(gpu0_power) must not be empty on zero power");
    std::wstring naPwrText;
    for (const auto& s : naPwrSegs) naPwrText += s.text;
    TEST_ASSERT(naPwrText.find(L"N/A") != std::wstring::npos, "gpu0_power must render N/A when power is 0");

    auto naFanSegs = Formatter::FormatCell("workstationa:gpu0_fan", sys, &cfg);
    TEST_ASSERT(!naFanSegs.empty(), "FormatCell(gpu0_fan) must not be empty on zero fan");
    std::wstring naFanText;
    for (const auto& s : naFanSegs) naFanText += s.text;
    TEST_ASSERT(naFanText.find(L"N/A") != std::wstring::npos, "gpu0_fan must render N/A when fan is 0");

    // 6. Verify TelemetryEngine case-insensitive feed dedup and cluster updates
    TelemetryEngine testEngine;
    testEngine.RecordUdpFeedActivity("WorkstationA", std::chrono::milliseconds(5000));
    TEST_ASSERT(testEngine.IsUdpFeedActive("workstationa"), "IsUdpFeedActive must match lowercase");
    TEST_ASSERT(testEngine.IsUdpFeedActive("WORKSTATIONA"), "IsUdpFeedActive must match uppercase");

    LlmSnapshot snap1{};
    snap1.is_online = true;
    testEngine.UpdateRigInCluster("WorkstationA", snap1);
    LlmSnapshot snap2 = snap1;
    snap2.llm_count = 2;
    testEngine.UpdateRigInCluster("workstationa", snap2);
    SystemMetrics curM = testEngine.CollectImmediate();
    TEST_ASSERT(curM.cluster.rig_count == 1, "Case-insensitive UpdateRigInCluster must update existing rig and not duplicate entries");
    TEST_ASSERT(curM.cluster.rigs[0].snapshot.llm_count == 2, "Updated snapshot must take effect");

    // 7. Verify Generic "llm" in CustomGrid prepends rig label
    AppConfig gridCfg = cfg;
    gridCfg.layout = OverlayLayout::CustomGrid;
    auto customLlmSegs = Formatter::FormatCell("llm", sys, &gridCfg);
    TEST_ASSERT(!customLlmSegs.empty(), "FormatCell('llm') in CustomGrid must return segments");
    bool foundRigLabel = false;
    for (const auto& s : customLlmSegs) {
        if (s.text.find(L"WorkstationA") != std::wstring::npos) foundRigLabel = true;
    }
    TEST_ASSERT(foundRigLabel, "FormatCell('llm') in CustomGrid must resolve and prepend rig label");

    return true;
}

bool Test_TelemetryEngine_DynamicReceiverActivationAndFanPack() {
    AppConfig initCfg{};
    initCfg.cluster.clusterKey = ""; // Initially unconfigured
    initCfg.cluster.listenPort = 9987;

    TelemetryEngine engine(initCfg);
    engine.StartBackgroundWorkers(AppMode::CommandCenter);

    // Initial state: no key, listener not active
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Dynamic config reload with generated 64-char key
    uint8_t rawSk[Crypto::KEY_SIZE] = {0};
    Crypto::GenerateKey(rawSk);
    uint8_t rawPk[Crypto::KEY_SIZE] = {0};
    Crypto::DerivePublicKey(rawSk, rawPk);
    std::string keyHex = Crypto::KeyToHex(rawSk);
    Crypto::SecureWipe(rawSk, sizeof(rawSk));

    AppConfig reloadedCfg = initCfg;
    reloadedCfg.cluster.clusterKey = keyHex;
    engine.UpdateConfig(reloadedCfg);

    // Give UDP receiver loop time to bind and spin up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Spin up transmitter targeting the dynamically opened port
    std::vector<SatelliteTargetConfig> targets;
    SatelliteTargetConfig target;
    target.name = "center";
    target.target_host = "127.0.0.1";
    target.target_port = 9987;
    std::memcpy(target.public_key, rawPk, Crypto::KEY_SIZE);
    targets.push_back(target);

    UdpTransmitter transmitter;
    bool openOk = transmitter.Open("DynamicRig1", targets);
    TEST_ASSERT(openOk, "Transmitter must successfully open socket");

    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 77;
    payload.ram_used_mb = 4096;
    payload.ram_total_mb = 8192;
    payload.gpu_count = 1;
    payload.gpus[0].utilization_pct = 66;
    payload.gpus[0].vram_used_mb = 3000;
    payload.gpus[0].vram_total_mb = 8000;
    payload.gpus[0].temp_c = 58;
    payload.gpus[0].power_w = 125;
    payload.gpus[0].fan_rpm = 1950; // Fan RPM verification

    transmitter.SendPayload(payload);

    // Wait for packet reception in engine
    bool received = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SystemMetrics m = engine.GetLatestMetrics();
        if (m.cluster.rig_count > 0) {
            for (uint8_t r = 0; r < m.cluster.rig_count; ++r) {
                if (RigIdEquals(m.cluster.rigs[r].rig_id, "DynamicRig1")) {
                    TEST_ASSERT(m.cluster.rigs[r].cpu_load_pct == 77, "CPU load must match 77%");
                    TEST_ASSERT(m.cluster.rigs[r].gpus[0].fan_rpm == 1950, "Fan RPM must match 1950");
                    received = true;
                    break;
                }
            }
        }
        if (received) break;
    }

    transmitter.Close();
    engine.RequestShutdown();

    TEST_ASSERT(received, "Dynamic receiver must receive and authenticate packet on dynamically opened port");
    return true;
}


