#include "llm_collector.h"
#include "model_name_condenser.h"
#include "util/http_client.h"
#include "util/json_mini.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <sstream>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace BareStats {

LlmCollector::LlmCollector(
    const std::string& rig_id,
    const std::string& base_url,
    LlmAdapter forced_adapter
)
    : rig_id_(rig_id),
      base_url_(base_url),
      configured_adapter_(forced_adapter),
      resolved_adapter_(forced_adapter)
{
    // Clean trailing slashes from base_url_
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

int LlmCollector::FetchHttp(const std::string& path, std::string& out_response) {
    std::string full_url = base_url_ + (path.empty() || path.front() != '/' ? "/" + path : path);
    HttpResponse resp = HttpClient::Get(full_url, timeout_ms, 65536);
    out_response = std::move(resp.body);
    return resp.status_code;
}

bool LlmCollector::IsOnline() const {
    return GetSnapshot().is_online;
}

void LlmCollector::PublishSnapshot(bool is_online, const LlmSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    size_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    current_snapshot_ = snapshot;
    current_snapshot_.is_online = is_online;

    std::atomic_thread_fence(std::memory_order_release);
    seq_.store(s + 2, std::memory_order_release);
}

LlmSnapshot LlmCollector::GetSnapshot() const {
    LlmSnapshot copy{};
    while (true) {
        size_t seq0 = seq_.load(std::memory_order_acquire);
        if (seq0 & 1) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
            __yield();
#else
            __asm__ volatile("yield");
#endif
#endif
            continue;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        copy = current_snapshot_;
        std::atomic_thread_fence(std::memory_order_acquire);
        size_t seq1 = seq_.load(std::memory_order_acquire);

        if (seq0 == seq1) {
            return copy;
        }
    }
}

bool LlmCollector::AutoNegotiateAdapter() {
    std::string response;

    // 1. Probe llama.cpp slots
    int status = FetchHttp("/slots", response);
    if (status == 200) {
        resolved_adapter_ = LlmAdapter::LlamaCpp;
        return true;
    }
    if (status == -1) {
        // Host/port is unreachable (connection refused or timeout); abort remaining probes on this tick
        return false;
    }

    // 2. Probe llama.cpp props
    status = FetchHttp("/props", response);
    if (status == 200) {
        resolved_adapter_ = LlmAdapter::LlamaCpp;
        return true;
    }
    if (status == -1) return false;

    // 3. Probe Prometheus (/metrics) - vLLM, SGLang, TGI, llama-server
    status = FetchHttp("/metrics", response);
    if (status == 200 || (status == 503 && response.find("Loading") != std::string::npos)) {
        resolved_adapter_ = LlmAdapter::Prometheus;
        return true;
    }
    if (status == -1) return false;

    // 4. Probe Ollama active models (/api/ps)
    status = FetchHttp("/api/ps", response);
    if (status == 200) {
        resolved_adapter_ = LlmAdapter::Ollama;
        return true;
    }
    if (status == -1) return false;

    // 5. Probe OpenAI-compatible catalog (/v1/models)
    status = FetchHttp("/v1/models", response);
    if (status == 200) {
        resolved_adapter_ = LlmAdapter::OpenAiCompat;
        return true;
    }

    return false;
}

void LlmCollector::HandleFailure() {
    consecutive_failures_++;

    if (consecutive_failures_ >= 3) {
        // Strike 3: declare offline and back off to retry interval (10s)
        resolved_adapter_ = configured_adapter_; // Re-sniff on recovery
        next_poll_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_interval_ms);

        LlmSnapshot dead{};
        dead.is_online = false;
        PublishSnapshot(false, dead);
    } else {
        // Transient jitter (strike 1 or 2): retain previous snapshot to prevent flicker
        next_poll_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(poll_interval_ms);
    }
}

