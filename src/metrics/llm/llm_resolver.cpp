#include "llm_resolver.h"
#include <cstring>
#include <cstdio>

namespace BareStats {

const Wire::LlmMetrics* ResolveGenericLlm(const RigClusterSnapshot& cluster) {
    const Wire::LlmMetrics* best_generating_model = nullptr;
    const Wire::LlmMetrics* largest_vram_model = nullptr;
    const Wire::LlmMetrics* first_discovered_model = nullptr;

    float highest_tps = -1.0f;
    uint32_t max_vram = 0;

    uint8_t max_rigs = std::min(cluster.rig_count, static_cast<uint8_t>(8));
    for (uint8_t r = 0; r < max_rigs; ++r) {
        const auto& rig = cluster.rigs[r];
        if (!rig.snapshot.is_online || rig.snapshot.llm_count == 0) continue;

        uint8_t max_models = std::min(rig.snapshot.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
        for (uint8_t i = 0; i < max_models; ++i) {
            const auto& model = rig.snapshot.llms[i];

            // Priority 3: First discovered (Fallback)
            if (!first_discovered_model) {
                first_discovered_model = &model;
            }

            // Priority 2: Largest resident VRAM footprint
            if (model.vram_used_mb > max_vram) {
                max_vram = model.vram_used_mb;
                largest_vram_model = &model;
            }

            // Priority 1: Actively generating tokens (Highest live TPS or BurstHold or Thinking)
            if (model.state == Wire::LlmState::Generating ||
                model.state == Wire::LlmState::BurstHold ||
                model.state == Wire::LlmState::Thinking) {
                if (model.tps > highest_tps || !best_generating_model) {
                    highest_tps = model.tps;
                    best_generating_model = &model;
                }
            }
        }
    }

    if (best_generating_model) return best_generating_model;
    if (largest_vram_model) return largest_vram_model;
    return first_discovered_model; // nullptr if entire cluster is offline or empty
}

const Wire::LlmMetrics* ResolveRigModel(
    const RigClusterSnapshot& cluster,
    const std::string& rig_target,
    int model_index
) {
    uint8_t max_rigs = std::min(cluster.rig_count, static_cast<uint8_t>(8));
    if (rig_target == "any" || rig_target.empty()) {
        if (model_index >= 0) {
            for (uint8_t r = 0; r < max_rigs; ++r) {
                uint8_t max_m = std::min(cluster.rigs[r].snapshot.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
                if (cluster.rigs[r].snapshot.is_online && model_index < max_m) {
                    return &cluster.rigs[r].snapshot.llms[model_index];
                }
            }
            return nullptr;
        }
        return ResolveGenericLlm(cluster);
    }

    for (uint8_t r = 0; r < max_rigs; ++r) {
        if (RigIdEquals(cluster.rigs[r].rig_id, rig_target)) {
            const auto& snap = cluster.rigs[r].snapshot;
            if (!snap.is_online || snap.llm_count == 0) return nullptr;

            uint8_t max_models = std::min(snap.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
            if (model_index >= 0) {
                if (model_index < max_models) {
                    return &snap.llms[model_index];
                }
                return nullptr;
            }

            // model_index == -1: find active model on this rig, else largest vram, else index 0
            const Wire::LlmMetrics* active_model = nullptr;
            const Wire::LlmMetrics* max_vram_model = nullptr;
            uint32_t max_vram = 0;
            float max_tps = -1.0f;

            for (uint8_t i = 0; i < max_models; ++i) {
                const auto& m = snap.llms[i];
                if (m.state == Wire::LlmState::Generating ||
                    m.state == Wire::LlmState::BurstHold ||
                    m.state == Wire::LlmState::Thinking) {
                    if (m.tps > max_tps || !active_model) {
                        max_tps = m.tps;
                        active_model = &m;
                    }
                }
                if (m.vram_used_mb > max_vram) {
                    max_vram = m.vram_used_mb;
                    max_vram_model = &m;
                }
            }

            if (active_model) return active_model;
            if (max_vram_model) return max_vram_model;
            return &snap.llms[0];
        }
    }

    return nullptr;
}

std::string FormatGridCellText(
    const GridToken& token,
    const RigClusterSnapshot& cluster,
    const ModelNameCondenser& condenser,
    const std::string& target_model
) {
    const Wire::LlmMetrics* m = (token.rig_target == "any")
        ? ((token.model_index >= 0) ? ResolveRigModel(cluster, "any", token.model_index) : ResolveGenericLlm(cluster))
        : ResolveRigModel(cluster, token.rig_target, token.model_index);

    uint8_t max_rigs = std::min(cluster.rig_count, static_cast<uint8_t>(8));
    if (!m) {
        for (uint8_t r = 0; r < max_rigs; ++r) {
            if (token.rig_target == "any" || token.rig_target.empty() || RigIdEquals(cluster.rigs[r].rig_id, token.rig_target)) {
                if (cluster.rigs[r].snapshot.is_online) {
                    return "Idle";
                }
            }
        }
        return "Offline";
    }

    switch (token.metric) {
        case LlmMetricType::Composite: {
            if (token.model_index == -1) {
                const LlmSnapshot* matchedSnap = nullptr;
                if (token.rig_target == "any" || token.rig_target.empty()) {
                    for (uint8_t r = 0; r < max_rigs; ++r) {
                        const auto& snap = cluster.rigs[r].snapshot;
                        if (!snap.is_online || snap.llm_count == 0) continue;
                        uint8_t max_m = std::min(snap.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
                        for (uint8_t i = 0; i < max_m; ++i) {
                            if (&snap.llms[i] == m) {
                                matchedSnap = &snap;
                                break;
                            }
                        }
                        if (matchedSnap) break;
                    }
                } else {
                    for (uint8_t r = 0; r < max_rigs; ++r) {
                        if (RigIdEquals(cluster.rigs[r].rig_id, token.rig_target)) {
                            if (cluster.rigs[r].snapshot.is_online && cluster.rigs[r].snapshot.llm_count > 0) {
                                matchedSnap = &cluster.rigs[r].snapshot;
                            }
                            break;
                        }
                    }
                }
                if (matchedSnap) {
                    return FormatMultiModelWidget(*matchedSnap, condenser, target_model);
                }
            }
            return FormatCompositeLlmWidget(*m, condenser);
        }

        case LlmMetricType::ModelName: {
            size_t name_len = strnlen(m->model_name, sizeof(m->model_name));
            return condenser.Condense(std::string_view(m->model_name, name_len));
        }

        case LlmMetricType::Tps: {
            char buf[32];
            if (m->state == Wire::LlmState::Generating) {
                std::snprintf(buf, sizeof(buf), "%.1f T/s", m->tps);
                return buf;
            } else if (m->state == Wire::LlmState::BurstHold) {
                std::snprintf(buf, sizeof(buf), "%.1f T/s (Done)", m->tps);
                return buf;
            } else if (m->state == Wire::LlmState::Thinking) {
                return "Thinking...";
            } else if (m->state == Wire::LlmState::Loading) {
                return "Loading...";
            } else if (m->state == Wire::LlmState::Offline) {
                return "Offline";
            }
            return "0.0 T/s";
        }

        case LlmMetricType::KvCache:
            return "KV " + std::to_string(m->kv_cache_pct) + "%";

        case LlmMetricType::Vram: {
            char buf[32];
            if (m->vram_used_mb >= 1024) {
                std::snprintf(buf, sizeof(buf), "%.1f GB", m->vram_used_mb / 1024.0f);
            } else {
                std::snprintf(buf, sizeof(buf), "%u MB", m->vram_used_mb);
            }
            return buf;
        }

        case LlmMetricType::Queue:
            return "Q: " + std::to_string(m->queue_running) + "/" + std::to_string(m->queue_waiting);

        default:
            return FormatCompositeLlmWidget(*m, condenser);
    }
}

} // namespace BareStats
