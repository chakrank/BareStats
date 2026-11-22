#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cctype>

namespace BareStats {
namespace JsonMini {

// Strips leading/trailing whitespace
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Searches for a key in a JSON object: "key": "value"
inline std::string_view FindString(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string_view::npos) {
        size_t after_key = pos + needle.size();
        // Skip whitespace and colon
        while (after_key < json.size() && (std::isspace(static_cast<unsigned char>(json[after_key])) || json[after_key] == ':')) {
            if (json[after_key] == ':') {
                after_key++;
                break;
            }
            after_key++;
        }
        while (after_key < json.size() && std::isspace(static_cast<unsigned char>(json[after_key]))) after_key++;

        if (after_key < json.size() && json[after_key] == '"') {
            size_t val_start = after_key + 1;
            size_t val_end = json.find('"', val_start);
            if (val_end != std::string_view::npos) {
                return json.substr(val_start, val_end - val_start);
            }
        }

        // Keep looking if key appeared in another context
        pos = json.find(needle, pos + 1);
    }
    return "";
}

// Searches for an integer in a JSON object: "key": 12345
inline int64_t FindInt(std::string_view json, std::string_view key, int64_t default_val = 0) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string_view::npos) {
        size_t after_key = pos + needle.size();
        while (after_key < json.size() && (std::isspace(static_cast<unsigned char>(json[after_key])) || json[after_key] == ':')) {
            if (json[after_key] == ':') {
                after_key++;
                break;
            }
            after_key++;
        }
        while (after_key < json.size() && std::isspace(static_cast<unsigned char>(json[after_key]))) after_key++;

        if (after_key < json.size() && (std::isdigit(static_cast<unsigned char>(json[after_key])) || json[after_key] == '-')) {
            size_t val_start = after_key;
            size_t val_end = val_start;
            while (val_end < json.size() && (std::isdigit(static_cast<unsigned char>(json[val_end])) || json[val_end] == '-')) {
                val_end++;
            }
            try {
                return std::stoll(std::string(json.substr(val_start, val_end - val_start)));
            } catch (...) {
                return default_val;
            }
        }
        pos = json.find(needle, pos + 1);
    }
    return default_val;
}

// Searches for a float in a JSON object: "key": 12.34
inline float FindFloat(std::string_view json, std::string_view key, float default_val = 0.0f) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string_view::npos) {
        size_t after_key = pos + needle.size();
        while (after_key < json.size() && (std::isspace(static_cast<unsigned char>(json[after_key])) || json[after_key] == ':')) {
            if (json[after_key] == ':') {
                after_key++;
                break;
            }
            after_key++;
        }
        while (after_key < json.size() && std::isspace(static_cast<unsigned char>(json[after_key]))) after_key++;

        if (after_key < json.size() && (std::isdigit(static_cast<unsigned char>(json[after_key])) || json[after_key] == '-' || json[after_key] == '.')) {
            size_t val_start = after_key;
            size_t val_end = val_start;
            while (val_end < json.size() && (std::isdigit(static_cast<unsigned char>(json[val_end])) || json[val_end] == '-' || json[val_end] == '.' || json[val_end] == 'e' || json[val_end] == 'E' || json[val_end] == '+')) {
                val_end++;
            }
            try {
                return std::stof(std::string(json.substr(val_start, val_end - val_start)));
            } catch (...) {
                return default_val;
            }
        }
        pos = json.find(needle, pos + 1);
    }
    return default_val;
}

// Extracts objects inside an array: "key": [ { ... }, { ... } ]
inline std::vector<std::string_view> FindArrayObjects(std::string_view json, std::string_view array_key = "") {
    std::vector<std::string_view> objects;

    size_t array_start = 0;
    if (!array_key.empty()) {
        std::string needle = "\"" + std::string(array_key) + "\"";
        size_t key_pos = json.find(needle);
        if (key_pos == std::string_view::npos) return objects;
        array_start = json.find('[', key_pos);
        if (array_start == std::string_view::npos) return objects;
    } else {
        // Root array e.g. [ { ... }, { ... } ]
        array_start = json.find('[');
        if (array_start == std::string_view::npos) return objects;
    }

    size_t pos = array_start + 1;
    const size_t len = json.size();

    while (pos < len) {
        // Find next object start '{'
        size_t obj_start = json.find('{', pos);
        if (obj_start == std::string_view::npos) break;

        // Check if an array closing bracket ']' appears before '{'
        size_t array_end = json.find(']', pos);
        if (array_end != std::string_view::npos && array_end < obj_start) break;

        // Balance curly braces to find the matching '}'
        int depth = 1;
        size_t curr = obj_start + 1;
        bool in_string = false;

        while (curr < len && depth > 0) {
            char c = json[curr];
            if (c == '"') {
                size_t bs_count = 0;
                size_t back = curr;
                while (back > 0 && json[back - 1] == '\\') {
                    bs_count++;
                    back--;
                }
                if ((bs_count % 2) == 0) {
                    in_string = !in_string;
                }
            } else if (!in_string) {
                if (c == '{') depth++;
                else if (c == '}') depth--;
            }
            curr++;
        }

        if (depth == 0) {
            objects.push_back(json.substr(obj_start, curr - obj_start));
            pos = curr;
        } else {
            break; // Malformed JSON
        }
    }

    return objects;
}

} // namespace JsonMini
} // namespace BareStats
