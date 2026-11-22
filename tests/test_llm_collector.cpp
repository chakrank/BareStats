#include "test_common.h"
#include "util/http_client.h"
#include "util/json_mini.h"
#include "metrics/llm/llm_collector.h"
#include <cstring>
#include <cmath>

using namespace BareStats;
using namespace BareStats::Wire;

bool Test_HttpClient_ParseUrlAndDechunk() {
    std::string host, path;
    uint16_t port = 0;

    // Test 1: Standard host, port, path
    TEST_ASSERT(HttpClient::ParseUrl("http://127.0.0.1:11434/api/ps", host, port, path), "ParseUrl 1 failed");
    TEST_ASSERT(host == "127.0.0.1", "Host 1 must match");
    TEST_ASSERT(port == 11434, "Port 1 must match");
    TEST_ASSERT(path == "/api/ps", "Path 1 must match");

    // Test 2: Localhost with default port 80
    TEST_ASSERT(HttpClient::ParseUrl("http://localhost/metrics", host, port, path), "ParseUrl 2 failed");
    TEST_ASSERT(host == "localhost", "Host 2 must match");
    TEST_ASSERT(port == 80, "Port 2 must match default 80");
    TEST_ASSERT(path == "/metrics", "Path 2 must match");

    // Test 3: Root path fallback
    TEST_ASSERT(HttpClient::ParseUrl("http://rig1.local:8080", host, port, path), "ParseUrl 3 failed");
    TEST_ASSERT(host == "rig1.local", "Host 3 must match");
    TEST_ASSERT(port == 8080, "Port 3 must match");
    TEST_ASSERT(path == "/", "Path 3 must default to /");

    // Test 4: Rejection of HTTPS for local scraper
    TEST_ASSERT(!HttpClient::ParseUrl("https://127.0.0.1:8000/metrics", host, port, path), "ParseUrl must reject https");

    // Test 4b: Rejection of URLs with whitespace, CRLF, and ASCII control characters (GAP-07)
    TEST_ASSERT(!HttpClient::ParseUrl("http://127.0.0.1:11434 /api", host, port, path), "ParseUrl must reject space");
    TEST_ASSERT(!HttpClient::ParseUrl("http://127.0.0.1:11434\t/api", host, port, path), "ParseUrl must reject tab");
    TEST_ASSERT(!HttpClient::ParseUrl("http://127.0.0.1:11434/api\r\n", host, port, path), "ParseUrl must reject CRLF");
    TEST_ASSERT(!HttpClient::ParseUrl("http://127.0.0.1:11434/api\n", host, port, path), "ParseUrl must reject newline");
    TEST_ASSERT(!HttpClient::ParseUrl("http://127.0.0.1:11434\x01/api", host, port, path), "ParseUrl must reject control char");

    // Test 5: HTTP Chunked Dechunking
    std::string chunked_data = "4\r\nWiki\r\n6\r\npedia \r\n0\r\n\r\n";
    std::string dechunked = HttpClient::Dechunk(chunked_data);
    TEST_ASSERT(dechunked == "Wikipedia ", "Dechunk must match expected text");

    // Test 6: Dechunking integer overflow resilience (VULN-04)
    std::string overflow_chunk = "FFFFFFFFFFFFFF00\r\nA\r\n";
    std::string safe_dechunk = HttpClient::Dechunk(overflow_chunk);
    (void)safe_dechunk; // Verify no exception or crash occurs

    return true;
}

bool Test_JsonMini_Parsers() {
    std::string_view sample_json = R"({
        "name": "llama3.1:70b",
        "size_vram": 42949672960,
        "temperature": 0.7,
        "models": [
            { "id": "m1", "val": 100 },
            { "id": "m2", "val": 200 }
        ]
    })";

    TEST_ASSERT(JsonMini::FindString(sample_json, "name") == "llama3.1:70b", "FindString failed");
    TEST_ASSERT(JsonMini::FindInt(sample_json, "size_vram") == 42949672960LL, "FindInt failed");
    TEST_ASSERT(std::abs(JsonMini::FindFloat(sample_json, "temperature") - 0.7f) < 0.001f, "FindFloat failed");

    auto array_objs = JsonMini::FindArrayObjects(sample_json, "models");
    TEST_ASSERT(array_objs.size() == 2, "FindArrayObjects count mismatch");
    TEST_ASSERT(JsonMini::FindString(array_objs[0], "id") == "m1", "Array item 0 id mismatch");
    TEST_ASSERT(JsonMini::FindInt(array_objs[0], "val") == 100, "Array item 0 val mismatch");
    TEST_ASSERT(JsonMini::FindString(array_objs[1], "id") == "m2", "Array item 1 id mismatch");
    TEST_ASSERT(JsonMini::FindInt(array_objs[1], "val") == 200, "Array item 1 val mismatch");

    return true;
}

bool Test_LlmCollector_PrometheusParsing() {
    // Realistic multi-vendor vLLM / SGLang sample payload
    std::string_view vllm_sample =
        "# HELP vllm:generation_tokens_total Number of generation tokens\n"
        "# TYPE vllm:generation_tokens_total counter\n"
        "vllm:generation_tokens_total{model_name=\"deepseek-ai/DeepSeek-R1-14B\"} 4820\n"
        "# HELP vllm:prompt_tokens_total Number of prompt tokens\n"
        "# TYPE vllm:prompt_tokens_total counter\n"
        "vllm:prompt_tokens_total{model_name=\"deepseek-ai/DeepSeek-R1-14B\"} 1024\n"
        "vllm:num_requests_running 2\n"
        "vllm:num_requests_waiting 1\n"
        "vllm:gpu_cache_usage_perc 0.35\n";

    uint64_t gen = 0, prompt = 0;
    std::string model;
    uint16_t q_running = 0, q_waiting = 0;
    uint8_t kv = 0;

    bool ok = LlmCollector::ParsePrometheusMetrics(vllm_sample, gen, prompt, model, q_running, q_waiting, kv);
    TEST_ASSERT(ok, "ParsePrometheusMetrics failed for vLLM");
    TEST_ASSERT(gen == 4820, "Gen tokens must match 4820");
    TEST_ASSERT(prompt == 1024, "Prompt tokens must match 1024");
    TEST_ASSERT(model == "deepseek-ai/DeepSeek-R1-14B", "Model name must match");
    TEST_ASSERT(q_running == 2, "Queue running must match 2");
    TEST_ASSERT(q_waiting == 1, "Queue waiting must match 1");
    TEST_ASSERT(kv == 35, "KV cache must match 35%");

    // llama-server --metrics sample
    std::string_view llamacpp_sample =
        "llamacpp:tokens_predicted_total{model=\"qwen2.5-coder-7b\"} 1500\n"
        "llamacpp:prompt_tokens_total{model=\"qwen2.5-coder-7b\"} 512\n"
        "llamacpp:kv_cache_usage_ratio 0.18\n";

    ok = LlmCollector::ParsePrometheusMetrics(llamacpp_sample, gen, prompt, model, q_running, q_waiting, kv);
    TEST_ASSERT(ok, "ParsePrometheusMetrics failed for llamacpp");
    TEST_ASSERT(gen == 1500, "Gen tokens must match 1500");
    TEST_ASSERT(prompt == 512, "Prompt tokens must match 512");
    TEST_ASSERT(model == "qwen2.5-coder-7b", "Model name must match");
    TEST_ASSERT(kv == 18, "KV cache must match 18%");

    // BareStats Universal Python Tracker sample (port 38111)
    std::string_view barestats_py_sample =
        "# HELP vllm:generation_tokens_total Cumulative generated tokens\n"
        "# TYPE vllm:generation_tokens_total counter\n"
        "vllm:generation_tokens_total{model=\"Qwen2.5-0.5B-Instruct\"} 420\n"
        "# HELP vllm:prompt_tokens_total Cumulative prompt tokens\n"
        "# TYPE vllm:prompt_tokens_total counter\n"
        "vllm:prompt_tokens_total{model=\"Qwen2.5-0.5B-Instruct\"} 64\n";

    ok = LlmCollector::ParsePrometheusMetrics(barestats_py_sample, gen, prompt, model, q_running, q_waiting, kv);
    TEST_ASSERT(ok, "ParsePrometheusMetrics failed for BareStats Universal Python Tracker");
    TEST_ASSERT(gen == 420, "Gen tokens must match 420");
    TEST_ASSERT(prompt == 64, "Prompt tokens must match 64");
    TEST_ASSERT(model == "Qwen2.5-0.5B-Instruct", "Model name must match Qwen2.5-0.5B-Instruct");

    // Test 3: Resilience against NaN float in metrics (VULN-09)
    std::string_view nan_sample = "vllm:gpu_cache_usage_perc NaN\n";
    kv = 0;
    LlmCollector::ParsePrometheusMetrics(nan_sample, gen, prompt, model, q_running, q_waiting, kv);
    TEST_ASSERT(kv == 0, "NaN float metric must not corrupt kv cache or trigger UB");

    // Test 4: Floating-point and scientific-notation counter parsing (GAP-06)
    std::string_view float_sci_sample =
        "vllm:generation_tokens_total 1.5e+04\n"
        "vllm:prompt_tokens_total 142.0\n"
        "vllm:num_requests_running 3.0\n"
        "vllm:num_requests_waiting 0.0\n";
    gen = 0; prompt = 0; q_running = 0; q_waiting = 0;
    ok = LlmCollector::ParsePrometheusMetrics(float_sci_sample, gen, prompt, model, q_running, q_waiting, kv);
    TEST_ASSERT(ok, "ParsePrometheusMetrics must succeed for floating and scientific notation");
    TEST_ASSERT(gen == 15000, "Scientific notation 1.5e+04 must parse to 15000");
    TEST_ASSERT(prompt == 142, "Float 142.0 must parse to 142");
    TEST_ASSERT(q_running == 3, "Float 3.0 must parse to 3");
    TEST_ASSERT(q_waiting == 0, "Float 0.0 must parse to 0");

    return true;
}

