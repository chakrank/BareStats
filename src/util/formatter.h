#pragma once

#include "../metrics/metrics.h"
#include <string>

namespace BareStats {

struct AppConfig;

class Formatter {
public:
    static std::wstring Format(const SystemMetrics& metrics, const AppConfig* config = nullptr);
    static std::string FormatUtf8(const SystemMetrics& metrics, const AppConfig* config = nullptr);

private:
    static std::wstring FormatBytesGb(uint64_t usedBytes, uint64_t totalBytes);
};

} // namespace BareStats
