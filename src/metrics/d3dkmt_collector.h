#pragma once

#include <windows.h>
#include <optional>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace BareStats {

class D3dKmtCollector {
public:
    D3dKmtCollector();
    ~D3dKmtCollector();

    bool IsAvailable() const;
    std::optional<double> QueryEngineUtilization(const LUID& adapterLuid);
    void Reset();

private:
    struct AdapterNodeState {
        uint32_t nodeCount = 0;
        std::vector<uint64_t> lastRunningTimes;
        LARGE_INTEGER lastQpc{};
        double lastUtilization = 0.0;
        bool initialized = false;
    };

    bool isAvailable_ = false;
    LARGE_INTEGER qpcFrequency_{};
    void* pfnQueryStats_ = nullptr;
    std::unordered_map<uint64_t, AdapterNodeState> adapterStates_;
};

} // namespace BareStats
