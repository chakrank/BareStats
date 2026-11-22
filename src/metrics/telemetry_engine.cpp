#include "telemetry_engine.h"
#include "util/crypto/crypto_box.h"
#include "util/os_utils.h"
#include "config/ini_parser.h"
#include "util/logger.h"
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace BareStats {

uint8_t TelemetryEngine::ComputeMaxGpuUtilization(const SystemMetrics& sample) {
    uint8_t maxPct = 0;
    for (const auto& g : sample.gpus) {
        uint8_t u = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(g.utilizationPercent.value_or(0.0))), 0, 100));
        if (u > maxPct) {
            maxPct = u;
        }
    }
    return maxPct;
}

TelemetryEngine::TelemetryEngine()
    : config_() {
    RebuildLlmCollectors();
}

TelemetryEngine::TelemetryEngine(const AppConfig& config)
    : config_(config) {
    RebuildLlmCollectors();
}

TelemetryEngine::~TelemetryEngine() {
    Stop();
}

void TelemetryEngine::RebuildLlmCollectors() {
    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        cfg = config_;
    }

    std::vector<std::unique_ptr<LlmCollector>> newCollectors;
    if (cfg.llm.enabled) {
        auto endpoints = ParseEndpoints(cfg.llm.endpoints);
        for (const auto& ep : endpoints) {
            auto collector = std::make_unique<LlmCollector>(ep.rig_id, ep.url, LlmAdapter::AutoDetect);
            collector->timeout_ms = cfg.llm.timeoutMs;
            collector->poll_interval_ms = cfg.llm.pollIntervalMs;
            collector->retry_interval_ms = cfg.llm.retryIntervalMs;
            collector->decay_hold_sec = cfg.llm.decayHoldSec;
            collector->filter_embeddings = cfg.llm.filterEmbeddings;
            collector->min_vram_mb = cfg.llm.minVramMb;
            newCollectors.push_back(std::move(collector));
        }
    }

    {
        std::lock_guard<std::mutex> lock(llmCollectorsMutex_);
        llmCollectors_ = std::move(newCollectors);
    }
}

size_t TelemetryEngine::GetLlmCollectorCount() const {
    std::lock_guard<std::mutex> lock(llmCollectorsMutex_);
    return llmCollectors_.size();
}

void TelemetryEngine::RecordUdpFeedActivity(const std::string& rigId, std::chrono::milliseconds timeout) {
    std::string lower = rigId;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> lock(feedDedupMutex_);
    activeUdpFeeds_[lower] = std::chrono::steady_clock::now() + timeout;
}

bool TelemetryEngine::IsUdpFeedActive(const std::string& rigId) const {
    std::string lower = rigId;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> lock(feedDedupMutex_);
    auto it = activeUdpFeeds_.find(lower);
    if (it != activeUdpFeeds_.end() && it->second > std::chrono::steady_clock::now()) {
        return true;
    }
    return false;
}

bool TelemetryEngine::CheckUdpFeedTimeouts() {
    auto now = std::chrono::steady_clock::now();
    bool clusterChanged = false;

    std::lock_guard<std::mutex> clusterLock(clusterUpdateMutex_);
    RigClusterSnapshot snap = clusterSeqlock_.ReadSnapshot();

    {
        std::lock_guard<std::mutex> lock(feedDedupMutex_);
        for (uint8_t r = 0; r < snap.rig_count; ++r) {
            std::string rId(snap.rigs[r].rig_id, strnlen(snap.rigs[r].rig_id, sizeof(snap.rigs[r].rig_id)));
            for (char& c : rId) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            auto it = activeUdpFeeds_.find(rId);
            if (it != activeUdpFeeds_.end()) {
                if (now >= it->second) {
                    if (snap.rigs[r].is_online || snap.rigs[r].snapshot.is_online || snap.rigs[r].cpu_load_pct != 0 || snap.rigs[r].ram_total_mb != 0) {
                        snap.rigs[r].is_online = false;
                        snap.rigs[r].snapshot.is_online = false;
                        snap.rigs[r].snapshot.llm_count = 0;
                        snap.rigs[r].cpu_load_pct = 0;
                        snap.rigs[r].ram_used_mb = 0;
                        snap.rigs[r].ram_total_mb = 0;
                        snap.rigs[r].gpu_count = 0;
                        for (size_t g = 0; g < Wire::WIRE_MAX_GPUS; ++g) {
                            snap.rigs[r].gpus[g] = {};
                        }
                        clusterChanged = true;
                        LOG_INFO("CLUSTER", "Remote satellite node timed out (offline): " + rId);
                    }
                }
            }
        }
    }

    if (clusterChanged) {
        clusterSeqlock_.WriteSnapshot(snap);
        std::lock_guard<std::mutex> lock(metricsMutex_);
        latestMetrics_.cluster = snap;
    }
    return clusterChanged;
}