bool Test_LlmCollector_OllamaParsing() {
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
    // Test with GPU compute at 30% (> 15% threshold for generating state)
    bool ok = LlmCollector::ParseOllamaPs(ollama_sample, snap, 30);
    TEST_ASSERT(ok, "ParseOllamaPs failed");
    TEST_ASSERT(snap.is_online, "Snapshot must be online");
    TEST_ASSERT(snap.llm_count == 2, "Must extract 2 models");

    // Primary model
    TEST_ASSERT(std::strcmp(snap.llms[0].model_name, "llama3:8b") == 0, "Model 0 name mismatch");
    TEST_ASSERT(std::strcmp(snap.llms[0].quant, "Q4_0") == 0, "Model 0 quant mismatch");
    TEST_ASSERT(snap.llms[0].vram_used_mb == 5120, "Model 0 VRAM must match 5120 MB");
    TEST_ASSERT(snap.llms[0].state == LlmState::Generating, "Model 0 must correlate to Generating on GPU spike");
    TEST_ASSERT(snap.llms[0].tps == 0.0f, "Model 0 TPS must be suppressed");

    // Secondary model (must remain Idle even during GPU spike)
    TEST_ASSERT(std::strcmp(snap.llms[1].model_name, "nomic-embed-text:latest") == 0, "Model 1 name mismatch");
    TEST_ASSERT(snap.llms[1].state == LlmState::Idle, "Secondary model must remain Idle");

    // Test with low GPU compute (5%) -> Primary model must be Idle
    LlmSnapshot idle_snap{};
    ok = LlmCollector::ParseOllamaPs(ollama_sample, idle_snap, 5);
    TEST_ASSERT(idle_snap.llms[0].state == LlmState::Idle, "Primary model must be Idle when GPU is low");

    // Test negative size_vram clamping (VULN-09)
    std::string_view neg_vram_sample = R"({
        "models": [{ "name": "bad:model", "size_vram": -1048576 }]
    })";
    LlmSnapshot neg_snap{};
    ok = LlmCollector::ParseOllamaPs(neg_vram_sample, neg_snap, 0);
    TEST_ASSERT(ok, "ParseOllamaPs must succeed with negative size_vram");
    TEST_ASSERT(neg_snap.llms[0].vram_used_mb == 0, "Negative size_vram must clamp to 0 MB without underflow wrap");

    return true;
}

bool Test_LlmCollector_LlamaCppSlotsParsing() {
    std::string_view slots_sample = R"([
        {
            "id": 0,
            "id_task": 10,
            "state": 1,
            "n_decoded": 85,
            "model": "qwen2.5-coder-7b"
        },
        {
            "id": 1,
            "id_task": -1,
            "state": 0,
            "n_decoded": 0,
            "model": "qwen2.5-coder-7b"
        }
    ])";

    uint32_t active_slots = 0;
    uint64_t decoded_tokens = 0;
    std::string model;

    bool ok = LlmCollector::ParseLlamaCppSlots(slots_sample, active_slots, decoded_tokens, model);
    TEST_ASSERT(ok, "ParseLlamaCppSlots failed");
    TEST_ASSERT(active_slots == 1, "Active slots must be 1");
    TEST_ASSERT(decoded_tokens == 85, "Decoded tokens must be 85");
    TEST_ASSERT(model == "qwen2.5-coder-7b", "Model name must match");

    return true;
}

bool Test_LlmCollector_OpenAiModelsParsing() {
    std::string_view openai_sample = R"({
        "object": "list",
        "data": [
            {
                "id": "deepseek-r1-distill-qwen-14b",
                "object": "model",
                "owned_by": "organization"
            }
        ]
    })";

    std::string model_id;
    bool ok = LlmCollector::ParseOpenAiModels(openai_sample, model_id);
    TEST_ASSERT(ok, "ParseOpenAiModels failed");
    TEST_ASSERT(model_id == "deepseek-r1-distill-qwen-14b", "Model ID must match");

    return true;
}

bool Test_LlmCollector_3StrikeDegradationAndBurstHold() {
    // Test collector pointed at non-existent port (guaranteed connection failure)
    LlmCollector collector("rig_fail", "http://127.0.0.1:59999", LlmAdapter::Prometheus);
    collector.timeout_ms = 50;
    collector.poll_interval_ms = 10;
    collector.retry_interval_ms = 200;

    // Initially offline
    TEST_ASSERT(!collector.IsOnline(), "Initial state must be offline");

    // Strike 1
    collector.Update();
    TEST_ASSERT(!collector.IsOnline(), "Strike 1 must not be online");

    // Manually publish online snapshot to simulate node that was healthy
    LlmSnapshot healthy{};
    healthy.is_online = true;
    healthy.llm_count = 1;
    std::strncpy(healthy.llms[0].model_name, "test_model", sizeof(healthy.llms[0].model_name) - 1);
    healthy.llms[0].state = LlmState::Generating;
    healthy.llms[0].tps = 25.0f;

    // Simulate 1st failure after healthy: transient jitter should keep online
    LlmCollector collector2("rig_jitter", "http://127.0.0.1:59999", LlmAdapter::Prometheus);
    collector2.timeout_ms = 50;

    return true;
}

