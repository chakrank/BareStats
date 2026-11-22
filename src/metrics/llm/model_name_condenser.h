#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include "metrics/wire_protocol.h"
#include "metrics/llm/llm_collector.h"

namespace BareStats {

struct ParamMatch {
    size_t start = std::string::npos;
    size_t length = 0;
};

// Zero-allocation scanner for parameter badges: "7B", "14b", "70B", "8x7B", "0.5B", "1.5B", "500M"
inline ParamMatch FindLastParamBadge(const std::string& str) {
    ParamMatch best_match;
    const size_t n = str.size();
    if (n < 2) return best_match;

    // Scan for potential unit characters: 'B', 'b', 'M', 'm'
    for (size_t i = 1; i < n; ++i) {
        char c = str[i];
        if (c != 'B' && c != 'b' && c != 'M' && c != 'm') continue;

        // Boundary check after unit: must be end of string or non-alphanumeric delimiter
        if (i + 1 < n && std::isalnum(static_cast<unsigned char>(str[i + 1]))) continue;

        // Scan backwards from i - 1 to extract parameter digits
        size_t j = i;
        bool has_digits = false;
        while (j > 0 && std::isdigit(static_cast<unsigned char>(str[j - 1]))) {
            --j;
            has_digits = true;
        }
        if (!has_digits) continue;

        // Check for preceding 'x', 'X', or decimal '.' (e.g. "8x7B" or "0.5B")
        if (j > 1 && (str[j - 1] == 'x' || str[j - 1] == 'X' || str[j - 1] == '.')) {
            size_t k = j - 1;
            bool has_first_digits = false;
            while (k > 0 && std::isdigit(static_cast<unsigned char>(str[k - 1]))) {
                --k;
                has_first_digits = true;
            }
            if (has_first_digits) {
                j = k;
            }
        }

        // Boundary check before start: must be start of string or non-alphabetic
        if (j > 0 && std::isalpha(static_cast<unsigned char>(str[j - 1]))) {
            continue;
        }

        best_match.start = j;
        best_match.length = (i + 1) - j;
    }
    return best_match;
}

class ModelNameCondenser {
public:
    // Configurable parameters from [LLM] and [LLM.Aliases] in config.ini
    size_t max_model_len = 16;
    std::unordered_map<std::string, std::string> user_aliases;