void TelemetryEngine::UpdateRigInCluster(const std::string& rigId, const LlmSnapshot& snapshot) {
    try {
        std::lock_guard<std::mutex> clusterLock(clusterUpdateMutex_);
        RigClusterSnapshot updated = clusterSeqlock_.ReadSnapshot();
        updated.rig_count = std::min(updated.rig_count, static_cast<uint8_t>(8));
        int foundIdx = -1;
        for (uint8_t r = 0; r < updated.rig_count; ++r) {
            if (RigIdEquals(updated.rigs[r].rig_id, rigId)) {
                foundIdx = r;
                break;
            }
        }
        if (foundIdx == -1 && updated.rig_count < 8) {
            foundIdx = updated.rig_count++;
            size_t copyLen = std::min(sizeof(updated.rigs[foundIdx].rig_id) - 1, rigId.size());
            std::memcpy(updated.rigs[foundIdx].rig_id, rigId.c_str(), copyLen);
            updated.rigs[foundIdx].rig_id[copyLen] = '\0';
        }
        if (foundIdx >= 0 && foundIdx < 8) {
            updated.rigs[foundIdx].snapshot = snapshot;
            updated.rigs[foundIdx].snapshot.llm_count = std::min(snapshot.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
        }
        clusterSeqlock_.WriteSnapshot(updated);
        {
            std::lock_guard<std::mutex> lock(metricsMutex_);
            latestMetrics_.cluster = updated;
        }
    } catch (...) {}
}

SystemMetrics TelemetryEngine::CollectImmediate() {
    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        cfg = config_;
    }

    SystemMetrics metrics;
    bool collectCpu = cfg.showCpu || cfg.IsMetricInActiveLayout("cpu");
    bool collectRam = cfg.showRam || cfg.IsMetricInActiveLayout("ram");
    bool collectGpu = cfg.showGpu || cfg.IsMetricInActiveLayout("gpu");
    bool collectNet = cfg.showNet || cfg.IsMetricInActiveLayout("net");

    if (collectCpu) metrics.cpu = cpuCollector_.Collect();
    if (collectRam) metrics.memory = memoryCollector_.Collect();
    if (collectGpu) {
        metrics.gpus = gpuCollector_.CollectMultiple(cfg.targetGpuIndices, cfg.targetAllGpus);
        if (!metrics.gpus.empty()) {
            metrics.gpu = metrics.gpus[0];
        }
    }
    if (collectNet) metrics.network = netCollector_.Collect();
    metrics.cluster = clusterSeqlock_.ReadSnapshot();
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        latestMetrics_ = metrics;
    }
    return metrics;
}

void TelemetryEngine::Start(std::function<void()> onUpdateCallback) {
    if (isRunning_.load()) return;

    onUpdateCallback_ = std::move(onUpdateCallback);
    isRunning_.store(true);
    isSuspended_.store(false);

    workerThread_ = std::thread(&TelemetryEngine::WorkerLoop, this);
}

#ifdef _WIN32
void TelemetryEngine::Start(HWND notifyHwnd, UINT notifyMsg) {
    if (isRunning_.load()) return;

    notifyHwnd_ = notifyHwnd;
    notifyMsg_ = notifyMsg;
    isRunning_.store(true);
    isSuspended_.store(false);

    workerThread_ = std::thread(&TelemetryEngine::WorkerLoop, this);
}
#endif