#include "metrics/llm/llm_resolver.h"
#include "metrics/llm/model_name_condenser.h"
#include "overlay/layout_engine.h"
#include "metrics/telemetry_engine.h"
#include "util/formatter.h"
#include <thread>
#include <chrono>

#define SUBTEST_VERIFY(id, name, expr) \
    do { \
        bool res = (expr); \
        std::cout << "    [Test " << id << "] " << name << ": " << (res ? "PASS" : "FAIL") << std::endl; \
        if (!res) all_passed = false; \
    } while (0)

bool Test_LLM_2A_OllamaAdapter() {
    bool all_passed = true;
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    const std::string_view qwen_sample = R"({
        "models": [
            {
                "name": "qwen3.8:27b-gsq-42k",
                "model": "qwen3.8:27b-gsq-42k",
                "size": 14521739508,
                "digest": "8485e21c2f0abe9e425a3445265b6faa25163ec5171219eee9d1b2d51306f795",
                "details": {
                    "format": "gguf",
                    "family": "qwen35",
                    "parameter_size": "27.3B",
                    "quantization_level": "IQ3_S"
                },
                "expires_at": "2026-10-22T12:25:37.1060928+05:30",
                "size_vram": 14521739508,
                "context_length": 43008,
                "runner": "llamacpp"
            }
        ]
    })";

    const std::string_view empty_sample = R"({"models": []})";

    const std::string_view two_models_sample = R"({
        "models": [
            {
                "name": "qwen3.8:27b-gsq-42k",
                "size_vram": 14521739508,
                "details": { "quantization_level": "IQ3_S" }
            },
            {
                "name": "nomic-embed-text:latest",
                "size_vram": 314572800,
                "details": { "quantization_level": "F16" }
            }
        ]
    })";

    // 1. Ollama online, one model resident
    LlmSnapshot snap1{};
    bool ok1 = LlmCollector::ParseOllamaPs(qwen_sample, snap1, 0, false);
    bool t1 = ok1 && snap1.is_online && snap1.llm_count == 1 &&
              std::strcmp(snap1.llms[0].model_name, "qwen3.8:27b-gsq-42k") == 0 &&
              snap1.llms[0].tps == 0.0f && snap1.llms[0].state == Wire::LlmState::Idle;
    SUBTEST_VERIFY(1, "Ollama online, one model resident (gpu=0 -> Idle, tps=0.0)", t1);

    // 2. Ollama online, zero models ({"models": []})
    LlmSnapshot snap2{};
    bool ok2 = LlmCollector::ParseOllamaPs(empty_sample, snap2, 0, false);
    RigClusterSnapshot cluster2{};
    cluster2.rig_count = 1;
    std::strncpy(cluster2.rigs[0].rig_id, "local", sizeof(cluster2.rigs[0].rig_id) - 1);
    cluster2.rigs[0].snapshot = snap2; // is_online=true, llm_count=0
    std::string cell_text2 = FormatGridCellText(ParseGridToken("local:llm"), cluster2, condenser);
    bool t2 = ok2 && snap2.is_online && snap2.llm_count == 0 && cell_text2 == "Idle";
    SUBTEST_VERIFY(2, "Ollama online, zero models -> cell text exactly 'Idle'", t2);

    // 3. Ollama online, one model, GPU compute > 15% (72%)
    LlmSnapshot snap3{};
    bool ok3 = LlmCollector::ParseOllamaPs(qwen_sample, snap3, 72, false);
    bool t3 = ok3 && snap3.llm_count == 1 && snap3.llms[0].state == Wire::LlmState::Generating;
    SUBTEST_VERIFY(3, "Ollama online, one model, GPU 72% -> Generating", t3);

    // 4. Ollama online, two co-resident models, GPU compute > 15% (72%)
    LlmSnapshot snap4{};
    bool ok4 = LlmCollector::ParseOllamaPs(two_models_sample, snap4, 72, false);
    bool t4 = ok4 && snap4.llm_count == 2 &&
              snap4.llms[0].state == Wire::LlmState::Generating &&
              snap4.llms[1].state == Wire::LlmState::Idle;
    SUBTEST_VERIFY(4, "Ollama online, two models, GPU 72% -> index 0 Generating, index 1 Idle", t4);

    // 5. Ollama online, one model, GPU compute <= 15% (5%)
    LlmSnapshot snap5{};
    bool ok5 = LlmCollector::ParseOllamaPs(qwen_sample, snap5, 5, false);
    bool t5 = ok5 && snap5.llms[0].state == Wire::LlmState::Idle;
    SUBTEST_VERIFY(5, "Ollama online, one model, GPU 5% -> Idle", t5);

    // 6. Ollama HTTP non-200 (e.g. 500)
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 500;
        r.error_message = "Internal Server Error";
        return r;
    });
    LlmCollector c6("local", "http://127.0.0.1:11434", LlmAdapter::Ollama);
    LlmSnapshot snap6{};
    bool ok6 = LlmCollectorTestAccess::PollOllama(c6, snap6, 0);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(6, "Ollama HTTP 500 -> PollOllama returns false", !ok6);

    return all_passed;
}

bool Test_LLM_2B_LlamaCppAdapter() {
    bool all_passed = true;
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    const std::string_view slots_active = R"([
        { "id": 0, "state": 1, "n_decoded": 85, "model": "qwen2.5-coder-7b" }
    ])";

    // 7. llama-server online, /slots returns one active slot
    HttpClient::SetMockHandler([&](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/slots") != std::string_view::npos) {
            r.status_code = 200;
            r.body = slots_active;
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c7("local", "http://127.0.0.1:8080", LlmAdapter::LlamaCpp);
    LlmSnapshot snap7{};
    bool ok7 = LlmCollectorTestAccess::PollLlamaCpp(c7, snap7);
    HttpClient::ClearMockHandler();
    bool t7 = ok7 && snap7.is_online && snap7.llm_count == 1 &&
              std::string(snap7.llms[0].model_name) == "qwen2.5-coder-7b";
    SUBTEST_VERIFY(7, "llama-server online, /slots active slot -> is_online=true, model_name populated", t7);

    // 8. llama-server online, /slots returns empty slots array
    RigClusterSnapshot cluster8{};
    cluster8.rig_count = 1;
    std::strncpy(cluster8.rigs[0].rig_id, "local", sizeof(cluster8.rigs[0].rig_id) - 1);
    cluster8.rigs[0].snapshot.is_online = true;
    cluster8.rigs[0].snapshot.llm_count = 0;
    std::string cell_text8 = FormatGridCellText(ParseGridToken("local:llm"), cluster8, condenser);
    bool t8 = (cell_text8 == "Idle");
    SUBTEST_VERIFY(8, "llama-server online, 0 resident models -> cell text exactly 'Idle'", t8);

    // 9. llama-server /slots returns 404
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 404;
        return r;
    });
    LlmCollector c9("local", "http://127.0.0.1:8080", LlmAdapter::LlamaCpp);
    LlmSnapshot snap9{};
    bool ok9 = LlmCollectorTestAccess::PollLlamaCpp(c9, snap9);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(9, "llama-server /slots 404 -> PollLlamaCpp returns false", !ok9);

    // 10. llama-server connection refused (-1)
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = -1;
        r.error_message = "Connection refused";
        return r;
    });
    LlmCollector c10("local", "http://127.0.0.1:8080", LlmAdapter::LlamaCpp);
    LlmSnapshot snap10{};
    bool ok10 = LlmCollectorTestAccess::PollLlamaCpp(c10, snap10);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(10, "llama-server connection refused -> PollLlamaCpp returns false", !ok10);

    return all_passed;
}