void LlmCollector::Update(uint8_t current_gpu_compute_pct) {
    auto now = std::chrono::steady_clock::now();
    if (now < next_poll_time_) return;

    // 1. Auto-negotiate adapter if needed
    if (resolved_adapter_ == LlmAdapter::AutoDetect) {
        if (!AutoNegotiateAdapter()) {
            HandleFailure();
            return;
        }
    }

    // 2. Dispatch scraper based on resolved adapter
    LlmSnapshot new_snapshot{};
    bool success = false;

    switch (resolved_adapter_) {
        case LlmAdapter::Prometheus:
            success = PollPrometheus(new_snapshot);
            break;
        case LlmAdapter::Ollama:
            success = PollOllama(new_snapshot, current_gpu_compute_pct);
            break;
        case LlmAdapter::LlamaCpp:
            success = PollLlamaCpp(new_snapshot);
            break;
        case LlmAdapter::OpenAiCompat:
            success = PollOpenAiCompat(new_snapshot, current_gpu_compute_pct);
            break;
        default:
            break;
    }

    // 3. 3-Strike Degradation Logic
    if (success) {
        consecutive_failures_ = 0;
        next_poll_time_ = now + std::chrono::milliseconds(poll_interval_ms);
        PublishSnapshot(true, new_snapshot);
    } else {
        HandleFailure();
    }
}