void TelemetryEngine::InitializeModeWorkers() {
    RebuildLlmCollectors();

    if (appMode_ == AppMode::HeadlessSatellite || appMode_ == AppMode::Hybrid) {
        if (appMode_ == AppMode::HeadlessSatellite) {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_.targetAllGpus = true;
            config_.gpuIndex = 0;
            config_.targetGpuIndices.clear();
        }
        IniDocument satDoc;
        std::filesystem::path satPath = GetExecutableDirectory() / "satellite.ini";
        if (!std::filesystem::exists(satPath)) {
            if (std::filesystem::exists("satellite.ini")) {
                satPath = "satellite.ini";
            } else {
                std::filesystem::path cfgSat = std::filesystem::path(ConfigManager::GetDefaultConfigPath()).parent_path() / "satellite.ini";
                if (std::filesystem::exists(cfgSat)) {
                    satPath = cfgSat;
                } else if (std::filesystem::exists("/etc/barestats/satellite.ini")) {
                    satPath = "/etc/barestats/satellite.ini";
                }
            }
        }
        if (satDoc.Load(satPath.string())) {
            std::string rigId = satDoc.GetString("Satellite", "rig_id", "rig1");
            transmitterRigId_ = rigId;

            if (appMode_ == AppMode::HeadlessSatellite || satDoc.HasKey("Satellite", "gpu_index")) {
                std::string defaultGpu = (appMode_ == AppMode::HeadlessSatellite) ? "all" : "0";
                std::string gpuTargetStr = satDoc.GetString("Satellite", "gpu_index", defaultGpu);
                std::string lowerTarget = gpuTargetStr;
                for (char& c : lowerTarget) c = static_cast<char>(tolower(c));

                std::lock_guard<std::mutex> lock(configMutex_);
                config_.gpuTarget = gpuTargetStr;
                config_.targetGpuIndices.clear();
                if (lowerTarget == "all") {
                    config_.targetAllGpus = true;
                    config_.gpuIndex = 0;
                } else {
                    config_.targetAllGpus = false;
                    std::stringstream ssGpu(gpuTargetStr);
                    std::string token;
                    while (std::getline(ssGpu, token, ',')) {
                        size_t tFirst = token.find_first_not_of(" \t\r\n");
                        size_t tLast = token.find_last_not_of(" \t\r\n");
                        if (tFirst != std::string::npos && tLast != std::string::npos) {
                            try {
                                int idx = std::stoi(token.substr(tFirst, tLast - tFirst + 1));
                                if (idx >= 0) {
                                    config_.targetGpuIndices.push_back(static_cast<uint32_t>(idx));
                                }
                            } catch (...) {}
                        }
                    }
                    if (config_.targetGpuIndices.empty()) {
                        config_.targetGpuIndices.push_back(0);
                    }
                    config_.gpuIndex = static_cast<int>(config_.targetGpuIndices[0]);
                }
            }

            std::vector<SatelliteTargetConfig> targets;

            // 1. Scan for multi-target sections: [Target.<name>]
            std::vector<std::string> sections = satDoc.GetSections();
            for (const auto& sec : sections) {
                std::string lowerSec = sec;
                for (char& c : lowerSec) c = static_cast<char>(tolower(c));
                if (lowerSec.rfind("target.", 0) == 0 && sec.size() > 7) {
                    std::string targetName = sec.substr(7);
                    std::string host = satDoc.GetString(sec, "target_host", "auto");
                    if (host == "auto") host = "255.255.255.255";
                    uint16_t port = static_cast<uint16_t>(satDoc.GetInt(sec, "target_port", 9889));

                    std::string pubKeyHex = satDoc.GetString(sec, "public_key", "");
                    uint8_t pkBytes[Crypto::KEY_SIZE] = {0};
                    bool keyOk = false;
                    if (!pubKeyHex.empty()) {
                        keyOk = Crypto::HexToKey(pubKeyHex, pkBytes);
                    } else {
                        // Fallback: check cluster_key (legacy secret key from which public key is derived)
                        std::string privKeyHex = satDoc.GetString(sec, "cluster_key", "");
                        if (!privKeyHex.empty()) {
                            uint8_t skBytes[Crypto::KEY_SIZE] = {0};
                            if (Crypto::HexToKey(privKeyHex, skBytes)) {
                                Crypto::DerivePublicKey(skBytes, pkBytes);
                                Crypto::SecureWipe(skBytes, sizeof(skBytes));
                                keyOk = true;
                            }
                        }
                    }

                    if (keyOk) {
                        SatelliteTargetConfig tc;
                        tc.name = targetName;
                        tc.target_host = host;
                        tc.target_port = port;
                        std::memcpy(tc.public_key, pkBytes, Crypto::KEY_SIZE);
                        targets.push_back(tc);
                    } else {
                        LOG_ERROR("CLUSTER", "Invalid key in [" + sec + "]: must be 64 hex characters");
                    }
                }
            }

            // 2. Fallback to default [Satellite] block if no [Target.*] section was configured
            if (targets.empty()) {
                std::string targetHost = satDoc.GetString("Satellite", "target_host", "auto");
                if (targetHost == "auto") targetHost = "255.255.255.255";
                uint16_t targetPort = static_cast<uint16_t>(satDoc.GetInt("Satellite", "target_port", 9889));

                std::string pubKeyHex = satDoc.GetString("Satellite", "public_key", "");
                uint8_t pkBytes[Crypto::KEY_SIZE] = {0};
                bool keyOk = false;
                if (!pubKeyHex.empty()) {
                    keyOk = Crypto::HexToKey(pubKeyHex, pkBytes);
                } else {
                    std::string privKeyHex = satDoc.GetString("Satellite", "cluster_key", "");
                    if (!privKeyHex.empty()) {
                        uint8_t skBytes[Crypto::KEY_SIZE] = {0};
                        if (Crypto::HexToKey(privKeyHex, skBytes)) {
                            Crypto::DerivePublicKey(skBytes, pkBytes);
                            Crypto::SecureWipe(skBytes, sizeof(skBytes));
                            keyOk = true;
                        }
                    }
                }

                if (keyOk) {
                    SatelliteTargetConfig tc;
                    tc.name = "default";
                    tc.target_host = targetHost;
                    tc.target_port = targetPort;
                    std::memcpy(tc.public_key, pkBytes, Crypto::KEY_SIZE);
                    targets.push_back(tc);
                } else {
                    LOG_ERROR("CLUSTER", "Invalid public_key/cluster_key in satellite.ini: must be 64 hex characters");
                }
            }

            if (!targets.empty()) {
                bool ok = transmitter_.Open(rigId, targets);
                transmitterActive_.store(ok);
                if (ok) {
                    LOG_INFO("CLUSTER", "Satellite transmitter active: rig_id=" + rigId + ", " + std::to_string(targets.size()) + " target(s) configured");
                } else {
                    LOG_ERROR("CLUSTER", "Failed to bind transmitter socket for rig_id=" + rigId);
                }
            }

            std::string satLlmUrl = satDoc.GetString("Satellite", "llm_endpoint", satDoc.GetString("LLM", "endpoint", ""));
            if (!satLlmUrl.empty()) {
                auto satCollector = std::make_unique<LlmCollector>(rigId, satLlmUrl, LlmAdapter::AutoDetect);
                satCollector->timeout_ms = 300;
                satCollector->poll_interval_ms = 1000;
                satCollector->retry_interval_ms = 10000;
                satCollector->decay_hold_sec = 3;
                satCollector->filter_embeddings = true;
                satCollector->min_vram_mb = 500;
                std::lock_guard<std::mutex> lock(llmCollectorsMutex_);
                llmCollectors_.push_back(std::move(satCollector));
            }
        } else {
            LOG_ERROR("CLUSTER", "Failed to load satellite.ini from " + satPath.string());
        }
    }

    if (appMode_ == AppMode::CommandCenter || appMode_ == AppMode::Hybrid) {
        AppConfig cfg;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            cfg = config_;
        }
        if (!cfg.cluster.clusterKey.empty() && cfg.cluster.clusterKey.size() == 64) {
            uint8_t keyBytes[Crypto::KEY_SIZE] = {0};
            if (Crypto::HexToKey(cfg.cluster.clusterKey, keyBytes)) {
                bool bound = receiver_.Bind(cfg.cluster.listenPort, keyBytes);
                Crypto::SecureWipe(keyBytes, sizeof(keyBytes));
                if (!cfg.cluster.allowedRigs.empty()) {
                    std::vector<std::string> allowed;
                    std::stringstream ss(cfg.cluster.allowedRigs);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
                        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
                        if (!item.empty()) allowed.push_back(item);
                    }
                    receiver_.SetAllowedRigs(allowed);
                }
                if (transmitterActive_.load()) {
                    receiver_.SetIgnoredSessionId(transmitter_.GetSessionId());
                }
                receiverActive_.store(bound);
                if (bound) {
                    LOG_INFO("CLUSTER", "Command Center UDP listener active on port " + std::to_string(cfg.cluster.listenPort));
                    if (!udpRxThread_.joinable()) {
                        udpRxThread_ = std::thread(&TelemetryEngine::UdpRxLoop, this);
                    }
                } else {
                    LOG_ERROR("CLUSTER", "Failed to bind Command Center UDP listener to port " + std::to_string(cfg.cluster.listenPort));
                }
            } else {
                LOG_ERROR("CLUSTER", "Invalid cluster_key in config.ini: must be 64 hex characters");
            }
        }
    }
}