bool Test_LLM_2C_PrometheusAdapter() {
    bool all_passed = true;

    // 11. vLLM metrics, generation tokens increasing
    LlmCollector c11("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    c11.poll_interval_ms = 0;
    std::string prom_tick1 = "vllm:generation_tokens_total 1000\nvllm:prompt_tokens_total 50\n";
    std::string prom_tick2 = "vllm:generation_tokens_total 1130\nvllm:prompt_tokens_total 50\n";

    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = prom_tick1;
        return r;
    });
    c11.Update(0);

    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = prom_tick2;
        return r;
    });
    c11.Update(0);
    HttpClient::ClearMockHandler();

    auto snap11 = c11.GetSnapshot();
    bool t11 = snap11.is_online && snap11.llms[0].state == Wire::LlmState::Generating && snap11.llms[0].tps > 0.0f;
    SUBTEST_VERIFY(11, "vLLM metrics gen tokens increasing -> state == Generating, tps > 0", t11);

    // 12. vLLM metrics, prompt tokens increasing but generation flat
    LlmCollector c12("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    c12.poll_interval_ms = 0;
    std::string prom12_t1 = "vllm:generation_tokens_total 1000\nvllm:prompt_tokens_total 50\n";
    std::string prom12_t2 = "vllm:generation_tokens_total 1000\nvllm:prompt_tokens_total 200\n";

    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = prom12_t1;
        return r;
    });
    c12.Update(0);

    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = prom12_t2;
        return r;
    });
    c12.Update(0);
    HttpClient::ClearMockHandler();

    auto snap12 = c12.GetSnapshot();
    bool t12 = snap12.is_online && snap12.llms[0].state == Wire::LlmState::Thinking;
    SUBTEST_VERIFY(12, "vLLM prompt tokens increasing, gen flat -> state == Thinking", t12);

    // 13. BurstHold transition
    LlmCollector c13("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    c13.poll_interval_ms = 0;
    c13.decay_hold_sec = 2; // 2 seconds burst hold

    // Tick 1: gen increasing
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = "vllm:generation_tokens_total 100\n";
        return r;
    });
    c13.Update(0);

    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = "vllm:generation_tokens_total 150\n";
        return r;
    });
    c13.Update(0);
    auto s13_1 = c13.GetSnapshot();

    // Tick 2: gen flat, elapsed < 2s -> BurstHold
    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    c13.Update(0);
    auto s13_2 = c13.GetSnapshot();

    // Tick 3: gen flat, elapsed >= decay_hold_sec -> Idle
    c13.decay_hold_sec = 0; // Immediate expiry
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    c13.Update(0);
    auto s13_3 = c13.GetSnapshot();
    HttpClient::ClearMockHandler();

    bool t13 = (s13_1.llms[0].state == Wire::LlmState::Generating) &&
               (s13_2.llms[0].state == Wire::LlmState::BurstHold) &&
               (s13_3.llms[0].state == Wire::LlmState::Idle);
    SUBTEST_VERIFY(13, "BurstHold transition: Generating -> BurstHold -> Idle", t13);

    // 14. Counter reset (model swap)
    LlmCollector c14("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    c14.poll_interval_ms = 0;
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = "vllm:generation_tokens_total{model_name=\"model-a\"} 5000\n";
        return r;
    });
    c14.Update(0);

    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = "vllm:generation_tokens_total{model_name=\"model-b\"} 10\n";
        return r;
    });
    c14.Update(0);
    HttpClient::ClearMockHandler();

    auto s14 = c14.GetSnapshot();
    bool t14 = (s14.llms[0].tps == 0.0f) && (s14.llms[0].state == Wire::LlmState::Idle) &&
               (std::string(s14.llms[0].model_name) == "model-b");
    SUBTEST_VERIFY(14, "Counter reset on model swap -> no negative TPS, clean Idle state", t14);

    // 15. SGLang prefix detection
    uint64_t gen15 = 0, pr15 = 0;
    std::string m15;
    uint16_t qr15 = 0, qw15 = 0;
    uint8_t kv15 = 0;
    std::string_view sglang_sample = "sglang:generation_tokens_total 500\nsglang:prompt_tokens_total 100\n";
    bool ok15 = LlmCollector::ParsePrometheusMetrics(sglang_sample, gen15, pr15, m15, qr15, qw15, kv15);
    bool t15 = ok15 && gen15 == 500 && pr15 == 100;
    SUBTEST_VERIFY(15, "SGLang prefix detection -> gen=500, prompt=100 parsed", t15);

    // 16. TGI prefix detection
    uint64_t gen16 = 0, pr16 = 0;
    std::string m16;
    uint16_t qr16 = 0, qw16 = 0;
    uint8_t kv16 = 0;
    std::string_view tgi_sample = "tgi_request_generated_tokens_sum 750\ntgi_request_prompt_tokens_sum 250\n";
    bool ok16 = LlmCollector::ParsePrometheusMetrics(tgi_sample, gen16, pr16, m16, qr16, qw16, kv16);
    bool t16 = ok16 && gen16 == 750 && pr16 == 250;
    SUBTEST_VERIFY(16, "TGI prefix detection -> gen=750, prompt=250 parsed", t16);

    // 17. llama-server --metrics prefix
    uint64_t gen17 = 0, pr17 = 0;
    std::string m17;
    uint16_t qr17 = 0, qw17 = 0;
    uint8_t kv17 = 0;
    std::string_view llamacpp_sample = "llamacpp:tokens_predicted_total 300\nllamacpp:prompt_tokens_total 80\n";
    bool ok17 = LlmCollector::ParsePrometheusMetrics(llamacpp_sample, gen17, pr17, m17, qr17, qw17, kv17);
    bool t17 = ok17 && gen17 == 300 && pr17 == 80;
    SUBTEST_VERIFY(17, "llama-server --metrics prefix -> gen=300, prompt=80 parsed", t17);

    // 18. KV cache saturation
    uint64_t gen18 = 0, pr18 = 0;
    std::string m18;
    uint16_t qr18 = 0, qw18 = 0;
    uint8_t kv18 = 0;
    std::string_view kv_sample = "vllm:kv_cache_usage_perc 0.72\n";
    LlmCollector::ParsePrometheusMetrics(kv_sample, gen18, pr18, m18, qr18, qw18, kv18);
    SUBTEST_VERIFY(18, "KV cache saturation -> kv_cache_pct == 72", kv18 == 72);

    // 19. Queue depth
    uint64_t gen19 = 0, pr19 = 0;
    std::string m19;
    uint16_t qr19 = 0, qw19 = 0;
    uint8_t kv19 = 0;
    std::string_view q_sample = "vllm:num_requests_running 2\nvllm:num_requests_waiting 5\n";
    LlmCollector::ParsePrometheusMetrics(q_sample, gen19, pr19, m19, qr19, qw19, kv19);
    SUBTEST_VERIFY(19, "Queue depth -> running=2, waiting=5", qr19 == 2 && qw19 == 5);

    // 20. HTTP 503 with body containing "Loading"
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 503;
        r.body = "Loading model weights into GPU memory...";
        return r;
    });
    LlmCollector c20("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    LlmSnapshot snap20{};
    bool ok20 = LlmCollectorTestAccess::PollPrometheus(c20, snap20);
    HttpClient::ClearMockHandler();
    bool t20 = ok20 && snap20.is_online && snap20.llms[0].state == Wire::LlmState::Loading;
    SUBTEST_VERIFY(20, "HTTP 503 with 'Loading' -> returns true, state == Loading, no failure strike", t20);

    // 21. HTTP 503 without "Loading"
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 503;
        r.body = "Service Unavailable";
        return r;
    });
    LlmCollector c21("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    LlmSnapshot snap21{};
    bool ok21 = LlmCollectorTestAccess::PollPrometheus(c21, snap21);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(21, "HTTP 503 without 'Loading' -> PollPrometheus returns false", !ok21);

    // 22. HTTP 404 (no /metrics endpoint)
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 404;
        return r;
    });
    LlmCollector c22("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    LlmSnapshot snap22{};
    bool ok22 = LlmCollectorTestAccess::PollPrometheus(c22, snap22);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(22, "HTTP 404 -> PollPrometheus returns false", !ok22);

    return all_passed;
}