// Prometheus parser helper across vLLM, SGLang, TGI, llama-server
bool LlmCollector::ParsePrometheusMetrics(
    std::string_view body,
    uint64_t& out_gen_tokens,
    uint64_t& out_prompt_tokens,
    std::string& out_model_name,
    uint16_t& out_queue_running,
    uint16_t& out_queue_waiting,
    uint8_t& out_kv_pct
) {
    out_gen_tokens = 0;
    out_prompt_tokens = 0;
    out_model_name.clear();
    out_queue_running = 0;
    out_queue_waiting = 0;
    out_kv_pct = 0;

    bool found_gen = false;
    bool found_prompt = false;

    std::istringstream stream{std::string(body)};
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty() || line.front() == '#') continue;

        // Extract metric name (up to '{' or space)
        size_t name_end = line.find_first_of(" {");
        if (name_end == std::string::npos) continue;

        std::string_view m_name(line.data(), name_end);

        // Extract value (after last space)
        size_t last_space = line.rfind(' ');
        if (last_space == std::string::npos || last_space + 1 >= line.size()) continue;

        std::string_view val_str(line.data() + last_space + 1, line.size() - (last_space + 1));

        // Model name label extraction from curly braces
        if (out_model_name.empty()) {
            size_t brace_open = line.find('{');
            size_t brace_close = line.find('}', brace_open);
            if (brace_open != std::string::npos && brace_close != std::string::npos) {
                std::string_view labels(line.data() + brace_open, brace_close - brace_open);
                size_t m_pos = labels.find("model_name=\"");
                if (m_pos == std::string_view::npos) {
                    m_pos = labels.find("model=\"");
                }
                if (m_pos != std::string_view::npos) {
                    size_t quote_start = labels.find('"', m_pos);
                    if (quote_start != std::string_view::npos) {
                        size_t quote_end = labels.find('"', quote_start + 1);
                        if (quote_end != std::string_view::npos) {
                            out_model_name = std::string(labels.substr(quote_start + 1, quote_end - quote_start - 1));
                        }
                    }
                }
            }
        }

        // 1. Generation tokens counter
        if (m_name == "vllm:generation_tokens_total" ||
            m_name == "sglang:generation_tokens_total" ||
            m_name == "tgi_request_generated_tokens_sum" ||
            m_name == "llamacpp:tokens_predicted_total") {
            try {
                if (!val_str.empty() && val_str[0] != '-' && val_str.find('-') == std::string_view::npos) {
                    double d = std::stod(std::string(val_str));
                    if (!std::isnan(d) && !std::isinf(d) && d >= 0.0) {
                        out_gen_tokens = static_cast<uint64_t>(d);
                        found_gen = true;
                    }
                }
            } catch (...) {}
        }
        // 2. Prompt tokens counter
        else if (m_name == "vllm:prompt_tokens_total" ||
                 m_name == "sglang:prompt_tokens_total" ||
                 m_name == "tgi_request_prompt_tokens_sum" ||
                 m_name == "llamacpp:prompt_tokens_total") {
            try {
                if (!val_str.empty() && val_str[0] != '-' && val_str.find('-') == std::string_view::npos) {
                    double d = std::stod(std::string(val_str));
                    if (!std::isnan(d) && !std::isinf(d) && d >= 0.0) {
                        out_prompt_tokens = static_cast<uint64_t>(d);
                        found_prompt = true;
                    }
                }
            } catch (...) {}
        }
        // 3. Queue running
        else if (m_name == "vllm:num_requests_running" ||
                 m_name == "sglang:num_requests_running" ||
                 m_name == "tgi_queue_size") {
            try {
                if (!val_str.empty() && val_str[0] != '-' && val_str.find('-') == std::string_view::npos) {
                    double d = std::stod(std::string(val_str));
                    if (!std::isnan(d) && !std::isinf(d) && d >= 0.0) {
                        out_queue_running = static_cast<uint16_t>(std::clamp(d, 0.0, 65535.0));
                    } else {
                        out_queue_running = 0;
                    }
                } else {
                    out_queue_running = 0;
                }
            } catch (...) {
                out_queue_running = 0;
            }
        }
        // 4. Queue waiting
        else if (m_name == "vllm:num_requests_waiting" ||
                 m_name == "sglang:num_requests_waiting") {
            try {
                if (!val_str.empty() && val_str[0] != '-' && val_str.find('-') == std::string_view::npos) {
                    double d = std::stod(std::string(val_str));
                    if (!std::isnan(d) && !std::isinf(d) && d >= 0.0) {
                        out_queue_waiting = static_cast<uint16_t>(std::clamp(d, 0.0, 65535.0));
                    } else {
                        out_queue_waiting = 0;
                    }
                } else {
                    out_queue_waiting = 0;
                }
            } catch (...) {
                out_queue_waiting = 0;
            }
        }
        // 5. KV cache usage
        else if (m_name == "vllm:kv_cache_usage_perc" ||
                 m_name == "vllm:gpu_cache_usage_perc" ||
                 m_name == "sglang:token_usage" ||
                 m_name == "llamacpp:kv_cache_usage_ratio") {
            try {
                float val = std::stof(std::string(val_str));
                if (!std::isnan(val) && !std::isinf(val)) {
                    if (val <= 1.0f && val > 0.0f) val *= 100.0f;
                    out_kv_pct = static_cast<uint8_t>(std::clamp(val, 0.0f, 100.0f));
                }
            } catch (...) {}
        }
    }

    return found_gen || found_prompt || !out_model_name.empty();
}

