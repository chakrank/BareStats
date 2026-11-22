#pragma once

#include <string>
#include <string_view>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <vector>
#include "metrics/wire_protocol.h"

namespace BareStats {

enum class LlmAdapter : uint8_t {
    AutoDetect = 0,
    LlamaCpp,     // GET /slots, /props (llama-server)
    Prometheus,   // GET /metrics (vLLM, SGLang, TGI, llama-server with --metrics)
    Ollama,       // GET /api/ps
    OpenAiCompat  // GET /v1/models (LM Studio, LocalAI, TabbyAPI, Aphrodite)
};

// Fixed capacity trivially copyable node snapshot
struct LlmSnapshot {
    bool              is_online = false;
    uint8_t           llm_count = 0;
    uint8_t           _reserved[2] = {0, 0};
    Wire::LlmMetrics  llms[Wire::WIRE_MAX_MODELS]; // Max 4 co-resident models
};
static_assert(std::is_trivially_copyable_v<LlmSnapshot>, "LlmSnapshot must be trivially copyable!");

class LlmCollector {
public:
    LlmCollector(
        const std::string& rig_id,
        const std::string& base_url,
        LlmAdapter forced_adapter = LlmAdapter::AutoDetect
    );

    // Main polling update tick (invoked by background worker)
    void Update(uint8_t current_gpu_compute_pct = 0);

    // Thread-safe lock-free snapshot access
    LlmSnapshot GetSnapshot() const;

    std::string GetRigId() const { return rig_id_; }
    std::string GetBaseUrl() const { return base_url_; }
    LlmAdapter  GetResolvedAdapter() const { return resolved_adapter_; }
    bool        IsOnline() const;

    // Configurable tunables
    uint32_t timeout_ms = 300;
    uint32_t poll_interval_ms = 1000;
    uint32_t retry_interval_ms = 10000;
    uint32_t decay_hold_sec = 3;
    bool filter_embeddings = true;
    uint32_t min_vram_mb = 500;

    // Direct parser helpers (exposed for testing with synthetic payloads)
    static bool ParsePrometheusMetrics(
        std::string_view body,
        uint64_t& out_gen_tokens,
        uint64_t& out_prompt_tokens,
        std::string& out_model_name,
        uint16_t& out_queue_running,
        uint16_t& out_queue_waiting,
        uint8_t& out_kv_pct
    );

    static bool ParseOllamaPs(
        std::string_view body,
        LlmSnapshot& out_snapshot,
        uint8_t current_gpu_compute_pct = 0,
        bool filter_embeddings = false,
        uint32_t min_vram_mb = 500
    );

    static bool ParseLlamaCppSlots(
        std::string_view body,
        uint32_t& out_active_slots,
        uint64_t& out_decoded_tokens,
        std::string& out_model_name
    );

    static bool ParseOpenAiModels(
        std::string_view body,
        std::string& out_model_id
    );

private:
    std::string rig_id_;
    std::string base_url_;
    LlmAdapter  configured_adapter_;
    LlmAdapter  resolved_adapter_;

    // Seqlock state publishing
    mutable std::mutex  write_mutex_;
    std::atomic<size_t> seq_{0};
    LlmSnapshot         current_snapshot_{};

    // Resilience & Backoff State Machine
    uint8_t consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point next_poll_time_;

    // Burst Hold State Tracker
    struct BurstHoldState {
        float last_tps = 0.0f;
        std::chrono::steady_clock::time_point finish_time;
        bool is_holding = false;
    } burst_hold_;

    // Prometheus Delta Tracking State
    struct PrometheusState {
        uint64_t last_gen_tokens = 0;
        uint64_t last_prompt_tokens = 0;
        std::chrono::steady_clock::time_point last_tick_time;
        std::string last_model_id;
        bool initialized = false;
    } prom_state_;

    // llama.cpp Delta Tracking State
    struct LlamaCppState {
        uint64_t last_decoded_tokens = 0;
        std::chrono::steady_clock::time_point last_tick_time;
        std::string cached_model_name;
        bool initialized = false;
    } llama_state_;

    // Internal sub-routines
    bool AutoNegotiateAdapter();
    void HandleFailure();
    void PublishSnapshot(bool is_online, const LlmSnapshot& snapshot);

    bool PollLlamaCpp(LlmSnapshot& out_snapshot);
    bool PollPrometheus(LlmSnapshot& out_snapshot);
    bool PollOllama(LlmSnapshot& out_snapshot, uint8_t gpu_compute_pct);
    bool PollOpenAiCompat(LlmSnapshot& out_snapshot, uint8_t gpu_compute_pct);

    int FetchHttp(const std::string& path, std::string& out_response);

    friend struct LlmCollectorTestAccess;
};

struct LlmCollectorTestAccess {
    static bool AutoNegotiateAdapter(LlmCollector& c) { return c.AutoNegotiateAdapter(); }
    static bool PollOllama(LlmCollector& c, LlmSnapshot& s, uint8_t gpu) { return c.PollOllama(s, gpu); }
    static bool PollLlamaCpp(LlmCollector& c, LlmSnapshot& s) { return c.PollLlamaCpp(s); }
    static bool PollPrometheus(LlmCollector& c, LlmSnapshot& s) { return c.PollPrometheus(s); }
    static bool PollOpenAiCompat(LlmCollector& c, LlmSnapshot& s, uint8_t gpu) { return c.PollOpenAiCompat(s, gpu); }
    static void HandleFailure(LlmCollector& c) { c.HandleFailure(); }
    static uint8_t GetConsecutiveFailures(const LlmCollector& c) { return c.consecutive_failures_; }
    static void SetConsecutiveFailures(LlmCollector& c, uint8_t val) { c.consecutive_failures_ = val; }
    static std::chrono::steady_clock::time_point GetNextPollTime(const LlmCollector& c) { return c.next_poll_time_; }
    static void SetNextPollTime(LlmCollector& c, std::chrono::steady_clock::time_point tp) { c.next_poll_time_ = tp; }
    static void SetResolvedAdapter(LlmCollector& c, LlmAdapter a) { c.resolved_adapter_ = a; }
    static void PublishSnapshot(LlmCollector& c, bool online, const LlmSnapshot& s) { c.PublishSnapshot(online, s); }
};

} // namespace BareStats
