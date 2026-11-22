#include "test_common.h"
#include "overlay/layout_engine.h"
#include "metrics/llm/llm_resolver.h"
#include "overlay/island_window_manager.h"
#include <cstring>

using namespace BareStats;

bool Test_LayoutEngine_ParseEndpoints() {
    std::string_view cfg = "local=http://127.0.0.1:11434, rig1=http://192.168.1.150:8000/, mac_studio=http://mac.local:8080 # comment";
    auto eps = ParseEndpoints(cfg);

    TEST_ASSERT(eps.size() == 3, "Must parse exactly 3 endpoints");

    TEST_ASSERT(eps[0].rig_id == "local", "Endpoint 0 rig must be local");
    TEST_ASSERT(eps[0].url == "http://127.0.0.1:11434", "Endpoint 0 url must preserve colon and port");

    TEST_ASSERT(eps[1].rig_id == "rig1", "Endpoint 1 rig must be rig1");
    TEST_ASSERT(eps[1].url == "http://192.168.1.150:8000", "Endpoint 1 trailing slash must be stripped");

    TEST_ASSERT(eps[2].rig_id == "mac_studio", "Endpoint 2 rig must be mac_studio");
    TEST_ASSERT(eps[2].url == "http://mac.local:8080", "Endpoint 2 comment must be stripped");

    // Test bare URL without '='
    auto eps_bare = ParseEndpoints("http://127.0.0.1:11434/");
    TEST_ASSERT(eps_bare.size() == 1, "Must parse 1 bare endpoint");
    TEST_ASSERT(eps_bare[0].rig_id == "local", "Bare endpoint defaults to local");
    TEST_ASSERT(eps_bare[0].url == "http://127.0.0.1:11434", "Bare endpoint url stripped");

    // Test splitting on semicolon ';' and comma ',' with trailing comments
    std::string_view cfg_semi = "local=http://127.0.0.1:11434; rig1=http://192.168.1.150:8000; mac=http://mac.local:8080 ; trailing comment";
    auto eps_semi = ParseEndpoints(cfg_semi);
    TEST_ASSERT(eps_semi.size() == 3, "Must parse 3 endpoints separated by semicolons and ignore trailing comments");
    TEST_ASSERT(eps_semi[0].rig_id == "local" && eps_semi[0].url == "http://127.0.0.1:11434", "Endpoint 0");
    TEST_ASSERT(eps_semi[1].rig_id == "rig1" && eps_semi[1].url == "http://192.168.1.150:8000", "Endpoint 1");
    TEST_ASSERT(eps_semi[2].rig_id == "mac" && eps_semi[2].url == "http://mac.local:8080", "Endpoint 2");

    return true;
}

bool Test_LayoutEngine_ParseGridToken() {
    // 1 part
    GridToken t = ParseGridToken("llm");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == -1 && t.metric == LlmMetricType::Composite, "Token llm");

    t = ParseGridToken("tps");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == -1 && t.metric == LlmMetricType::Tps, "Token tps");

    t = ParseGridToken("llm0");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == 0 && t.metric == LlmMetricType::Composite, "Token llm0");

    t = ParseGridToken("kv");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == -1 && t.metric == LlmMetricType::KvCache, "Token kv");

    t = ParseGridToken("vram");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == -1 && t.metric == LlmMetricType::Vram, "Token vram");

    t = ParseGridToken("queue");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == -1 && t.metric == LlmMetricType::Queue, "Token queue");

    // 2 parts
    t = ParseGridToken("rig1:tps");
    TEST_ASSERT(t.rig_target == "rig1" && t.model_index == -1 && t.metric == LlmMetricType::Tps, "Token rig1:tps");

    t = ParseGridToken("local:model");
    TEST_ASSERT(t.rig_target == "local" && t.model_index == -1 && t.metric == LlmMetricType::ModelName, "Token local:model");

    t = ParseGridToken("rig1:llm0");
    TEST_ASSERT(t.rig_target == "rig1" && t.model_index == 0 && t.metric == LlmMetricType::Composite, "Token rig1:llm0");

    t = ParseGridToken("llm0:tps");
    TEST_ASSERT(t.rig_target == "any" && t.model_index == 0 && t.metric == LlmMetricType::Tps, "Token llm0:tps");

    // 3 parts
    t = ParseGridToken("rig1:llm0:tps");
    TEST_ASSERT(t.rig_target == "rig1" && t.model_index == 0 && t.metric == LlmMetricType::Tps, "Token rig1:llm0:tps");

    t = ParseGridToken("mac:1:vram");
    TEST_ASSERT(t.rig_target == "mac" && t.model_index == 1 && t.metric == LlmMetricType::Vram, "Token mac:1:vram");

    return true;
}

