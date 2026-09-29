#pragma once

#include <cstdint>
#include <string>
#include <optional>

namespace BareStats {

struct CpuMetrics {
    double utilizationPercent = 0.0;
    std::optional<double> temperatureC;
    std::optional<double> powerWatts;
    std::optional<uint32_t> fanRpm;
};

struct GpuMetrics {
    bool isAvailable = false;
    std::string name;
    double utilizationPercent = 0.0;
    uint64_t memoryUsedBytes = 0;
    uint64_t memoryTotalBytes = 0;
    std::optional<uint32_t> temperatureC;
    std::optional<uint32_t> tempSlowdownLimitC;
    std::optional<uint32_t> powerWatts;
    std::optional<uint32_t> powerLimitWatts;
    std::optional<uint32_t> fanSpeedPercent;
    std::optional<uint32_t> fanRpm;
};

struct MemoryMetrics {
    uint64_t usedBytes = 0;
    uint64_t totalBytes = 0;
};

struct SystemMetrics {
    CpuMetrics cpu;
    GpuMetrics gpu;
    MemoryMetrics memory;
};

} // namespace BareStats
