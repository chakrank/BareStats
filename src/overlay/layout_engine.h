#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <cstdint>

namespace BareStats {

// --- Endpoint Configuration ---
struct EndpointDef {
    std::string rig_id;   // e.g. "local", "rig1", "mac_studio"
    std::string url;      // e.g. "http://192.168.1.150:8000"
};

// Parses: "endpoints = local=http://127.0.0.1:11434, rig1=http://rig1.local:8000"
// Implementation: Split by comma, then split strictly by the FIRST '=' character.
// Ignores all colons inside the HTTP protocol and port numbers.
std::vector<EndpointDef> ParseEndpoints(std::string_view config_line);

// --- Grid Token Addressing ---
enum class LlmMetricType : uint8_t {
    Composite,  // Full block: "deepseek-r1..14b 34.2 T/s"
    ModelName,  // Model identifier only: "deepseek-r1..14b"
    Tps,        // Live throughput: "34.2 T/s"
    KvCache,    // Context saturation: "KV 28%"
    Vram,       // Allocated VRAM: "9.2GB"
    Queue       // Streams & queue depth: "Q: 0/2"
};

struct GridToken {
    std::string   rig_target{"any"};   // "any", "local", "rig1"
    int           model_index{-1};     // -1 for "active generating", or 0, 1, 2, 3
    LlmMetricType metric{LlmMetricType::Composite};       // Sub-metric to draw
};

// Parses: "rig1:llm0:tps" or "local:model" or "llm" or "tps"
// Token split rule:
//   1 part  ("llm")             -> rig="any",  idx=-1, metric=Composite
//   2 parts ("rig1:tps")        -> rig="rig1", idx=-1, metric=Tps
//   3 parts ("rig1:llm0:tps")   -> rig="rig1", idx=0,  metric=Tps
GridToken ParseGridToken(std::string_view token_str);

} // namespace BareStats
