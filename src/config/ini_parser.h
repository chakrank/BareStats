#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace BareStats {

class IniDocument {
public:
    IniDocument() = default;

    bool Load(const std::string& filePath);
    bool LoadFromString(const std::string& content);
    bool Save(const std::string& filePath) const;
    std::string ToString() const;

    bool HasSection(const std::string& section) const;
    bool HasKey(const std::string& section, const std::string& key) const;

    std::string GetString(const std::string& section, const std::string& key, const std::string& defaultVal = "") const;
    int GetInt(const std::string& section, const std::string& key, int defaultVal = 0) const;
    bool GetBool(const std::string& section, const std::string& key, bool defaultVal = false) const;
    double GetDouble(const std::string& section, const std::string& key, double defaultVal = 0.0) const;

    void SetString(const std::string& section, const std::string& key, const std::string& value);
    void SetInt(const std::string& section, const std::string& key, int value);
    void SetBool(const std::string& section, const std::string& key, bool value);
    void SetDouble(const std::string& section, const std::string& key, double value);

    std::vector<std::string> GetSections() const;
    std::vector<std::string> GetKeys(const std::string& section) const;

private:
    enum class LineType {
        EmptyOrComment,
        SectionHeader,
        KeyValue
    };

    struct LineRecord {
        LineType type = LineType::EmptyOrComment;
        std::string raw;
        std::string section;
        std::string key;
        std::string value;
    };

    static std::string ToLower(const std::string& str);
    static std::string Trim(const std::string& str);

    std::vector<LineRecord> lines_;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> keyIndexMap_;
    std::unordered_map<std::string, size_t> sectionLastLineMap_;
    std::vector<std::string> orderedSections_;
};

} // namespace BareStats
