#include "layout_engine.h"
#include <algorithm>
#include <cctype>

namespace BareStats {

namespace {

std::string_view Trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

LlmMetricType StringToLlmMetric(std::string_view s) {
    if (s == "tps" || s == "llm_tps") return LlmMetricType::Tps;
    if (s == "model" || s == "model_name" || s == "llm_model") return LlmMetricType::ModelName;
    if (s == "kv" || s == "kv_cache" || s == "llm_kv") return LlmMetricType::KvCache;
    if (s == "vram" || s == "llm_vram") return LlmMetricType::Vram;
    if (s == "queue" || s == "q" || s == "llm_queue") return LlmMetricType::Queue;
    return LlmMetricType::Composite;
}

} // namespace

std::vector<EndpointDef> ParseEndpoints(std::string_view config_line) {
    std::vector<EndpointDef> endpoints;

    // Strip '#' comments if present
    size_t hash_pos = config_line.find('#');
    if (hash_pos != std::string_view::npos) {
        config_line = config_line.substr(0, hash_pos);
    }

    auto isEndpoint = [](std::string_view item) -> bool {
        item = Trim(item);
        if (item.empty()) return false;
        if (item.front() == '#' || item.front() == ';') return false;

        size_t eq = item.find('=');
        std::string_view val = (eq != std::string_view::npos) ? Trim(item.substr(eq + 1)) : item;
        if (val.empty()) return false;

        // URLs cannot contain whitespace
        if (val.find(' ') != std::string_view::npos || val.find('\t') != std::string_view::npos) {
            return false;
        }

        if (val.find("://") != std::string_view::npos) return true;
        if (val.find(':') != std::string_view::npos) {
            size_t colon = val.rfind(':');
            if (colon + 1 < val.size() && std::isdigit(static_cast<unsigned char>(val[colon + 1]))) {
                return true;
            }
        }
        return false;
    };

    size_t start = 0;
    while (start < config_line.size()) {
        size_t delim = config_line.find_first_of(",;", start);
        std::string_view item = (delim == std::string_view::npos)
            ? config_line.substr(start)
            : config_line.substr(start, delim - start);

        item = Trim(item);
        if (!item.empty()) {
            if (!isEndpoint(item)) {
                // If this item is not a valid endpoint, it represents a comment at the end of the line
                break;
            }

            size_t eq_pos = item.find('=');
            if (eq_pos != std::string_view::npos) {
                std::string rig = std::string(Trim(item.substr(0, eq_pos)));
                std::string url = std::string(Trim(item.substr(eq_pos + 1)));

                while (!url.empty() && url.back() == '/') {
                    url.pop_back();
                }

                if (!rig.empty() && !url.empty()) {
                    endpoints.push_back({std::move(rig), std::move(url)});
                }
            } else {
                std::string url = std::string(item);
                while (!url.empty() && url.back() == '/') {
                    url.pop_back();
                }
                endpoints.push_back({"local", std::move(url)});
            }
        }

        if (delim == std::string_view::npos) break;
        start = delim + 1;
    }

    return endpoints;
}

GridToken ParseGridToken(std::string_view token_str) {
    GridToken token;
    token.rig_target = "any";
    token.model_index = -1;
    token.metric = LlmMetricType::Composite;

    token_str = Trim(token_str);
    if (token_str.empty()) return token;

    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start < token_str.size()) {
        size_t colon = token_str.find(':', start);
        if (colon == std::string_view::npos) {
            parts.push_back(token_str.substr(start));
            break;
        }
        parts.push_back(token_str.substr(start, colon - start));
        start = colon + 1;
    }

    if (parts.size() == 1) {
        std::string_view p = parts[0];
        if (p == "llm") {
            token.rig_target = "any";
            token.model_index = -1;
            token.metric = LlmMetricType::Composite;
        } else if (p.rfind("llm", 0) == 0 && p.size() > 3 && std::isdigit(static_cast<unsigned char>(p[3]))) {
            token.rig_target = "any";
            token.model_index = p[3] - '0';
            token.metric = LlmMetricType::Composite;
        } else {
            token.rig_target = "any";
            token.model_index = -1;
            token.metric = StringToLlmMetric(p);
        }
    } else if (parts.size() == 2) {
        std::string_view p0 = parts[0];
        std::string_view p1 = parts[1];

        if (p0.rfind("llm", 0) == 0 && p0.size() > 3 && std::isdigit(static_cast<unsigned char>(p0[3]))) {
            token.rig_target = "any";
            token.model_index = p0[3] - '0';
            token.metric = StringToLlmMetric(p1);
        } else if (p1.rfind("llm", 0) == 0 && p1.size() > 3 && std::isdigit(static_cast<unsigned char>(p1[3]))) {
            token.rig_target = std::string(p0);
            token.model_index = p1[3] - '0';
            token.metric = LlmMetricType::Composite;
        } else {
            token.rig_target = std::string(p0);
            token.model_index = -1;
            token.metric = StringToLlmMetric(p1);
        }
    } else {
        // 3 or more parts: e.g. "rig1:llm0:tps"
        token.rig_target = std::string(parts[0]);
        std::string_view p1 = parts[1];
        if (p1.rfind("llm", 0) == 0 && p1.size() > 3 && std::isdigit(static_cast<unsigned char>(p1[3]))) {
            token.model_index = p1[3] - '0';
        } else if (p1.size() == 1 && std::isdigit(static_cast<unsigned char>(p1[0]))) {
            token.model_index = p1[0] - '0';
        } else {
            token.model_index = -1;
        }
        token.metric = StringToLlmMetric(parts[2]);
    }

    return token;
}

} // namespace BareStats
