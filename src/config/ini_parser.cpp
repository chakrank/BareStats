#include "ini_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace BareStats {

std::string IniDocument::ToLower(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string IniDocument::Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

bool IniDocument::Load(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return LoadFromString(content);
}

bool IniDocument::LoadFromString(const std::string& content) {
    lines_.clear();
    keyIndexMap_.clear();
    sectionLastLineMap_.clear();
    orderedSections_.clear();

    std::istringstream stream(content);
    std::string rawLine;
    std::string currentSectionLower;

    while (std::getline(stream, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }

        std::string trimmed = Trim(rawLine);
        LineRecord rec;
        rec.raw = rawLine;

        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            rec.type = LineType::EmptyOrComment;
            rec.section = currentSectionLower;
            lines_.push_back(rec);
            if (!currentSectionLower.empty()) {
                sectionLastLineMap_[currentSectionLower] = lines_.size() - 1;
            }
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            rec.type = LineType::SectionHeader;
            std::string secName = Trim(trimmed.substr(1, trimmed.size() - 2));
            currentSectionLower = ToLower(secName);
            rec.section = currentSectionLower;
            lines_.push_back(rec);
            sectionLastLineMap_[currentSectionLower] = lines_.size() - 1;

            if (std::find(orderedSections_.begin(), orderedSections_.end(), currentSectionLower) == orderedSections_.end()) {
                orderedSections_.push_back(currentSectionLower);
            }
            continue;
        }

        size_t eqPos = trimmed.find('=');
        if (eqPos != std::string::npos) {
            rec.type = LineType::KeyValue;
            rec.section = currentSectionLower;
            std::string rawKey = Trim(trimmed.substr(0, eqPos));
            std::string rawVal = Trim(trimmed.substr(eqPos + 1));

            // Strip enclosing quotes if present
            if (rawVal.size() >= 2 && ((rawVal.front() == '"' && rawVal.back() == '"') || (rawVal.front() == '\'' && rawVal.back() == '\''))) {
                rawVal = rawVal.substr(1, rawVal.size() - 2);
            }

            rec.key = ToLower(rawKey);
            rec.value = rawVal;
            lines_.push_back(rec);

            size_t lineIdx = lines_.size() - 1;
            keyIndexMap_[currentSectionLower][rec.key] = lineIdx;
            if (!currentSectionLower.empty()) {
                sectionLastLineMap_[currentSectionLower] = lineIdx;
            }
            continue;
        }

        rec.type = LineType::EmptyOrComment;
        rec.section = currentSectionLower;
        lines_.push_back(rec);
        if (!currentSectionLower.empty()) {
            sectionLastLineMap_[currentSectionLower] = lines_.size() - 1;
        }
    }

    return true;
}

