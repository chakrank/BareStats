#include "test_common.h"
#include "metrics/llm/model_name_condenser.h"
#include "metrics/llm/llm_collector.h"

using namespace BareStats;

bool Test_ModelNameCondenser_TableTraces() {
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;
    condenser.user_aliases["Meta-Llama-3.1-70B-Instruct"] = "llama-70b";

    // Row 1: Alias override
    TEST_ASSERT(condenser.Condense("Meta-Llama-3.1-70B-Instruct") == "llama-70b",
                "Alias override must resolve to llama-70b");

    // Row 2: DeepSeek R1 Distill Qwen 32B
    TEST_ASSERT(condenser.Condense("deepseek-ai/DeepSeek-R1-Distill-Qwen-32B-GGUF") == "DeepSeek-R1..32B",
                "DeepSeek must resolve to DeepSeek-R1..32B");

    // Row 3a: Qwen 7B fitting exactly within 16 chars
    TEST_ASSERT(condenser.Condense("bartowski/Qwen2.5-Coder-7B-Instruct-GGUF") == "Qwen2.5-Coder-7B",
                "Qwen 7B fitting within 16 chars must resolve to Qwen2.5-Coder-7B");

    // Row 3b: Qwen 7B with version suffix exceeding 16 chars (triggers parameter smart truncation)
    TEST_ASSERT(condenser.Condense("bartowski/Qwen2.5-Coder-7B-Instruct-v0.1-GGUF") == "Qwen2.5-Code..7B",
                "Qwen 7B exceeding 16 chars must resolve to Qwen2.5-Code..7B");

    // Row 4: Llama 3 8B with abliterated-v2 suffix (test clamp preventing parameter duplication)
    TEST_ASSERT(condenser.Condense("Llama-3-8B-Instruct-abliterated-v2") == "Llama-3..8B",
                "Must prevent parameter duplication and resolve to Llama-3..8B");

    // Row 5: nomic embed text (no parameter badge, fallback truncation)
    TEST_ASSERT(condenser.Condense("nomic-ai/nomic-embed-text-v1.5") == "nomic-embed-t...",
                "Embed text must fallback truncate to nomic-embed-t...");

    // Row 6: phi-3 mini (already <= 16 chars)
    TEST_ASSERT(condenser.Condense("phi-3-mini-4k-instruct") == "phi-3-mini-4k",
                "Phi-3 mini must strip -instruct and fit within 16 chars");

    // Case-insensitive substring matching and longest-match alias preference (GAP-04)
    condenser.user_aliases["llama-3"] = "l3-base";
    condenser.user_aliases["llama-3.1-70b"] = "l3.1-70b-override";
    // Exact match fast path
    TEST_ASSERT(condenser.Condense("llama-3") == "l3-base", "Exact match alias");
    // Case-insensitive substring match
    TEST_ASSERT(condenser.Condense("meta-llama/LLAMA-3-8B-Instruct") == "l3-base",
                "Case-insensitive substring match");
    // Longest matching key preference
    TEST_ASSERT(condenser.Condense("meta-llama/LLAMA-3.1-70B-Instruct-GGUF") == "l3.1-70b-override",
                "Longest matching alias preference");

    return true;
}

bool Test_ModelNameCondenser_ParamBadges() {
    // 7B
    ParamMatch m = FindLastParamBadge("Qwen2.5-7B");
    TEST_ASSERT(m.length == 2 && m.start == 8, "Must match 7B");

    // 14b lowercase
    m = FindLastParamBadge("DeepSeek-14b");
    TEST_ASSERT(m.length == 3 && m.start == 9, "Must match 14b");

    // 8x7B MoE
    m = FindLastParamBadge("Mixtral-8x7B-v0.1");
    TEST_ASSERT(m.length == 4 && m.start == 8, "Must match 8x7B");

    // 0.5B decimal
    m = FindLastParamBadge("Qwen2.5-0.5B");
    TEST_ASSERT(m.length == 4 && m.start == 8, "Must match 0.5B");

    // 500M
    m = FindLastParamBadge("SmolLM-500M");
    TEST_ASSERT(m.length == 4 && m.start == 7, "Must match 500M");

    // Negative tests: false positives must be rejected
    m = FindLastParamBadge("web-server");
    TEST_ASSERT(m.length == 0, "web-server must not match badge");

    m = FindLastParamBadge("bert-base");
    TEST_ASSERT(m.length == 0, "bert-base must not match badge");

    m = FindLastParamBadge("vram-100MB");
    TEST_ASSERT(m.length == 0, "100MB memory size must not match parameter badge");

    return true;
}