void TelemetryEngine::StartBackgroundWorkers(AppMode mode) {
    StartBackgroundWorkers(mode, std::function<void()>(nullptr));
}

void TelemetryEngine::StartBackgroundWorkers(AppMode mode, std::function<void()> onUpdateCallback) {
    appMode_ = mode;
    InitializeModeWorkers();
    Start(std::move(onUpdateCallback));
}

#ifdef _WIN32
void TelemetryEngine::StartBackgroundWorkers(AppMode mode, HWND notifyHwnd, UINT notifyMsg) {
    appMode_ = mode;
    InitializeModeWorkers();
    Start(notifyHwnd, notifyMsg);
}
#endif

void TelemetryEngine::Stop() {
    if (!isRunning_.load()) return;

    isRunning_.store(false);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    receiverActive_.store(false);
    receiver_.Close();
    if (udpRxThread_.joinable()) {
        udpRxThread_.join();
    }
    transmitterActive_.store(false);
    transmitter_.Close();
}

void TelemetryEngine::SetSuspended(bool suspended) {
    if (isSuspended_.load() == suspended) return;

    isSuspended_.store(suspended);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        isSuspendedChanged_ = true;
        cv_.notify_all();
    }
}

SystemMetrics TelemetryEngine::GetLatestMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    return latestMetrics_;
}