bool LlmCollector::PollPrometheus(LlmSnapshot& out_snapshot) {
    std::string response;
    int status = FetchHttp("/metrics", response);

    if (status != 200) {
        if (status == 503 && response.find("Loading") != std::string::npos) {
            out_snapshot.is_online = true;
            out_snapshot.llm_count = 1;
            std::memset(&out_snapshot.llms[0], 0, sizeof(Wire::LlmMetrics));
            std::strncpy(out_snapshot.llms[0].model_name, "Loading...", sizeof(out_snapshot.llms[0].model_name) - 1);
            out_snapshot.llms[0].state = Wire::LlmState::Loading;
            return true; // Valid 503 Loading state; do not trigger failure strike
        }
        return false;
    }

    uint64_t gen_tokens = 0;
    uint64_t prompt_tokens = 0;
    std::string model_name;
    uint16_t queue_running = 0;
    uint16_t queue_waiting = 0;
    uint8_t kv_pct = 0;

    if (!ParsePrometheusMetrics(response, gen_tokens, prompt_tokens, model_name, queue_running, queue_waiting, kv_pct)) {
        return false;
    }

    if (model_name.empty()) {
        model_name = prom_state_.last_model_id.empty() ? rig_id_ : prom_state_.last_model_id;
    }

    auto now = std::chrono::steady_clock::now();
    float live_tps = 0.0f;
    Wire::LlmState detected_state = Wire::LlmState::Idle;

    if (prom_state_.initialized && model_name == prom_state_.last_model_id && gen_tokens >= prom_state_.last_gen_tokens) {
        std::chrono::duration<float> delta_sec = now - prom_state_.last_tick_time;
        uint64_t delta_gen = gen_tokens - prom_state_.last_gen_tokens;
        uint64_t delta_prompt = prompt_tokens >= prom_state_.last_prompt_tokens ? (prompt_tokens - prom_state_.last_prompt_tokens) : 0;

        if (delta_sec.count() > 0.05f) {
            live_tps = std::clamp(static_cast<float>(delta_gen) / delta_sec.count(), 0.0f, 10000.0f);
        }

        if (delta_prompt > 0 && delta_gen == 0) {
            detected_state = Wire::LlmState::Thinking;
            burst_hold_.is_holding = false;
        } else if (delta_gen > 0) {
            detected_state = Wire::LlmState::Generating;
            burst_hold_.last_tps = live_tps;
            burst_hold_.finish_time = now;
            burst_hold_.is_holding = true;
        } else if (queue_running > 0) {
            detected_state = Wire::LlmState::Generating;
            burst_hold_.is_holding = false;
        } else if (burst_hold_.is_holding) {
            auto hold_sec = std::chrono::duration_cast<std::chrono::seconds>(now - burst_hold_.finish_time).count();
            if (hold_sec < static_cast<int64_t>(decay_hold_sec)) {
                detected_state = Wire::LlmState::BurstHold;
                live_tps = burst_hold_.last_tps;
            } else {
                burst_hold_.is_holding = false;
                detected_state = Wire::LlmState::Idle;
            }
        }
    } else {
        // Initial scrape tick or model changed
        if (queue_running > 0) {
            detected_state = Wire::LlmState::Generating;
        } else {
            detected_state = Wire::LlmState::Idle;
        }
    }

    prom_state_.last_gen_tokens = gen_tokens;
    prom_state_.last_prompt_tokens = prompt_tokens;
    prom_state_.last_tick_time = now;
    prom_state_.last_model_id = model_name;
    prom_state_.initialized = true;

    out_snapshot.is_online = true;
    out_snapshot.llm_count = 1;
    std::memset(&out_snapshot.llms[0], 0, sizeof(Wire::LlmMetrics));
    std::strncpy(out_snapshot.llms[0].model_name, model_name.c_str(), sizeof(out_snapshot.llms[0].model_name) - 1);
    out_snapshot.llms[0].state = detected_state;
    out_snapshot.llms[0].tps = live_tps;
    out_snapshot.llms[0].kv_cache_pct = kv_pct;
    out_snapshot.llms[0].queue_running = queue_running;
    out_snapshot.llms[0].queue_waiting = queue_waiting;

    return true;
}