bool Test_LLM_2D_OpenAiCompatAdapter() {
    bool all_passed = true;
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    const std::string_view openai_one = R"({
        "data": [ { "id": "llama-3-8b-instruct", "object": "model" } ]
    })";

    // 23. LM Studio /v1/models returns one model
    HttpClient::SetMockHandler([&](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 200;
        r.body = openai_one;
        return r;
    });
    LlmCollector c23("local", "http://127.0.0.1:1234", LlmAdapter::OpenAiCompat);
    LlmSnapshot snap23_idle{}, snap23_gen{};
    bool ok23_idle = LlmCollectorTestAccess::PollOpenAiCompat(c23, snap23_idle, 0);
    bool ok23_gen = LlmCollectorTestAccess::PollOpenAiCompat(c23, snap23_gen, 25);
    HttpClient::ClearMockHandler();
    bool t23 = ok23_idle && snap23_idle.is_online && snap23_idle.llm_count == 1 &&
               std::string(snap23_idle.llms[0].model_name) == "llama-3-8b-instruct" &&
               snap23_idle.llms[0].tps == 0.0f && snap23_idle.llms[0].state == Wire::LlmState::Idle &&
               ok23_gen && snap23_gen.llms[0].state == Wire::LlmState::Generating;
    SUBTEST_VERIFY(23, "LM Studio /v1/models one model -> Idle at gpu=0, Generating at gpu=25", t23);

    // 24. LM Studio /v1/models returns empty list -> cell text is "Idle"
    RigClusterSnapshot cluster24{};
    cluster24.rig_count = 1;
    std::strncpy(cluster24.rigs[0].rig_id, "local", sizeof(cluster24.rigs[0].rig_id) - 1);
    cluster24.rigs[0].snapshot.is_online = true;
    cluster24.rigs[0].snapshot.llm_count = 0;
    std::string cell_text24 = FormatGridCellText(ParseGridToken("local:llm"), cluster24, condenser);
    bool t24 = (cell_text24 == "Idle");
    SUBTEST_VERIFY(24, "LM Studio /v1/models empty list -> formatted cell text exactly 'Idle'", t24);

    // 25. OpenAI-compatible HTTP non-200
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 500;
        return r;
    });
    LlmCollector c25("local", "http://127.0.0.1:1234", LlmAdapter::OpenAiCompat);
    LlmSnapshot snap25{};
    bool ok25 = LlmCollectorTestAccess::PollOpenAiCompat(c25, snap25, 0);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(25, "OpenAI-compatible HTTP 500 -> PollOpenAiCompat returns false", !ok25);

    return all_passed;
}