void TelemetryEngine::Recover() {
    gpuCollector_.Recover();
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        isSuspendedChanged_ = true;
        cv_.notify_all();
    }
}

void TelemetryEngine::Shutdown() {
    Stop();
    gpuCollector_.Shutdown();
    transmitter_.Close();
    receiver_.Close();
}

void TelemetryEngine::Wake() {
    std::lock_guard<std::mutex> lock(cvMutex_);
    cv_.notify_all();
}

void TelemetryEngine::UpdateConfig(const AppConfig& config) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
    }
    if (appMode_ == AppMode::CommandCenter || appMode_ == AppMode::Hybrid) {
        if (!config.cluster.clusterKey.empty() && config.cluster.clusterKey.size() == 64 && !receiverActive_.load()) {
            uint8_t keyBytes[Crypto::KEY_SIZE] = {0};
            if (Crypto::HexToKey(config.cluster.clusterKey, keyBytes)) {
                bool bound = receiver_.Bind(config.cluster.listenPort, keyBytes);
                Crypto::SecureWipe(keyBytes, sizeof(keyBytes));
                if (!config.cluster.allowedRigs.empty()) {
                    std::vector<std::string> allowed;
                    std::stringstream ss(config.cluster.allowedRigs);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
                        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
                        if (!item.empty()) allowed.push_back(item);
                    }
                    receiver_.SetAllowedRigs(allowed);
                }
                if (transmitterActive_.load()) {
                    receiver_.SetIgnoredSessionId(transmitter_.GetSessionId());
                }
                receiverActive_.store(bound);
                if (bound) {
                    LOG_INFO("CLUSTER", "Command Center UDP listener active on port " + std::to_string(config.cluster.listenPort));
                    if (!udpRxThread_.joinable()) {
                        udpRxThread_ = std::thread(&TelemetryEngine::UdpRxLoop, this);
                    }
                } else {
                    LOG_ERROR("CLUSTER", "Failed to bind Command Center UDP listener to port " + std::to_string(config.cluster.listenPort));
                }
            } else {
                LOG_ERROR("CLUSTER", "Invalid cluster_key in config.ini: must be 64 hex characters");
            }
        }
    }
    if (receiverActive_.load()) {
        std::vector<std::string> allowed;
        if (!config.cluster.allowedRigs.empty()) {
            std::stringstream ss(config.cluster.allowedRigs);
            std::string item;
            while (std::getline(ss, item, ',')) {
                while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
                while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
                if (!item.empty()) allowed.push_back(item);
            }
        }
        receiver_.SetAllowedRigs(allowed);
    }
    RebuildLlmCollectors();
    Wake();
}

