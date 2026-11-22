#include "d3dkmt_collector.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <algorithm>

using PFN_D3DKMTQueryStatistics = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);

namespace BareStats {

D3dKmtCollector::D3dKmtCollector() {
    QueryPerformanceFrequency(&qpcFrequency_);
    HMODULE hGdi = GetModuleHandleA("gdi32.dll");
    if (!hGdi) {
        hGdi = LoadLibraryA("gdi32.dll");
    }
    if (hGdi) {
        pfnQueryStats_ = reinterpret_cast<void*>(GetProcAddress(hGdi, "D3DKMTQueryStatistics"));
    }
    isAvailable_ = (pfnQueryStats_ != nullptr);
}

D3dKmtCollector::~D3dKmtCollector() = default;

bool D3dKmtCollector::IsAvailable() const {
    return isAvailable_;
}

void D3dKmtCollector::Reset() {
    adapterStates_.clear();
}

std::optional<double> D3dKmtCollector::QueryEngineUtilization(const LUID& adapterLuid) {
    if (!isAvailable_ || !pfnQueryStats_) {
        return std::nullopt;
    }

    auto pfn = reinterpret_cast<PFN_D3DKMTQueryStatistics>(pfnQueryStats_);

    uint64_t luidKey = (static_cast<uint64_t>(adapterLuid.HighPart) << 32) | adapterLuid.LowPart;
    auto& state = adapterStates_[luidKey];

    // Discover node count if uninitialized
    if (state.nodeCount == 0) {
        D3DKMT_QUERYSTATISTICS query{};
        query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        query.AdapterLuid = adapterLuid;

        NTSTATUS status = pfn(&query);
        if (status != 0) {
            return std::nullopt;
        }

        state.nodeCount = query.QueryResult.AdapterInformation.NodeCount;
        if (state.nodeCount == 0) {
            return std::nullopt;
        }
        state.lastRunningTimes.resize(state.nodeCount, 0);
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    std::vector<uint64_t> currentTimes(state.nodeCount, 0);
    for (uint32_t n = 0; n < state.nodeCount; ++n) {
        D3DKMT_QUERYSTATISTICS nq{};
        nq.Type = D3DKMT_QUERYSTATISTICS_NODE;
        nq.AdapterLuid = adapterLuid;
        nq.QueryNode.NodeId = n;
        if (pfn(&nq) == 0) {
            currentTimes[n] = nq.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
        }
    }

    if (!state.initialized) {
        state.lastRunningTimes = std::move(currentTimes);
        state.lastQpc = now;
        state.lastUtilization = 0.0;
        state.initialized = true;
        return 0.0;
    }

    double elapsedSec = static_cast<double>(now.QuadPart - state.lastQpc.QuadPart) / static_cast<double>(qpcFrequency_.QuadPart);
    if (elapsedSec <= 0.05) {
        return state.lastUtilization;
    }

    double sumUtil = 0.0;
    for (uint32_t n = 0; n < state.nodeCount; ++n) {
        uint64_t delta = (currentTimes[n] >= state.lastRunningTimes[n])
            ? (currentTimes[n] - state.lastRunningTimes[n])
            : 0;
        // RunningTime is recorded in 100-nanosecond units (10,000,000 units per second)
        double runningSec = static_cast<double>(delta) / 10000000.0;
        double pct = (runningSec / elapsedSec) * 100.0;
        sumUtil += pct;
    }

    state.lastRunningTimes = std::move(currentTimes);
    state.lastQpc = now;
    state.lastUtilization = std::clamp(sumUtil, 0.0, 100.0);

    return state.lastUtilization;
}

std::optional<D3dKmtMemoryStats> D3dKmtCollector::QueryMemoryUsage(const LUID& adapterLuid) {
    if (!isAvailable_ || !pfnQueryStats_) {
        return std::nullopt;
    }

    auto pfn = reinterpret_cast<PFN_D3DKMTQueryStatistics>(pfnQueryStats_);

    D3DKMT_QUERYSTATISTICS query{};
    query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    query.AdapterLuid = adapterLuid;

    if (pfn(&query) != 0) {
        return std::nullopt;
    }

    ULONG nbSegments = query.QueryResult.AdapterInformation.NbSegments;
    if (nbSegments == 0) {
        return std::nullopt;
    }

    D3dKmtMemoryStats stats{};
    for (ULONG s = 0; s < nbSegments; ++s) {
        D3DKMT_QUERYSTATISTICS sq{};
        sq.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        sq.AdapterLuid = adapterLuid;
        sq.QuerySegment.SegmentId = s;

        if (pfn(&sq) == 0) {
            const auto& segInfo = sq.QueryResult.SegmentInformation;
            if (segInfo.Aperture) {
                stats.sharedBytesResident += segInfo.BytesResident;
                stats.sharedCommitLimit += segInfo.CommitLimit;
            } else {
                stats.dedicatedBytesResident += segInfo.BytesResident;
                stats.dedicatedCommitLimit += segInfo.CommitLimit;
            }
        }
    }

    stats.totalBytesResident = stats.dedicatedBytesResident + stats.sharedBytesResident;
    return stats;
}

} // namespace BareStats
