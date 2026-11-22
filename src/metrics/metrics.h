#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "metrics/llm/llm_resolver.h"

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
    std::optional<double> utilizationPercent;
    uint64_t memoryUsedBytes = 0;
    uint64_t memoryTotalBytes = 0;
    uint64_t memoryReservedBytes = 0;
    bool isIntegrated = false;
    uint64_t dedicatedVideoMemoryBytes = 0; // BIOS dedicated aperture (e.g. 128 MB)
    uint64_t sharedSystemMemoryBytes = 0;   // Shared system memory pool (e.g. 7.9 GB)
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

struct NetworkMetrics {
    bool isAvailable = false;
    uint64_t rxBytesPerSec = 0;   // Inbound / download throughput
    uint64_t txBytesPerSec = 0;   // Outbound / upload throughput
    uint64_t totalRxBytes = 0;    // Lifetime received bytes
    uint64_t totalTxBytes = 0;    // Lifetime transmitted bytes
    std::string primaryAdapterName; // Cross-platform UTF-8 primary adapter name
    std::wstring primaryAdapter;  // Primary active physical gateway adapter name
};

struct SystemMetrics {
    CpuMetrics cpu;
    GpuMetrics gpu;               // Primary targeted GPU (for single-GPU compatibility)
    std::vector<GpuMetrics> gpus; // All targeted GPUs for side-by-side multi-GPU monitoring
    MemoryMetrics memory;
    NetworkMetrics network;
    RigClusterSnapshot cluster{};
};

} // namespace BareStats