bool Test_LlmResolver_PriorityCascade() {
    RigClusterSnapshot cluster{};
    cluster.rig_count = 2;

    // Rig 0 ("local"): 1 model, Idle, 4096 MB VRAM
    std::strncpy(cluster.rigs[0].rig_id, "local", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;
    cluster.rigs[0].snapshot.llm_count = 1;
    std::strncpy(cluster.rigs[0].snapshot.llms[0].model_name, "qwen2.5:7b", sizeof(cluster.rigs[0].snapshot.llms[0].model_name) - 1);
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Idle;
    cluster.rigs[0].snapshot.llms[0].vram_used_mb = 4096;
    cluster.rigs[0].snapshot.llms[0].tps = 0.0f;

    // Rig 1 ("rig1"): 1 model, Idle, 8192 MB VRAM
    std::strncpy(cluster.rigs[1].rig_id, "rig1", sizeof(cluster.rigs[1].rig_id) - 1);
    cluster.rigs[1].snapshot.is_online = true;
    cluster.rigs[1].snapshot.llm_count = 1;
    std::strncpy(cluster.rigs[1].snapshot.llms[0].model_name, "deepseek-r1:14b", sizeof(cluster.rigs[1].snapshot.llms[0].model_name) - 1);
    cluster.rigs[1].snapshot.llms[0].state = Wire::LlmState::Idle;
    cluster.rigs[1].snapshot.llms[0].vram_used_mb = 8192;
    cluster.rigs[1].snapshot.llms[0].tps = 0.0f;

    // Stage 1: Both Idle -> Priority 2 selects largest resident VRAM (rig1, 8192 MB)
    const Wire::LlmMetrics* resolved = ResolveGenericLlm(cluster);
    TEST_ASSERT(resolved != nullptr, "Must resolve model");
    TEST_ASSERT(std::string(resolved->model_name) == "deepseek-r1:14b", "Priority 2 must select largest VRAM model");

    // Stage 2: Rig 0 starts actively generating tokens at 35.5 TPS -> Priority 1 overrides largest VRAM
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Generating;
    cluster.rigs[0].snapshot.llms[0].tps = 35.5f;

    resolved = ResolveGenericLlm(cluster);
    TEST_ASSERT(resolved != nullptr, "Must resolve model");
    TEST_ASSERT(std::string(resolved->model_name) == "qwen2.5:7b", "Priority 1 generating must override largest VRAM");

    // Stage 3: Rig 0 finishes and enters BurstHold -> BurstHold retains Priority 1 over Idle
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::BurstHold;

    resolved = ResolveGenericLlm(cluster);
    TEST_ASSERT(resolved != nullptr, "Must resolve model");
    TEST_ASSERT(std::string(resolved->model_name) == "qwen2.5:7b", "BurstHold must retain active priority");

    // Stage 4: Entire cluster offline -> returns nullptr
    cluster.rigs[0].snapshot.is_online = false;
    cluster.rigs[1].snapshot.is_online = false;
    resolved = ResolveGenericLlm(cluster);
    TEST_ASSERT(resolved == nullptr, "Offline cluster must resolve to nullptr");

    return true;
}

bool Test_LlmResolver_FormatGridCellText() {
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    RigClusterSnapshot cluster{};
    cluster.rig_count = 1;

    std::strncpy(cluster.rigs[0].rig_id, "rig1", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;
    cluster.rigs[0].snapshot.llm_count = 1;

    auto& m = cluster.rigs[0].snapshot.llms[0];
    std::strncpy(m.model_name, "deepseek-ai/DeepSeek-R1-14B-GGUF", sizeof(m.model_name) - 1);
    m.state = Wire::LlmState::Generating;
    m.tps = 42.1f;
    m.kv_cache_pct = 32;
    m.vram_used_mb = 8192;
    m.queue_running = 2;
    m.queue_waiting = 1;

    // rig1:tps
    GridToken t = ParseGridToken("rig1:tps");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "42.1 T/s", "Format tps");

    // rig1:model
    t = ParseGridToken("rig1:model");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "DeepSeek-R1..14B", "Format model");

    // rig1:kv
    t = ParseGridToken("rig1:kv");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "KV 32%", "Format kv");

    // rig1:vram
    t = ParseGridToken("rig1:vram");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "8.0 GB", "Format vram");

    // rig1:queue
    t = ParseGridToken("rig1:queue");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "Q: 2/1", "Format queue");

    // Unknown/Offline rig
    t = ParseGridToken("nonexistent:tps");
    TEST_ASSERT(FormatGridCellText(t, cluster, condenser) == "Offline", "Format nonexistent rig");

    // Multi-model composite targeting with target_model (GAP-05)
    cluster.rigs[0].snapshot.llm_count = 2;
    std::strncpy(cluster.rigs[0].snapshot.llms[1].model_name, "qwen2.5-coder-7b", sizeof(cluster.rigs[0].snapshot.llms[1].model_name) - 1);
    cluster.rigs[0].snapshot.llms[1].state = Wire::LlmState::Idle;
    cluster.rigs[0].snapshot.llms[1].tps = 0.0f;

    GridToken t_comp = ParseGridToken("rig1:llm");
    std::string s_active = FormatGridCellText(t_comp, cluster, condenser, "active");
    TEST_ASSERT(s_active == "DeepSeek-R1..14B 42.1 T/s (+1)", "FormatGridCellText active with (+1)");

    std::string s_idx1 = FormatGridCellText(t_comp, cluster, condenser, "1");
    TEST_ASSERT(s_idx1 == "qwen2.5-coder-7b Idle", "FormatGridCellText index 1");

    std::string s_all = FormatGridCellText(t_comp, cluster, condenser, "all");
    TEST_ASSERT(s_all == "DeepSeek-R1..14B 42.1 T/s, qwen2.5-coder-7b Idle", "FormatGridCellText all");

    return true;
}

bool Test_IslandWindowManager_Visibility() {
    IslandWindowManager mgr;

    // Calling with null handle must be safely handled without throwing or crashing
    TEST_ASSERT(!mgr.IsWindowVisibleOS(nullptr), "Null handle must not be visible");

    mgr.UpdateIslandVisibility(nullptr, true);
    mgr.UpdateIslandVisibility(nullptr, false);
    mgr.ShowSafely(nullptr);
    mgr.Hide(nullptr);

    return true;
}