bool LlmCollector::ParseOllamaPs(
    std::string_view body,
    LlmSnapshot& out_snapshot,
    uint8_t current_gpu_compute_pct,
    bool filter_embeddings,
    uint32_t min_vram_mb
) {
    auto model_objs = JsonMini::FindArrayObjects(body, "models");
    out_snapshot.is_online = true;
    out_snapshot.llm_count = 0;

    for (size_t i = 0; i < model_objs.size(); ++i) {
        std::string_view m_name = JsonMini::FindString(model_objs[i], "name");
        int64_t vram_bytes = JsonMini::FindInt(model_objs[i], "size_vram", 0);
        if (vram_bytes < 0) vram_bytes = 0;

        if (filter_embeddings && ModelNameCondenser::IsEmbeddingModel(std::string(m_name), static_cast<uint64_t>(vram_bytes), min_vram_mb)) {
            continue;
        }
        if (out_snapshot.llm_count >= Wire::WIRE_MAX_MODELS) break;

        uint8_t idx = out_snapshot.llm_count++;
        std::memset(&out_snapshot.llms[idx], 0, sizeof(Wire::LlmMetrics));

        std::string_view m_quant = JsonMini::FindString(model_objs[i], "quantization_level");

        std::strncpy(out_snapshot.llms[idx].model_name, std::string(m_name).c_str(), sizeof(out_snapshot.llms[idx].model_name) - 1);
        std::strncpy(out_snapshot.llms[idx].quant, std::string(m_quant).c_str(), sizeof(out_snapshot.llms[idx].quant) - 1);
        out_snapshot.llms[idx].vram_used_mb = static_cast<uint32_t>(vram_bytes / (1024 * 1024));
        out_snapshot.llms[idx].tps = 0.0f; // TPS suppressed for Ollama Direct Pull

        // Hardware correlation: only primary model claims active generation on GPU compute spike
        if (idx == 0 && current_gpu_compute_pct > 15) {
            out_snapshot.llms[idx].state = Wire::LlmState::Generating;
        } else {
            out_snapshot.llms[idx].state = Wire::LlmState::Idle;
        }
    }

    return true;
}

bool LlmCollector::PollOllama(LlmSnapshot& out_snapshot, uint8_t gpu_compute_pct) {
    std::string response;
    int status = FetchHttp("/api/ps", response);
    if (status != 200) return false;

    return ParseOllamaPs(response, out_snapshot, gpu_compute_pct, filter_embeddings, min_vram_mb);
}

bool LlmCollector::ParseLlamaCppSlots(
    std::string_view body,
    uint32_t& out_active_slots,
    uint64_t& out_decoded_tokens,
    std::string& out_model_name
) {
    out_active_slots = 0;
    out_decoded_tokens = 0;

    auto slots = JsonMini::FindArrayObjects(body);
    for (const auto& s : slots) {
        int64_t state = JsonMini::FindInt(s, "state", 0);
        int64_t n_dec = JsonMini::FindInt(s, "n_decoded", 0);
        std::string_view m = JsonMini::FindString(s, "model");

        if (state == 1) { // 1 = Processing / Decoding
            out_active_slots++;
        }
        if (n_dec > 0) {
            out_decoded_tokens += static_cast<uint64_t>(n_dec);
        }
        if (out_model_name.empty() && !m.empty()) {
            out_model_name = std::string(m);
        }
    }

    return !slots.empty();
}