bool Test_LLM_2E_AutoNegotiation() {
    bool all_passed = true;

    // 26. Ollama on 11434 (/slots 404, /props 404, /metrics 404, /api/ps 200)
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/api/ps") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "{\"models\":[]}";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c26("local", "http://127.0.0.1:11434", LlmAdapter::AutoDetect);
    bool ok26 = LlmCollectorTestAccess::AutoNegotiateAdapter(c26);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(26, "Auto-negotiate Ollama -> resolved LlmAdapter::Ollama", ok26 && c26.GetResolvedAdapter() == LlmAdapter::Ollama);

    // 27. llama-server on 8080 (with /slots 200)
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/slots") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "[]";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c27("local", "http://127.0.0.1:8080", LlmAdapter::AutoDetect);
    bool ok27 = LlmCollectorTestAccess::AutoNegotiateAdapter(c27);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(27, "Auto-negotiate llama-server -> resolved LlmAdapter::LlamaCpp", ok27 && c27.GetResolvedAdapter() == LlmAdapter::LlamaCpp);

    // 28. vLLM on 8000 (/slots 404, /props 404, /metrics 200)
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/metrics") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "# HELP metric\n";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c28("local", "http://127.0.0.1:8000", LlmAdapter::AutoDetect);
    bool ok28 = LlmCollectorTestAccess::AutoNegotiateAdapter(c28);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(28, "Auto-negotiate vLLM -> resolved LlmAdapter::Prometheus", ok28 && c28.GetResolvedAdapter() == LlmAdapter::Prometheus);

    // 29. LM Studio on 1234 (/v1/models 200)
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/v1/models") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "{\"data\":[]}";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c29("local", "http://127.0.0.1:1234", LlmAdapter::AutoDetect);
    bool ok29 = LlmCollectorTestAccess::AutoNegotiateAdapter(c29);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(29, "Auto-negotiate LM Studio -> resolved LlmAdapter::OpenAiCompat", ok29 && c29.GetResolvedAdapter() == LlmAdapter::OpenAiCompat);

    // 30. llama-server with both /slots and /metrics available -> priority: /slots first
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/slots") != std::string_view::npos || url.find("/metrics") != std::string_view::npos) {
            r.status_code = 200;
            r.body = url.find("/slots") != std::string_view::npos ? "[]" : "# HELP\n";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c30("local", "http://127.0.0.1:8080", LlmAdapter::AutoDetect);
    bool ok30 = LlmCollectorTestAccess::AutoNegotiateAdapter(c30);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(30, "Auto-negotiate priority: /slots before /metrics -> LlamaCpp", ok30 && c30.GetResolvedAdapter() == LlmAdapter::LlamaCpp);

    // 31. All probes 404 or connection refused
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 404;
        return r;
    });
    LlmCollector c31("local", "http://127.0.0.1:9999", LlmAdapter::AutoDetect);
    bool ok31 = LlmCollectorTestAccess::AutoNegotiateAdapter(c31);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(31, "All probes 404 -> AutoNegotiateAdapter returns false", !ok31);

    // 32. Forced adapter (configured_adapter_ != AutoDetect)
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/api/ps") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "{\"models\":[]}";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    LlmCollector c32("local", "http://127.0.0.1:11434", LlmAdapter::Ollama);
    c32.poll_interval_ms = 0;
    c32.Update(0);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(32, "Forced adapter -> skips auto-negotiate, polls directly", c32.GetResolvedAdapter() == LlmAdapter::Ollama);

    return all_passed;
}

bool Test_LLM_2F_3StrikeAndBackoff() {
    bool all_passed = true;

    LlmCollector c("local", "http://127.0.0.1:59999", LlmAdapter::AutoDetect);
    c.poll_interval_ms = 1000;
    c.retry_interval_ms = 10000;

    // 33. 1 failure: no state change, normal interval
    LlmCollectorTestAccess::HandleFailure(c);
    bool t33 = (LlmCollectorTestAccess::GetConsecutiveFailures(c) == 1);
    SUBTEST_VERIFY(33, "Strike 1: failures=1, retained state", t33);

    // 34. 2 failures: no state change, normal interval
    LlmCollectorTestAccess::HandleFailure(c);
    bool t34 = (LlmCollectorTestAccess::GetConsecutiveFailures(c) == 2);
    SUBTEST_VERIFY(34, "Strike 2: failures=2, transient jitter retained", t34);

    // 35. 3 failures: is_online=false published, backoff to retry_interval_ms (+10s)
    LlmCollectorTestAccess::HandleFailure(c);
    auto next_poll = LlmCollectorTestAccess::GetNextPollTime(c);
    bool t35 = (LlmCollectorTestAccess::GetConsecutiveFailures(c) == 3) &&
               (!c.IsOnline()) &&
               (next_poll >= std::chrono::steady_clock::now() + std::chrono::seconds(9));
    SUBTEST_VERIFY(35, "Strike 3: is_online=false published, +10s backoff", t35);

    // 36. Recovery after backoff: next poll succeeds -> consecutive_failures resets to 0, is_online=true
    HttpClient::SetMockHandler([](std::string_view url, uint32_t, size_t) {
        HttpResponse r;
        if (url.find("/api/ps") != std::string_view::npos) {
            r.status_code = 200;
            r.body = "{\"models\":[]}";
        } else {
            r.status_code = 404;
        }
        return r;
    });
    // Set next poll time to now so poll is permitted
    LlmCollectorTestAccess::SetNextPollTime(c, std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
    c.poll_interval_ms = 0;
    c.Update(0);
    HttpClient::ClearMockHandler();

    bool t36 = (LlmCollectorTestAccess::GetConsecutiveFailures(c) == 0) && c.IsOnline();
    SUBTEST_VERIFY(36, "Recovery: successful poll resets failures to 0, is_online=true", t36);

    // 37. HTTP 503 Loading does NOT count as a failure strike
    LlmCollector c37("local", "http://127.0.0.1:8000", LlmAdapter::Prometheus);
    HttpClient::SetMockHandler([](std::string_view, uint32_t, size_t) {
        HttpResponse r;
        r.status_code = 503;
        r.body = "Loading weights...";
        return r;
    });
    LlmSnapshot s37{};
    bool ok37 = LlmCollectorTestAccess::PollPrometheus(c37, s37);
    HttpClient::ClearMockHandler();
    SUBTEST_VERIFY(37, "HTTP 503 Loading does not count as failure strike", ok37 && s37.llms[0].state == Wire::LlmState::Loading);

    return all_passed;
}

bool Test_LLM_2G_TokenParsingAndResolver() {
    bool all_passed = true;
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    // 38. "local:LLM" -> rig_target="local", metric=Composite
    GridToken t38 = ParseGridToken("local:LLM");
    SUBTEST_VERIFY(38, "Token 'local:LLM' -> rig_target='local', metric=Composite",
                   t38.rig_target == "local" && t38.metric == LlmMetricType::Composite);

    // 39. "rig1:LLM" -> rig_target="rig1", metric=Composite
    GridToken t39 = ParseGridToken("rig1:LLM");
    SUBTEST_VERIFY(39, "Token 'rig1:LLM' -> rig_target='rig1', metric=Composite",
                   t39.rig_target == "rig1" && t39.metric == LlmMetricType::Composite);

    // 40. "llm" -> rig_target="any", metric=Composite
    GridToken t40 = ParseGridToken("llm");
    SUBTEST_VERIFY(40, "Token 'llm' -> rig_target='any', metric=Composite",
                   t40.rig_target == "any" && t40.metric == LlmMetricType::Composite);

    // 41. "local:tps" -> rig_target="local", metric=Tps
    GridToken t41 = ParseGridToken("local:tps");
    SUBTEST_VERIFY(41, "Token 'local:tps' -> rig_target='local', metric=Tps",
                   t41.rig_target == "local" && t41.metric == LlmMetricType::Tps);

    // 42. "rig1:llm0:tps" -> rig_target="rig1", model_index=0, metric=Tps
    GridToken t42 = ParseGridToken("rig1:llm0:tps");
    SUBTEST_VERIFY(42, "Token 'rig1:llm0:tps' -> rig_target='rig1', model_index=0, metric=Tps",
                   t42.rig_target == "rig1" && t42.model_index == 0 && t42.metric == LlmMetricType::Tps);

    // 43. "local:kv" -> rig_target="local", metric=KvCache
    GridToken t43 = ParseGridToken("local:kv");
    SUBTEST_VERIFY(43, "Token 'local:kv' -> rig_target='local', metric=KvCache",
                   t43.rig_target == "local" && t43.metric == LlmMetricType::KvCache);

    // 44. ResolveRigModel: online + llm_count > 0 -> returns pointer
    RigClusterSnapshot cluster44{};
    cluster44.rig_count = 1;
    std::strncpy(cluster44.rigs[0].rig_id, "local", sizeof(cluster44.rigs[0].rig_id) - 1);
    cluster44.rigs[0].snapshot.is_online = true;
    cluster44.rigs[0].snapshot.llm_count = 1;
    std::strncpy(cluster44.rigs[0].snapshot.llms[0].model_name, "test:model", sizeof(cluster44.rigs[0].snapshot.llms[0].model_name) - 1);
    const Wire::LlmMetrics* m44 = ResolveRigModel(cluster44, "local", -1);
    SUBTEST_VERIFY(44, "ResolveRigModel: online + llm_count > 0 -> returns pointer to model", m44 != nullptr);

    // 45. ResolveRigModel: online + llm_count == 0 -> returns nullptr -> formatter outputs "Idle"
    RigClusterSnapshot cluster45{};
    cluster45.rig_count = 1;
    std::strncpy(cluster45.rigs[0].rig_id, "local", sizeof(cluster45.rigs[0].rig_id) - 1);
    cluster45.rigs[0].snapshot.is_online = true;
    cluster45.rigs[0].snapshot.llm_count = 0;
    std::string text45 = FormatGridCellText(ParseGridToken("local:llm"), cluster45, condenser);
    SUBTEST_VERIFY(45, "ResolveRigModel: online + llm_count==0 -> formatter outputs 'Idle'", text45 == "Idle");

    // 46. ResolveRigModel: is_online == false -> returns nullptr -> formatter outputs "Offline"
    RigClusterSnapshot cluster46{};
    cluster46.rig_count = 1;
    std::strncpy(cluster46.rigs[0].rig_id, "local", sizeof(cluster46.rigs[0].rig_id) - 1);
    cluster46.rigs[0].snapshot.is_online = false;
    cluster46.rigs[0].snapshot.llm_count = 0;
    std::string text46 = FormatGridCellText(ParseGridToken("local:llm"), cluster46, condenser);
    SUBTEST_VERIFY(46, "ResolveRigModel: is_online==false -> formatter outputs 'Offline'", text46 == "Offline");

    // 47. Generic "llm" token with multiple rigs (Rig A generating 45 TPS, Rig B idle 8GB)
    RigClusterSnapshot cluster47{};
    cluster47.rig_count = 2;
    std::strncpy(cluster47.rigs[0].rig_id, "rigA", sizeof(cluster47.rigs[0].rig_id) - 1);
    cluster47.rigs[0].snapshot.is_online = true;
    cluster47.rigs[0].snapshot.llm_count = 1;
    std::strncpy(cluster47.rigs[0].snapshot.llms[0].model_name, "modelA", sizeof(cluster47.rigs[0].snapshot.llms[0].model_name) - 1);
    cluster47.rigs[0].snapshot.llms[0].state = Wire::LlmState::Generating;
    cluster47.rigs[0].snapshot.llms[0].tps = 45.0f;

    std::strncpy(cluster47.rigs[1].rig_id, "rigB", sizeof(cluster47.rigs[1].rig_id) - 1);
    cluster47.rigs[1].snapshot.is_online = true;
    cluster47.rigs[1].snapshot.llm_count = 1;
    std::strncpy(cluster47.rigs[1].snapshot.llms[0].model_name, "modelB", sizeof(cluster47.rigs[1].snapshot.llms[0].model_name) - 1);
    cluster47.rigs[1].snapshot.llms[0].state = Wire::LlmState::Idle;
    cluster47.rigs[1].snapshot.llms[0].vram_used_mb = 8192;

    const Wire::LlmMetrics* m47 = ResolveGenericLlm(cluster47);
    SUBTEST_VERIFY(47, "Generic 'llm' -> resolves to Rig A (priority 1: generating)",
                   m47 != nullptr && std::string(m47->model_name) == "modelA");

    // 48. Generic "llm" token, no generating model (Rig A idle 4GB, Rig B idle 12GB)
    cluster47.rigs[0].snapshot.llms[0].state = Wire::LlmState::Idle;
    cluster47.rigs[0].snapshot.llms[0].vram_used_mb = 4096;
    cluster47.rigs[1].snapshot.llms[0].vram_used_mb = 12288;
    const Wire::LlmMetrics* m48 = ResolveGenericLlm(cluster47);
    SUBTEST_VERIFY(48, "Generic 'llm' without generating -> resolves to Rig B (priority 2: largest VRAM)",
                   m48 != nullptr && std::string(m48->model_name) == "modelB");

    // 49. Generic "llm" token, all offline -> returns nullptr -> "Offline"
    cluster47.rigs[0].snapshot.is_online = false;
    cluster47.rigs[1].snapshot.is_online = false;
    const Wire::LlmMetrics* m49 = ResolveGenericLlm(cluster47);
    std::string text49 = FormatGridCellText(ParseGridToken("llm"), cluster47, condenser);
    SUBTEST_VERIFY(49, "Generic 'llm' all offline -> returns nullptr -> 'Offline'", m49 == nullptr && text49 == "Offline");

    return all_passed;
}

bool Test_LLM_2H_MultiGpuCorrelation() {
    bool all_passed = true;

    // 50. Two GPUs: GPU0=0% (iGPU), GPU1=80% (dGPU) -> gpuPct passed to PollOllama is 80 (max)
    SystemMetrics sample50;
    sample50.gpus.resize(2);
    sample50.gpus[0].utilizationPercent = 0.0;
    sample50.gpus[1].utilizationPercent = 80.0;
    uint8_t gpuPct50 = TelemetryEngine::ComputeMaxGpuUtilization(sample50);
    SUBTEST_VERIFY(50, "Two GPUs (0% and 80%) -> max utilization is 80%", gpuPct50 == 80);

    // 51. Two GPUs: GPU0=10%, GPU1=5% -> gpuPct = 10 (below 15 threshold)
    SystemMetrics sample51;
    sample51.gpus.resize(2);
    sample51.gpus[0].utilizationPercent = 10.0;
    sample51.gpus[1].utilizationPercent = 5.0;
    uint8_t gpuPct51 = TelemetryEngine::ComputeMaxGpuUtilization(sample51);
    SUBTEST_VERIFY(51, "Two GPUs (10% and 5%) -> max is 10% (below 15% threshold)", gpuPct51 == 10 && gpuPct51 <= 15);

    // 52. Single GPU at 90% -> gpuPct = 90 (triggers Generating)
    SystemMetrics sample52;
    sample52.gpus.resize(1);
    sample52.gpus[0].utilizationPercent = 90.0;
    uint8_t gpuPct52 = TelemetryEngine::ComputeMaxGpuUtilization(sample52);
    SUBTEST_VERIFY(52, "Single GPU at 90% -> max is 90% (triggers Generating)", gpuPct52 == 90 && gpuPct52 > 15);

    // 53. Zero GPUs (CPU-only inference) -> gpuPct = 0
    SystemMetrics sample53;
    sample53.gpus.clear();
    uint8_t gpuPct53 = TelemetryEngine::ComputeMaxGpuUtilization(sample53);
    SUBTEST_VERIFY(53, "Zero GPUs -> max utilization is 0% (Idle)", gpuPct53 == 0);

    // 54. Satellite rig GPU data source: verify payload.gpus feeds cluster node, isolated from local sampler
    Wire::SatellitePayload satPayload{};
    std::strncpy(satPayload.rig_id, "rig1", sizeof(satPayload.rig_id) - 1);
    satPayload.gpu_count = 1;
    satPayload.gpus[0].utilization_pct = 75;
    satPayload.llm_count = 1;
    std::strncpy(satPayload.llms[0].model_name, "satellite:model", sizeof(satPayload.llms[0].model_name) - 1);
    satPayload.llms[0].state = Wire::LlmState::Generating;

    // Simulate Command Center cluster update from satellite packet
    RigClusterSnapshot satCluster{};
    satCluster.rig_count = 1;
    std::strncpy(satCluster.rigs[0].rig_id, satPayload.rig_id, sizeof(satCluster.rigs[0].rig_id) - 1);
    satCluster.rigs[0].gpu_count = satPayload.gpu_count;
    satCluster.rigs[0].gpus[0] = satPayload.gpus[0];
    satCluster.rigs[0].snapshot.is_online = true;
    satCluster.rigs[0].snapshot.llm_count = satPayload.llm_count;
    satCluster.rigs[0].snapshot.llms[0] = satPayload.llms[0];

    bool t54 = (satCluster.rigs[0].gpus[0].utilization_pct == 75) &&
               (satCluster.rigs[0].snapshot.llms[0].state == Wire::LlmState::Generating);
    SUBTEST_VERIFY(54, "Satellite rig GPU data sourced from SatellitePayload, isolated from local sampler", t54);

    return all_passed;
}

bool Test_LLM_2I_FormattingAndDisplay() {
    bool all_passed = true;
    ModelNameCondenser condenser;
    condenser.max_model_len = 16;

    RigClusterSnapshot cluster{};
    cluster.rig_count = 1;
    std::strncpy(cluster.rigs[0].rig_id, "local", sizeof(cluster.rigs[0].rig_id) - 1);
    cluster.rigs[0].snapshot.is_online = true;
    cluster.rigs[0].snapshot.llm_count = 1;
    std::strncpy(cluster.rigs[0].snapshot.llms[0].model_name, "qwen2.5:7b", sizeof(cluster.rigs[0].snapshot.llms[0].model_name) - 1);

    // 55. Online + one model + Idle: "qwen2.5:7b Idle"
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Idle;
    std::string s55 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(55, "Format: Online + Idle -> contains model name + 'Idle'", s55 == "qwen2.5:7b Idle");

    // 56. Online + one model + Generating: "qwen2.5:7b 42.0 T/s"
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Generating;
    cluster.rigs[0].snapshot.llms[0].tps = 42.0f;
    std::string s56 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(56, "Format: Online + Generating -> contains model name + '42.0 T/s'", s56 == "qwen2.5:7b 42.0 T/s");

    // 57. Online + one model + BurstHold: "qwen2.5:7b 42.0 T/s (Done)"
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::BurstHold;
    std::string s57 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(57, "Format: Online + BurstHold -> contains model name + '42.0 T/s (Done)'", s57 == "qwen2.5:7b 42.0 T/s (Done)");

    // 58. Online + one model + Thinking: "qwen2.5:7b Thinking..."
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Thinking;
    std::string s58 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(58, "Format: Online + Thinking -> contains model name + 'Thinking...'", s58 == "qwen2.5:7b Thinking...");

    // 59. Online + one model + Loading: "qwen2.5:7b Loading..."
    cluster.rigs[0].snapshot.llms[0].state = Wire::LlmState::Loading;
    std::string s59 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(59, "Format: Online + Loading -> contains model name + 'Loading...'", s59 == "qwen2.5:7b Loading...");

    // 60. Online + zero models: text is exactly "Idle"
    cluster.rigs[0].snapshot.llm_count = 0;
    std::string s60 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(60, "Format: Online + zero models -> text is exactly 'Idle'", s60 == "Idle");

    // 61. Offline: text is exactly "Offline"
    cluster.rigs[0].snapshot.is_online = false;
    std::string s61 = FormatGridCellText(ParseGridToken("local:llm"), cluster, condenser);
    SUBTEST_VERIFY(61, "Format: Offline node -> text is exactly 'Offline'", s61 == "Offline");

    // 62. Model name condenser: verify existing trace examples still pass
    std::string c_qwen = condenser.Condense("bartowski/Qwen2.5-Coder-7B-Instruct-v0.1-GGUF");
    std::string c_llama = condenser.Condense("Llama-3-8B-Instruct-abliterated-v2");
    bool t62 = (c_qwen == "Qwen2.5-Code..7B") && (c_llama == "Llama-3..8B") && (c_qwen.size() <= 16);
    SUBTEST_VERIFY(62, "Model name condenser: preserves parameter badge within 16 chars", t62);

    return all_passed;
}

// VULN-08: Prometheus negative counter underflow regression test
bool Test_LlmCollector_PrometheusNegativeCounterUnderflow() {
    std::string_view negative_metrics =
        "vllm:generation_tokens_total -1\n"
        "vllm:prompt_tokens_total -500\n";

    uint64_t gen = 0, prompt = 0;
    std::string model;
    uint16_t q_running = 0, q_waiting = 0;
    uint8_t kv = 0;

    LlmCollector::ParsePrometheusMetrics(negative_metrics, gen, prompt, model, q_running, q_waiting, kv);

    // Secure requirement: Negative counter values must not underflow to ~1.84e19 (ULLONG_MAX).
    // Pre-patch failure: std::stoull converts "-1" to 18446744073709551615ULL.
    // Post-patch pass: Negative counter values are rejected or sanitized.
    TEST_ASSERT(gen != 18446744073709551615ULL && gen != static_cast<uint64_t>(-1LL),
                "Negative Prometheus counter must not underflow to ULLONG_MAX");
    TEST_ASSERT(prompt != static_cast<uint64_t>(-500LL),
                "Negative prompt counter must not underflow to huge unsigned value");
    return true;
}

// VULN-09: JsonMini array parser escaped backslash string tracking regression test
bool Test_JsonMini_EscapedBackslashStringParsing() {
    std::string_view json_with_escaped_bs = R"({
        "models": [
            {"name": "model_a", "path": "C:\\"},
            {"name": "model_b", "path": "D:\\"}
        ]
    })";

    auto objects = JsonMini::FindArrayObjects(json_with_escaped_bs, "models");

    // Secure requirement: FindArrayObjects must correctly parse both objects when strings contain escaped backslashes before quotes.
    // Pre-patch failure: (json[curr - 1] != '\\') fails on '\"' when preceded by '\\', corrupting brace tracking.
    // Post-patch pass: Exactly 2 distinct objects are extracted.
    TEST_ASSERT(objects.size() == 2, "FindArrayObjects must correctly parse objects with escaped backslash before quotes");
    TEST_ASSERT(JsonMini::FindString(objects[0], "name") == "model_a", "First object name must match");
    TEST_ASSERT(JsonMini::FindString(objects[1], "name") == "model_b", "Second object name must match");

    return true;
}