bool IniDocument::Save(const std::string& filePath) const {
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    std::string content = ToString();
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

std::string IniDocument::ToString() const {
    std::ostringstream oss;
    for (const auto& rec : lines_) {
        oss << rec.raw << "\n";
    }
    return oss.str();
}

bool IniDocument::HasSection(const std::string& section) const {
    std::string secLower = ToLower(section);
    return keyIndexMap_.find(secLower) != keyIndexMap_.end() ||
           sectionLastLineMap_.find(secLower) != sectionLastLineMap_.end();
}

bool IniDocument::HasKey(const std::string& section, const std::string& key) const {
    std::string secLower = ToLower(section);
    std::string keyLower = ToLower(key);
    auto itSec = keyIndexMap_.find(secLower);
    if (itSec == keyIndexMap_.end()) return false;
    return itSec->second.find(keyLower) != itSec->second.end();
}

std::string IniDocument::GetString(const std::string& section, const std::string& key, const std::string& defaultVal) const {
    std::string secLower = ToLower(section);
    std::string keyLower = ToLower(key);
    auto itSec = keyIndexMap_.find(secLower);
    if (itSec == keyIndexMap_.end()) return defaultVal;
    auto itKey = itSec->second.find(keyLower);
    if (itKey == itSec->second.end()) return defaultVal;
    return lines_[itKey->second].value;
}

int IniDocument::GetInt(const std::string& section, const std::string& key, int defaultVal) const {
    std::string str = GetString(section, key, "");
    if (str.empty()) return defaultVal;
    try {
        return std::stoi(str, nullptr, 0); // Supports dec, hex (0x...), octal
    } catch (...) {
        return defaultVal;
    }
}

bool IniDocument::GetBool(const std::string& section, const std::string& key, bool defaultVal) const {
    std::string str = ToLower(GetString(section, key, ""));
    if (str.empty()) return defaultVal;
    if (str == "1" || str == "true" || str == "yes" || str == "on") return true;
    if (str == "0" || str == "false" || str == "no" || str == "off") return false;
    return defaultVal;
}

double IniDocument::GetDouble(const std::string& section, const std::string& key, double defaultVal) const {
    std::string str = GetString(section, key, "");
    if (str.empty()) return defaultVal;
    try {
        return std::stod(str);
    } catch (...) {
        return defaultVal;
    }
}

void IniDocument::SetString(const std::string& section, const std::string& key, const std::string& value) {
    std::string secLower = ToLower(section);
    std::string keyLower = ToLower(key);

    auto itSec = keyIndexMap_.find(secLower);
    if (itSec != keyIndexMap_.end()) {
        auto itKey = itSec->second.find(keyLower);
        if (itKey != itSec->second.end()) {
            // Update existing line
            size_t idx = itKey->second;
            lines_[idx].value = value;
            lines_[idx].raw = key + " = " + value;
            return;
        }
    }

    // Key doesn't exist yet. Check if section exists.
    auto itLast = sectionLastLineMap_.find(secLower);
    if (itLast != sectionLastLineMap_.end()) {
        size_t insertPos = itLast->second + 1;
        LineRecord rec;
        rec.type = LineType::KeyValue;
        rec.section = secLower;
        rec.key = keyLower;
        rec.value = value;
        rec.raw = key + " = " + value;

        lines_.insert(lines_.begin() + insertPos, rec);

        // Shift all subsequent mapped indices
        for (auto& s : keyIndexMap_) {
            for (auto& k : s.second) {
                if (k.second >= insertPos) {
                    k.second++;
                }
            }
        }
        for (auto& sl : sectionLastLineMap_) {
            if (sl.second >= insertPos) {
                sl.second++;
            }
        }

        keyIndexMap_[secLower][keyLower] = insertPos;
        sectionLastLineMap_[secLower] = insertPos;
        return;
    }

    // Section doesn't exist. Append new section and key at end of file.
    if (!lines_.empty() && !lines_.back().raw.empty()) {
        LineRecord emptyRec;
        emptyRec.type = LineType::EmptyOrComment;
        emptyRec.raw = "";
        lines_.push_back(emptyRec);
    }

    LineRecord secRec;
    secRec.type = LineType::SectionHeader;
    secRec.section = secLower;
    secRec.raw = "[" + section + "]";
    lines_.push_back(secRec);

    LineRecord valRec;
    valRec.type = LineType::KeyValue;
    valRec.section = secLower;
    valRec.key = keyLower;
    valRec.value = value;
    valRec.raw = key + " = " + value;
    lines_.push_back(valRec);

    size_t valIdx = lines_.size() - 1;
    keyIndexMap_[secLower][keyLower] = valIdx;
    sectionLastLineMap_[secLower] = valIdx;
    orderedSections_.push_back(secLower);
}

void IniDocument::SetInt(const std::string& section, const std::string& key, int value) {
    SetString(section, key, std::to_string(value));
}

void IniDocument::SetBool(const std::string& section, const std::string& key, bool value) {
    SetString(section, key, value ? "1" : "0");
}

void IniDocument::SetDouble(const std::string& section, const std::string& key, double value) {
    SetString(section, key, std::to_string(value));
}

std::vector<std::string> IniDocument::GetSections() const {
    return orderedSections_;
}

std::vector<std::string> IniDocument::GetKeys(const std::string& section) const {
    std::string secLower = ToLower(section);
    std::vector<std::string> keys;
    auto itSec = keyIndexMap_.find(secLower);
    if (itSec != keyIndexMap_.end()) {
        for (const auto& pair : itSec->second) {
            keys.push_back(pair.first);
        }
    }
    return keys;
}

} // namespace BareStats
