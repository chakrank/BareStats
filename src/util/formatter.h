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

struct FormattedSegment {
    std::wstring text;
    MetricSeverity severity = MetricSeverity::Normal;
};

class Formatter {
public:
    static std::wstring Format(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::string FormatUtf8(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::vector<FormattedSegment> FormatSegments(const SystemMetrics& metrics, const AppConfig* config = nullptr);

private:
    static std::wstring FormatBytesGb(uint64_t usedBytes, uint64_t totalBytes);
};

} // namespace BareStats