bool Test_ModelNameCondenser_EmbeddingFilter() {
    // Embedding models must return true
    TEST_ASSERT(ModelNameCondenser::IsEmbeddingModel("nomic-embed-text:latest"), "nomic-embed must be true");
    TEST_ASSERT(ModelNameCondenser::IsEmbeddingModel("bge-large-en-v1.5"), "bge-large must be true");
    TEST_ASSERT(ModelNameCondenser::IsEmbeddingModel("bert-base-uncased"), "bert must be true");
    TEST_ASSERT(ModelNameCondenser::IsEmbeddingModel("bge-reranker-large"), "reranker must be true");

    // Generative models, especially edge SLMs, must NEVER be classified as embeddings
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("qwen2.5-0.5b"), "qwen2.5-0.5b SLM must not be embedding");
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("smollm-135m"), "smollm-135m SLM must not be embedding");
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("deepseek-r1:14b"), "deepseek-r1 must not be embedding");
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("llama3.2:1b"), "llama3.2:1b must not be embedding");

    // Edge SLMs with explicit param badges below 500MB VRAM must NOT be filtered as embeddings
    uint64_t vram_400mb = 400ULL * 1024 * 1024;
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("smollm-135m", vram_400mb, 500), "smollm-135m must not be filtered even under 500MB VRAM");
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("qwen2.5-0.5b", vram_400mb, 500), "qwen2.5-0.5b must not be filtered even under 500MB VRAM");

    // min_vram_mb threshold honoring (GAP-05)
    TEST_ASSERT(ModelNameCondenser::IsEmbeddingModel("custom-tiny-model", vram_400mb, 500),
                "Sub-threshold model below min_vram_mb must return true (filtered)");
    uint64_t vram_600mb = 600ULL * 1024 * 1024;
    TEST_ASSERT(!ModelNameCondenser::IsEmbeddingModel("custom-generative-model", vram_600mb, 500),
                "Generative model meeting min_vram_mb must return false (not filtered)");

    return true;
}

bool Test_ModelNameCondenser_Formatter7Stages() {
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    Wire::LlmMetrics m{};
    std::strncpy(m.model_name, "deepseek-r1-14b", sizeof(m.model_name) - 1);
    m.tps = 42.5f;

    // 1. Offline
    m.state = Wire::LlmState::Offline;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Offline", "Offline stage");

    // 2. Loading
    m.state = Wire::LlmState::Loading;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Loading...", "Loading stage");

    // 3. Thinking
    m.state = Wire::LlmState::Thinking;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Thinking...", "Thinking stage");

    // 4. Generating
    m.state = Wire::LlmState::Generating;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b 42.5 T/s", "Generating stage");

    // 5. BurstHold (3-second decay hold)
    m.state = Wire::LlmState::BurstHold;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b 42.5 T/s (Done)", "BurstHold stage");

    // 6. Compacting
    m.state = Wire::LlmState::Compacting;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Compacting...", "Compacting stage");

    // 7. Idle
    m.state = Wire::LlmState::Idle;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Idle", "Idle stage");

    // 8. Unloading
    m.state = Wire::LlmState::Unloading;
    TEST_ASSERT(FormatCompositeLlmWidget(m, condenser) == "deepseek-r1-14b Unloading...", "Unloading stage");

    return true;
}

bool Test_ModelNameCondenser_MultiModelCoResidency() {
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    LlmSnapshot snap{};
    snap.is_online = true;
    snap.llm_count = 2;

    std::strncpy(snap.llms[0].model_name, "llama3:8b", sizeof(snap.llms[0].model_name) - 1);
    snap.llms[0].state = Wire::LlmState::Generating;
    snap.llms[0].tps = 38.2f;

    std::strncpy(snap.llms[1].model_name, "qwen2.5:7b", sizeof(snap.llms[1].model_name) - 1);
    snap.llms[1].state = Wire::LlmState::Idle;
    snap.llms[1].tps = 0.0f;

    // Default "active" with 2 models: should target active model and append (+1)
    std::string s_active = FormatMultiModelWidget(snap, condenser, "active");
    TEST_ASSERT(s_active == "llama3:8b 38.2 T/s (+1)", "Active model with co-residency (+1)");

    // Target specific index "0"
    std::string s_idx0 = FormatMultiModelWidget(snap, condenser, "0");
    TEST_ASSERT(s_idx0 == "llama3:8b 38.2 T/s", "Index 0 targeting");

    // Target specific index "1"
    std::string s_idx1 = FormatMultiModelWidget(snap, condenser, "1");
    TEST_ASSERT(s_idx1 == "qwen2.5:7b Idle", "Index 1 targeting");

    // Target "all"
    std::string s_all = FormatMultiModelWidget(snap, condenser, "all");
    TEST_ASSERT(s_all == "llama3:8b 38.2 T/s, qwen2.5:7b Idle", "All models targeting");

    // Offline snapshot
    LlmSnapshot offline_snap{};
    offline_snap.is_online = false;
    TEST_ASSERT(FormatMultiModelWidget(offline_snap, condenser, "active") == "Offline", "Offline snapshot");

    return true;
}

bool Test_LlmCollector_OllamaEmbeddingFiltering() {
    std::string_view ollama_sample = R"({
        "models": [
            {
                "name": "llama3:8b",
                "model": "llama3:8b",
                "size_vram": 5368709120,
                "details": {
                    "quantization_level": "Q4_0"
                }
            },
            {
                "name": "nomic-embed-text:latest",
                "model": "nomic-embed-text:latest",
                "size_vram": 314572800,
                "details": {
                    "quantization_level": "F16"
                }
            }
        ]
    })";

    LlmSnapshot snap{};
    // Test with filter_embeddings = true
    bool ok = LlmCollector::ParseOllamaPs(ollama_sample, snap, 25, true);
    TEST_ASSERT(ok, "ParseOllamaPs must succeed");
    TEST_ASSERT(snap.is_online, "Must be online");
    TEST_ASSERT(snap.llm_count == 1, "Must filter out nomic-embed-text leaving 1 model");
    TEST_ASSERT(std::string(snap.llms[0].model_name) == "llama3:8b", "Remaining model must be llama3:8b");
    TEST_ASSERT(snap.llms[0].state == Wire::LlmState::Generating, "Must be generating due to 25% GPU compute");

    return true;
}