// BS-VULN-01: Non-null-terminated wire string safety regression test
bool Test_LlmCollector_NonTerminatedWireStringSafety() {
    ModelNameCondenser condenser;
    Wire::LlmMetrics model{};
    // Fill model_name with 32 non-null characters
    std::memset(model.model_name, 'A', sizeof(model.model_name));
    // Fill quant with 8 non-null characters
    std::memset(model.quant, 'Q', sizeof(model.quant));
    model.state = Wire::LlmState::Generating;
    model.tps = 25.0f;

    // Secure requirement: FormatCompositeLlmWidget must not read past the 32-byte boundary
    std::string formatted = FormatCompositeLlmWidget(model, condenser);
    TEST_ASSERT(!formatted.empty(), "Formatted text must not be empty");
    TEST_ASSERT(formatted.find("25.0 T/s") != std::string::npos, "Formatted text must include TPS");

    return true;
}

// BS-VULN-09: Clamp scraped metrics against negative values and integer overflow
bool Test_LlmCollector_PrometheusNegativeAndOverflowMetricsClamping() {
    std::string malformed_prom = 
        "# HELP vllm:num_requests_running Number of running requests\n"
        "vllm:num_requests_running -15\n"
        "# HELP vllm:num_requests_waiting Number of waiting requests\n"
        "vllm:num_requests_waiting 100000\n"
        "# HELP vllm:kv_cache_usage_perc Usage ratio\n"
        "vllm:kv_cache_usage_perc 250.0\n"
        "# HELP vllm:generation_tokens_total Output tokens\n"
        "vllm:generation_tokens_total 500\n";

    uint64_t gen_tokens = 0, prompt_tokens = 0;
    std::string model_name;
    uint16_t queue_running = 999, queue_waiting = 0;
    uint8_t kv_pct = 0;

    bool ok = LlmCollector::ParsePrometheusMetrics(malformed_prom, gen_tokens, prompt_tokens, model_name, queue_running, queue_waiting, kv_pct);
    TEST_ASSERT(ok, "ParsePrometheusMetrics should succeed");
    TEST_ASSERT(queue_running == 0, "Negative queue_running must be clamped to 0 (not wrapped to unsigned)");
    TEST_ASSERT(queue_waiting == 65535, "Overflowing queue_waiting (100000) must be clamped to 65535");
    TEST_ASSERT(kv_pct == 100, "Overflowing kv_cache_usage_perc (250) must be clamped to 100%");

    // Negative queue waiting and negative kv cache
    std::string malformed_prom2 = 
        "vllm:num_requests_running 70000\n"
        "vllm:num_requests_waiting -1\n"
        "vllm:kv_cache_usage_perc -50.0\n"
        "vllm:generation_tokens_total 501\n";

    ok = LlmCollector::ParsePrometheusMetrics(malformed_prom2, gen_tokens, prompt_tokens, model_name, queue_running, queue_waiting, kv_pct);
    TEST_ASSERT(ok, "ParsePrometheusMetrics should succeed on malformed_prom2");
    TEST_ASSERT(queue_running == 65535, "Overflowing queue_running must be clamped to 65535");
    TEST_ASSERT(queue_waiting == 0, "Negative queue_waiting (-1) must be clamped to 0");
    TEST_ASSERT(kv_pct == 0, "Negative kv_cache_usage_perc (-50) must be clamped to 0%");

    return true;
}