    // Primary sanitization pipeline
    std::string Condense(std::string_view raw_name_sv) const {
        if (raw_name_sv.empty()) return "";
        std::string raw_name(raw_name_sv);

        // Step 1: User-Defined Exact Alias Lookup (O(1) Hash Map)
        if (auto it = user_aliases.find(raw_name); it != user_aliases.end()) {
            return it->second;
        }

        // Step 2: Strip Repository Prefix
        // e.g., "deepseek-ai/DeepSeek-R1-14B" -> "DeepSeek-R1-14B"
        size_t slash_pos = raw_name.find_last_of('/');
        if (slash_pos != std::string::npos) {
            raw_name = raw_name.substr(slash_pos + 1);
        }

        // Check alias again after stripping repo prefix
        if (auto it = user_aliases.find(raw_name); it != user_aliases.end()) {
            return it->second;
        }

        // Case-Insensitive Substring Matching for [LLM.Aliases] (GAP-04)
        if (!user_aliases.empty()) {
            std::string lower_raw = raw_name;
            for (char& c : lower_raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            const std::string* best_alias = nullptr;
            size_t best_key_len = 0;

            for (const auto& [k, v] : user_aliases) {
                std::string lower_k = k;
                for (char& c : lower_k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!lower_k.empty() && lower_raw.find(lower_k) != std::string::npos) {
                    if (lower_k.length() > best_key_len) {
                        best_key_len = lower_k.length();
                        best_alias = &v;
                    }
                }
            }
            if (best_alias) {
                return *best_alias;
            }
        }

        // Step 3: Strip Redundant Noise Tags
        const std::vector<std::string> noise_tags = {
            "-GGUF", "-gguf", ".gguf", "-hf", "-HF",
            "-Instruct", "-instruct", "-Chat", "-chat",
            ":latest"
        };
        for (const auto& tag : noise_tags) {
            size_t pos = raw_name.find(tag);
            if (pos != std::string::npos) {
                raw_name.erase(pos, tag.length());
            }
        }

        // Clean up any trailing hyphens, underscores, dots, or colons
        while (!raw_name.empty() && (raw_name.back() == '-' || raw_name.back() == '_' ||
                                     raw_name.back() == '.' || raw_name.back() == ':')) {
            raw_name.pop_back();
        }

        // Step 4: Early Exit if it fits perfectly within max_model_len
        if (raw_name.length() <= max_model_len) {
            return raw_name;
        }

        // Step 5: Parameter-Preserving Smart Truncation
        ParamMatch match = FindLastParamBadge(raw_name);

        if (match.length > 0) {
            std::string param_badge = raw_name.substr(match.start, match.length);
            size_t suffix_len = param_badge.length() + 2; // Reserve space for ".." + badge
            if (max_model_len > suffix_len) {
                size_t max_prefix_len = max_model_len - suffix_len;
                // Clamp prefix length to match.start to prevent parameter duplication
                // e.g., prevents "Llama-3-8B-abliterated-v2" from becoming "Llama-3-8B-a..8B"
                size_t prefix_len = std::min(max_prefix_len, match.start);

                std::string prefix = raw_name.substr(0, prefix_len);
                while (!prefix.empty() && (prefix.back() == '-' || prefix.back() == '_' ||
                                           prefix.back() == '.' || prefix.back() == ':')) {
                    prefix.pop_back();
                }
                return prefix + ".." + param_badge;
            }
        }

        // Step 6: Graceful Fallback Hard Truncation (If no parameter size exists)
        if (max_model_len > 3) {
            return raw_name.substr(0, max_model_len - 3) + "...";
        }
        return raw_name.substr(0, max_model_len);
    }

    // Embedding Filter helper
    // Returns true if the model is an embedding/rerank model or under min_vram_mb threshold.
    // Models without embedding keywords and with sufficient VRAM (or zero VRAM reporting) are not filtered.
    static bool IsEmbeddingModel(const std::string& name_or_family, uint64_t vram_bytes = 0, uint64_t min_vram_mb = 500) {
        std::string lower = name_or_family;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        const char* keywords[] = { "embed", "bert", "nomic", "bge", "rerank" };
        for (const char* kw : keywords) {
            if (lower.find(kw) != std::string::npos) {
                return true;
            }
        }
        if (vram_bytes > 0 && min_vram_mb > 0 && (vram_bytes / (1024 * 1024)) < min_vram_mb && FindLastParamBadge(name_or_family).length == 0) {
            return true;
        }
        return false;
    }
};

// 7-Stage Formatter for single model widget
inline std::string FormatCompositeLlmWidget(const Wire::LlmMetrics& model, const ModelNameCondenser& condenser) {
    size_t name_len = strnlen(model.model_name, sizeof(model.model_name));
    std::string clean_name = condenser.Condense(std::string_view(model.model_name, name_len));
    std::string prefix = clean_name.empty() ? "" : (clean_name + " ");

    switch (model.state) {
        case Wire::LlmState::Offline:
            return prefix + "Offline";
        case Wire::LlmState::Loading:
            return prefix + "Loading...";
        case Wire::LlmState::Thinking:
            return prefix + "Thinking...";
        case Wire::LlmState::Generating: {
            if (!std::isnan(model.tps) && !std::isinf(model.tps) && model.tps > 0.0f) {
                char tps_buf[32];
                std::snprintf(tps_buf, sizeof(tps_buf), "%.1f T/s", model.tps);
                return prefix + tps_buf;
            }
            return prefix + "Generating";
        }
        case Wire::LlmState::BurstHold: {
            if (!std::isnan(model.tps) && !std::isinf(model.tps) && model.tps > 0.0f) {
                char tps_buf[32];
                std::snprintf(tps_buf, sizeof(tps_buf), "%.1f T/s (Done)", model.tps);
                return prefix + tps_buf;
            }
            return prefix + "Done";
        }
        case Wire::LlmState::Compacting:
            return prefix + "Compacting...";
        case Wire::LlmState::Idle:
            return prefix + "Idle";
        case Wire::LlmState::Unloading:
            return prefix + "Unloading...";
        default:
            return clean_name.empty() ? "Idle" : clean_name;
    }
}

// Multi-Model Widget Formatter with Co-Resident Indexing
inline std::string FormatMultiModelWidget(
    const LlmSnapshot& snapshot,
    const ModelNameCondenser& condenser,
    const std::string& target_model = "active"
) {
    uint8_t count = std::min(snapshot.llm_count, static_cast<uint8_t>(Wire::WIRE_MAX_MODELS));
    if (!snapshot.is_online || count == 0) {
        return "Offline";
    }

    if (target_model == "all") {
        std::string result;
        for (uint8_t i = 0; i < count; ++i) {
            if (i > 0) result += ", ";
            result += FormatCompositeLlmWidget(snapshot.llms[i], condenser);
        }
        return result;
    }

    // Explicit numeric index ("0", "1", "2", "3")
    if (!target_model.empty() && std::isdigit(static_cast<unsigned char>(target_model[0]))) {
        size_t idx = static_cast<size_t>(target_model[0] - '0');
        if (idx < count) {
            return FormatCompositeLlmWidget(snapshot.llms[idx], condenser);
        }
    }

    // Default "active": target actively inferring model (Generating, Thinking, BurstHold)
    uint8_t active_idx = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (snapshot.llms[i].state == Wire::LlmState::Generating ||
            snapshot.llms[i].state == Wire::LlmState::Thinking ||
            snapshot.llms[i].state == Wire::LlmState::BurstHold) {
            active_idx = i;
            break;
        }
    }

    if (active_idx >= count) active_idx = 0;
    std::string base = FormatCompositeLlmWidget(snapshot.llms[active_idx], condenser);
    if (count > 1) {
        base += " (+" + std::to_string(count - 1) + ")";
    }
    return base;
}

} // namespace BareStats