bool LlmCollector::PollLlamaCpp(LlmSnapshot& out_snapshot) {
    std::string response;
    int status = FetchHttp("/slots", response);

    uint32_t active_slots = 0;
    uint64_t decoded_tokens = 0;
    std::string model_name;

    bool slots_parsed = (status == 200) && ParseLlamaCppSlots(response, active_slots, decoded_tokens, model_name);

    if (!slots_parsed) {
        // Fallback to /props or /v1/models if /slots is disabled or unconfigured in llama-server
        std::string props_resp;
        int props_status = FetchHttp("/props", props_resp);
        if (props_status == 200) {
            std::string_view m = JsonMini::FindString(props_resp, "model");
            if (!m.empty()) {
                model_name = std::string(m);
            }
        } else {
            std::string models_resp;
            int models_status = FetchHttp("/v1/models", models_resp);
            if (models_status == 200) {
                auto model_objs = JsonMini::FindArrayObjects(models_resp, "data");
                if (!model_objs.empty()) {
                    std::string_view id = JsonMini::FindString(model_objs[0], "id");
                    if (!id.empty()) model_name = std::string(id);
                }
            } else {
                return false;
            }
        }
    }

    // If model name not in /slots, query /props or use cached name
    if (model_name.empty()) {
        if (llama_state_.cached_model_name.empty()) {
            std::string props_resp;
            if (FetchHttp("/props", props_resp) == 200) {
                std::string_view m = JsonMini::FindString(props_resp, "model");
                if (!m.empty()) {
                    llama_state_.cached_model_name = std::string(m);
                }
            }
        }
        model_name = !llama_state_.cached_model_name.empty() ? llama_state_.cached_model_name : rig_id_;
    } else {
        llama_state_.cached_model_name = model_name;
    }

    auto now = std::chrono::steady_clock::now();
    float live_tps = 0.0f;
    Wire::LlmState detected_state = Wire::LlmState::Idle;

    if (llama_state_.initialized && decoded_tokens >= llama_state_.last_decoded_tokens) {
        std::chrono::duration<float> delta_sec = now - llama_state_.last_tick_time;
        uint64_t delta_tokens = decoded_tokens - llama_state_.last_decoded_tokens;

        if (delta_sec.count() > 0.05f) {
            float calculated = static_cast<float>(delta_tokens) / delta_sec.count();
            if (std::isnan(calculated) || std::isinf(calculated)) {
                live_tps = 0.0f;
            } else {
                live_tps = std::clamp(calculated, 0.0f, 10000.0f);
            }
        }

        if (delta_tokens > 0 || active_slots > 0) {
            detected_state = (delta_tokens > 0) ? Wire::LlmState::Generating : Wire::LlmState::Thinking;
            if (delta_tokens > 0) {
                burst_hold_.last_tps = live_tps;
                burst_hold_.finish_time = now;
                burst_hold_.is_holding = true;
            }
        } else if (burst_hold_.is_holding) {
            auto hold_sec = std::chrono::duration_cast<std::chrono::seconds>(now - burst_hold_.finish_time).count();
            if (hold_sec < static_cast<int64_t>(decay_hold_sec)) {
                detected_state = Wire::LlmState::BurstHold;
                live_tps = burst_hold_.last_tps;
            } else {
                burst_hold_.is_holding = false;
                detected_state = Wire::LlmState::Idle;
            }
        }
    }

    llama_state_.last_decoded_tokens = decoded_tokens;
    llama_state_.last_tick_time = now;
    llama_state_.initialized = true;

    out_snapshot.is_online = true;
    out_snapshot.llm_count = 1;
    std::memset(&out_snapshot.llms[0], 0, sizeof(Wire::LlmMetrics));
    std::strncpy(out_snapshot.llms[0].model_name, model_name.c_str(), sizeof(out_snapshot.llms[0].model_name) - 1);
    out_snapshot.llms[0].state = detected_state;
    out_snapshot.llms[0].tps = live_tps;
    out_snapshot.llms[0].queue_running = static_cast<uint16_t>(std::clamp(active_slots, 0U, 65535U));

    return true;
}

bool LlmCollector::ParseOpenAiModels(
    std::string_view body,
    std::string& out_model_id
) {
    auto data_objs = JsonMini::FindArrayObjects(body, "data");
    if (data_objs.empty()) return false;

    std::string_view m_id = JsonMini::FindString(data_objs[0], "id");
    if (m_id.empty()) return false;

    out_model_id = std::string(m_id);
    return true;
}

bool LlmCollector::PollOpenAiCompat(LlmSnapshot& out_snapshot, uint8_t gpu_compute_pct) {
    std::string response;
    int status = FetchHttp("/v1/models", response);
    if (status != 200) return false;

    std::string model_id;
    if (!ParseOpenAiModels(response, model_id)) return false;

    out_snapshot.is_online = true;
    out_snapshot.llm_count = 1;
    std::memset(&out_snapshot.llms[0], 0, sizeof(Wire::LlmMetrics));
    std::strncpy(out_snapshot.llms[0].model_name, model_id.c_str(), sizeof(out_snapshot.llms[0].model_name) - 1);
    out_snapshot.llms[0].state = (gpu_compute_pct > 15) ? Wire::LlmState::Generating : Wire::LlmState::Idle;
    out_snapshot.llms[0].tps = 0.0f;

    return true;
}

} // namespace BareStats