void TelemetryEngine::WorkerLoop() {
    // Initial warmup
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    while (isRunning_.load()) {
        if (!isSuspended_.load()) {
            try {
                AppConfig cfg;
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                cfg = config_;
            }

            SystemMetrics sample;
            bool collectCpu = cfg.showCpu || cfg.IsMetricInActiveLayout("cpu");
            bool collectRam = cfg.showRam || cfg.IsMetricInActiveLayout("ram");
            bool collectGpu = cfg.showGpu || cfg.IsMetricInActiveLayout("gpu");
            bool collectNet = cfg.showNet || cfg.IsMetricInActiveLayout("net");

            if (collectCpu) sample.cpu = cpuCollector_.Collect();
            if (collectRam) sample.memory = memoryCollector_.Collect();
            if (collectGpu) {
                sample.gpus = gpuCollector_.CollectMultiple(cfg.targetGpuIndices, cfg.targetAllGpus);
                if (!sample.gpus.empty()) {
                    sample.gpu = sample.gpus[0];
                }
            }
            if (collectNet) sample.network = netCollector_.Collect();

            if (appMode_ == AppMode::CommandCenter || appMode_ == AppMode::Hybrid) {
                CheckUdpFeedTimeouts();
            }

            sample.cluster = clusterSeqlock_.ReadSnapshot();

            {
                std::lock_guard<std::mutex> lock(metricsMutex_);
                latestMetrics_ = sample;
            }

            if (onUpdateCallback_) {
                try {
                    onUpdateCallback_();
                } catch (...) {}
            }

#ifdef _WIN32
            if (notifyHwnd_ && notifyMsg_) {
                PostMessageW(notifyHwnd_, notifyMsg_, 0, 0);
            }
#endif

            // Direct Pull LLM Scraping with Active UDP Feed Deduplication
            bool shouldPollLlm = (appMode_ == AppMode::CommandCenter || appMode_ == AppMode::Hybrid) && cfg.llm.enabled;
            if (appMode_ == AppMode::HeadlessSatellite) {
                std::lock_guard<std::mutex> lock(llmCollectorsMutex_);
                shouldPollLlm = !llmCollectors_.empty();
            }

            if (shouldPollLlm) {
                uint8_t gpuPct = ComputeMaxGpuUtilization(sample);
                std::vector<LlmCollector*> toPoll;
                {
                    std::lock_guard<std::mutex> lock(llmCollectorsMutex_);
                    for (auto& c : llmCollectors_) {
                        if (c) toPoll.push_back(c.get());
                    }
                }

                std::unordered_map<std::string, std::vector<LlmSnapshot>> groupedSnapshots;
                for (auto* c : toPoll) {
                    if (!c) continue;
                    // Suppress HTTP scraping if an active UDP satellite stream exists for this rig
                    if (IsUdpFeedActive(c->GetRigId())) {
                        continue;
                    }
                    c->Update(gpuPct);
                    groupedSnapshots[c->GetRigId()].push_back(c->GetSnapshot());
                }

                for (const auto& [rigId, snaps] : groupedSnapshots) {
                    LlmSnapshot merged{};
                    merged.is_online = false;
                    merged.llm_count = 0;

                    for (const auto& s : snaps) {
                        if (s.is_online) {
                            merged.is_online = true;
                            for (uint8_t m = 0; m < s.llm_count; ++m) {
                                bool duplicate = false;
                                auto modelMatches = [](const char* a, const char* b) -> bool {
                                    if (std::strncmp(a, b, sizeof(Wire::LlmMetrics::model_name)) == 0) return true;
                                    std::string_view s1(a, strnlen(a, sizeof(Wire::LlmMetrics::model_name)));
                                    std::string_view s2(b, strnlen(b, sizeof(Wire::LlmMetrics::model_name)));
                                    auto stripTag = [](std::string_view s) -> std::string_view {
                                        if (s.size() > 7 && s.substr(s.size() - 7) == ":latest") {
                                            return s.substr(0, s.size() - 7);
                                        }
                                        return s;
                                    };
                                    return stripTag(s1) == stripTag(s2);
                                };

                                auto stateRank = [](Wire::LlmState st) -> int {
                                    switch (st) {
                                        case Wire::LlmState::Generating: return 6;
                                        case Wire::LlmState::BurstHold:  return 5;
                                        case Wire::LlmState::Thinking:   return 4;
                                        case Wire::LlmState::Loading:    return 3;
                                        case Wire::LlmState::Compacting: return 2;
                                        case Wire::LlmState::Idle:       return 1;
                                        default:                         return 0;
                                    }
                                };

                                for (uint8_t d = 0; d < merged.llm_count; ++d) {
                                    if (modelMatches(merged.llms[d].model_name, s.llms[m].model_name)) {
                                        // Preserve VRAM and quantization attributes across endpoints
                                        merged.llms[d].vram_used_mb = std::max(merged.llms[d].vram_used_mb, s.llms[m].vram_used_mb);
                                        if (merged.llms[d].quant[0] == '\0' && s.llms[m].quant[0] != '\0') {
                                            std::strncpy(merged.llms[d].quant, s.llms[m].quant, sizeof(merged.llms[d].quant) - 1);
                                        }

                                        int incomingRank = stateRank(s.llms[m].state);
                                        int currentRank = stateRank(merged.llms[d].state);

                                        if (incomingRank > currentRank ||
                                            (s.llms[m].state == Wire::LlmState::Generating && s.llms[m].tps > merged.llms[d].tps)) {
                                            merged.llms[d].state = s.llms[m].state;
                                            merged.llms[d].tps = s.llms[m].tps;
                                            if (s.llms[m].kv_cache_pct > 0) merged.llms[d].kv_cache_pct = s.llms[m].kv_cache_pct;
                                            if (s.llms[m].queue_running > 0) merged.llms[d].queue_running = s.llms[m].queue_running;
                                            if (s.llms[m].queue_waiting > 0) merged.llms[d].queue_waiting = s.llms[m].queue_waiting;
                                        }
                                        duplicate = true;
                                        break;
                                    }
                                }
                                if (!duplicate && merged.llm_count < Wire::WIRE_MAX_MODELS) {
                                    merged.llms[merged.llm_count++] = s.llms[m];
                                }
                            }
                        }
                    }
                    UpdateRigInCluster(rigId, merged);
                }
            }

            if (transmitterActive_.load()) {
                Wire::SatellitePayload payload{};
                payload.cpu_load_pct = static_cast<uint8_t>(std::clamp(static_cast<int>(sample.cpu.utilizationPercent), 0, 100));
                payload.ram_used_mb = static_cast<uint32_t>(sample.memory.usedBytes / (1024 * 1024));
                payload.ram_total_mb = static_cast<uint32_t>(sample.memory.totalBytes / (1024 * 1024));

                uint8_t gCount = static_cast<uint8_t>(std::min(sample.gpus.size(), static_cast<size_t>(Wire::WIRE_MAX_GPUS)));
                payload.gpu_count = gCount;
                for (uint8_t g = 0; g < gCount; ++g) {
                    const auto& srcGpu = sample.gpus[g];
                    payload.gpus[g].vram_used_mb = static_cast<uint32_t>(srcGpu.memoryUsedBytes / (1024 * 1024));
                    payload.gpus[g].vram_total_mb = static_cast<uint32_t>(srcGpu.memoryTotalBytes / (1024 * 1024));
                    payload.gpus[g].utilization_pct = static_cast<uint8_t>(std::clamp(static_cast<int>(srcGpu.utilizationPercent.value_or(0.0)), 0, 100));
                    payload.gpus[g].temp_c = static_cast<int8_t>(std::clamp(static_cast<int>(srcGpu.temperatureC.value_or(0)), -128, 127));
                    payload.gpus[g].power_w = static_cast<uint16_t>(srcGpu.powerWatts.value_or(0));
                    payload.gpus[g].fan_rpm = static_cast<uint16_t>(srcGpu.fanRpm.value_or(srcGpu.fanSpeedPercent.value_or(0)));
                }

                // Match LLM models belonging to this transmitter's rig ID or the local scraper tag ("local")
                if (sample.cluster.rig_count > 0 && !transmitterRigId_.empty()) {
                    int matchIdx = -1;
                    // 1. Try explicit transmitterRigId_
                    for (uint8_t r = 0; r < sample.cluster.rig_count; ++r) {
                        if (sample.cluster.rigs[r].snapshot.is_online &&
                            RigIdEquals(sample.cluster.rigs[r].rig_id, transmitterRigId_)) {
                            matchIdx = r;
                            break;
                        }
                    }
                    // 2. Fall back to "local" (the default tag for resident HTTP scrapers on this machine)
                    if (matchIdx == -1) {
                        for (uint8_t r = 0; r < sample.cluster.rig_count; ++r) {
                            if (sample.cluster.rigs[r].snapshot.is_online &&
                                std::strncmp(sample.cluster.rigs[r].rig_id, "local", 5) == 0) {
                                matchIdx = r;
                                break;
                            }
                        }
                    }
                    if (matchIdx >= 0) {
                        const auto& matchNode = sample.cluster.rigs[matchIdx].snapshot;
                        if (matchNode.is_online) {
                            payload.sys_flags |= Wire::SYS_FLAG_LLM_ONLINE;
                        }
                        payload.llm_count = std::min(matchNode.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
                        for (uint8_t l = 0; l < payload.llm_count; ++l) {
                            payload.llms[l] = matchNode.llms[l];
                        }
                    }
                }

                transmitter_.SendPayload(payload);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("TELEMETRY", "Exception in worker sampling loop: " + std::string(e.what()));
        } catch (...) {
            LOG_ERROR("TELEMETRY", "Unknown non-standard exception in worker sampling loop");
        }
        }

        uint32_t interval = 1000;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            interval = (config_.refreshIntervalMs > 0) ? config_.refreshIntervalMs : 1000;
        }

        std::unique_lock<std::mutex> lock(cvMutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(interval), [this] {
            return !isRunning_.load() || (!isSuspended_.load() && isSuspendedChanged_);
        });
        isSuspendedChanged_ = false;
    }
}

