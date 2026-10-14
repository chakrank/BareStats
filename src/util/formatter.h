#pragma once

#include "../metrics/metrics.h"
#include <string>
#include <vector>

namespace BareStats {

struct AppConfig;

enum class MetricSeverity {
    Normal,
    High,
    Critical
};

enum class SegmentType {
    Label,       // e.g. " CPU ", " GPU ", " RAM "
    Number,      // e.g. "14", "35", "16", "0.3", "16"
    Unit,        // e.g. "%", "°C", "W", "RPM", "/", "GB", "N/A"
    Separator    // e.g. "│"
};

struct FormattedSegment {
    std::wstring text;
    MetricSeverity severity = MetricSeverity::Normal;
    SegmentType type = SegmentType::Number;
    bool isBold = false;
    bool isSeparator = false;
};

class Formatter {
public:
    static std::wstring Format(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::string FormatUtf8(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::vector<FormattedSegment> FormatSegments(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::vector<std::vector<FormattedSegment>> FormatRows(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::vector<FormattedSegment> FormatCell(const std::string& token, const SystemMetrics& metrics, const AppConfig* config = nullptr);

private:
    static std::wstring FormatBytesGb(uint64_t usedBytes, uint64_t totalBytes);
    static std::wstring FormatNetworkRate(uint64_t bytesPerSec);
};

} // namespace BareStats
