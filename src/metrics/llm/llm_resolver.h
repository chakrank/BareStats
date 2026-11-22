#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include "metrics/wire_protocol.h"
#include "metrics/llm/llm_collector.h"
#include "metrics/llm/model_name_condenser.h"
#include "overlay/layout_engine.h"

namespace BareStats {

// Fixed-capacity trivially copyable node snapshot across all active nodes
struct RigNodeSnapshot {
    char                 rig_id[16] = {0};
    uint8_t              cpu_load_pct = 0;
    uint8_t              gpu_count = 0;
    bool                 is_online = false; // Rig hardware telemetry feed is alive
    uint8_t              _pad = 0;
    uint32_t             ram_used_mb = 0;
    uint32_t             ram_total_mb = 0;
    Wire::WireGpuMetrics gpus[8] = {};
    LlmSnapshot          snapshot;          // LLM model and inference status
};

struct RigClusterSnapshot {
    uint8_t         rig_count = 0;
    uint8_t         _reserved[3] = {0, 0, 0};
    RigNodeSnapshot rigs[8]; // Max 8 rigs, zero heap allocation
};
static_assert(std::is_trivially_copyable_v<RigClusterSnapshot>, "RigClusterSnapshot must be trivially copyable!");

// Case-insensitive rig_id comparison helper
inline bool RigIdEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline bool RigIdEquals(const char* rig_id, std::string_view target) {
    size_t len = strnlen(rig_id, 16);
    return RigIdEquals(std::string_view(rig_id, len), target);
}

inline bool RigIdEquals(const char* rig_id_a, const char* rig_id_b) {
    size_t len_a = strnlen(rig_id_a, 16);
    size_t len_b = strnlen(rig_id_b, 16);
    return RigIdEquals(std::string_view(rig_id_a, len_a), std::string_view(rig_id_b, len_b));
}

// 3-Tier priority cascade for generic LLM resolution:
// 1. Actively generating tokens (Generating, BurstHold, Thinking with highest TPS)
// 2. Largest resident VRAM footprint
// 3. First discovered model fallback
const Wire::LlmMetrics* ResolveGenericLlm(const RigClusterSnapshot& cluster);

// Targeted model resolver for specific rig and optional model index
const Wire::LlmMetrics* ResolveRigModel(
    const RigClusterSnapshot& cluster,
    const std::string& rig_target,
    int model_index = -1
);

// High-level text formatter for layout engine grid cells
std::string FormatGridCellText(
    const GridToken& token,
    const RigClusterSnapshot& cluster,
    const ModelNameCondenser& condenser,
    const std::string& target_model = "active"
);

} // namespace BareStats