void TelemetryEngine::UdpRxLoop() {
    while (isRunning_.load() && receiverActive_.load()) {
        try {
            Wire::SatellitePayload payload{};
            if (receiver_.ReceivePacket(payload)) {
                // Drop self-loopback packets if local transmitter is active (prevents Command Center adopting its own broadcast)
                if (transmitterActive_.load() && payload.session_id == transmitter_.GetSessionId()) {
                    continue;
                }

                std::string rigIdStr(payload.rig_id, strnlen(payload.rig_id, sizeof(payload.rig_id)));
                RecordUdpFeedActivity(rigIdStr);

                RigClusterSnapshot updated{};
                {
                    std::lock_guard<std::mutex> clusterLock(clusterUpdateMutex_);
                    updated = clusterSeqlock_.ReadSnapshot();
                    updated.rig_count = std::min(updated.rig_count, static_cast<uint8_t>(8));
                    int foundIdx = -1;
                    for (uint8_t r = 0; r < updated.rig_count; ++r) {
                        if (RigIdEquals(updated.rigs[r].rig_id, payload.rig_id)) {
                            foundIdx = r;
                            break;
                        }
                    }
                    if (foundIdx == -1) {
                        if (updated.rig_count < 8) {
                            foundIdx = updated.rig_count++;
                        } else {
                            int evictIdx = -1;
                            for (uint8_t r = 0; r < updated.rig_count; ++r) {
                                if (!updated.rigs[r].snapshot.is_online) {
                                    evictIdx = r;
                                    break;
                                }
                            }
                            if (evictIdx == -1) {
                                evictIdx = 0;
                            }
                            foundIdx = evictIdx;
                        }
                        if (foundIdx >= 0 && foundIdx < 8) {
                            size_t copyLen = std::min(sizeof(updated.rigs[foundIdx].rig_id) - 1, sizeof(payload.rig_id));
                            std::memcpy(updated.rigs[foundIdx].rig_id, payload.rig_id, copyLen);
                            updated.rigs[foundIdx].rig_id[copyLen] = '\0';
                            LOG_INFO("CLUSTER", "Discovered new remote satellite node: " + rigIdStr);
                        }
                    }
                    if (foundIdx >= 0 && foundIdx < 8) {
                        auto& rigNode = updated.rigs[foundIdx];
                        rigNode.is_online = true;
                        rigNode.cpu_load_pct = payload.cpu_load_pct;
                        rigNode.ram_used_mb = payload.ram_used_mb;
                        rigNode.ram_total_mb = payload.ram_total_mb;
                        rigNode.gpu_count = std::min(payload.gpu_count, static_cast<uint8_t>(8));
                        for (uint8_t g = 0; g < rigNode.gpu_count; ++g) {
                            rigNode.gpus[g] = payload.gpus[g];
                        }
                        auto& node = rigNode.snapshot;
                        node.is_online = ((payload.sys_flags & Wire::SYS_FLAG_LLM_ONLINE) != 0) || (payload.llm_count > 0);
                        node.llm_count = std::min(payload.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
                        for (uint8_t l = 0; l < node.llm_count; ++l) {
                            node.llms[l] = payload.llms[l];
                            node.llms[l].model_name[sizeof(node.llms[l].model_name) - 1] = '\0';
                            node.llms[l].quant[sizeof(node.llms[l].quant) - 1] = '\0';
                        }
                    }
                    clusterSeqlock_.WriteSnapshot(updated);
                }

                {
                    std::lock_guard<std::mutex> lock(metricsMutex_);
                    latestMetrics_.cluster = updated;
                }

                if (onUpdateCallback_) {
                    try {
                        onUpdateCallback_();
                    } catch (...) {}
                }
#ifdef _WIN32
                if (notifyHwnd_ && notifyMsg_) {
                    PostMessageW(notifyHwnd_, notifyMsg_, 0, 0);
                }
#endif
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("CLUSTER", "UdpRxLoop exception caught: " + std::string(e.what()));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } catch (...) {
            LOG_ERROR("CLUSTER", "UdpRxLoop unknown exception caught");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void TelemetryEngine::StopBackgroundWorkers() {
    Stop();
}

void TelemetryEngine::RunDaemonLoopForever() {
    daemonRunning_.store(true);
    std::unique_lock<std::mutex> lock(cvMutex_);
    cv_.wait(lock, [this] {
        return !isRunning_.load() || !daemonRunning_.load();
    });
}

void TelemetryEngine::RequestShutdown() {
    daemonRunning_.store(false);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
    Stop();
}

RigClusterSnapshot TelemetryEngine::GetClusterSnapshot() const {
    return clusterSeqlock_.ReadSnapshot();
}

void TelemetryEngine::UpdateClusterSnapshot(const RigClusterSnapshot& cluster) {
    std::lock_guard<std::mutex> clusterLock(clusterUpdateMutex_);
    clusterSeqlock_.WriteSnapshot(cluster);
    std::lock_guard<std::mutex> lock(metricsMutex_);
    latestMetrics_.cluster = cluster;
}

} // namespace BareStats
