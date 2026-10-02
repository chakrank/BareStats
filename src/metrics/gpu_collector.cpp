#include "gpu_collector.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

namespace BareStats {

GpuCollector::GpuCollector() = default;
GpuCollector::~GpuCollector() = default;

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    std::string str(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
    return str;
}

bool GpuCollector::TryCollectDxgi(GpuMetrics& metrics, uint32_t deviceIndex) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    UINT adapterIdx = 0;
    UINT discreteCount = 0;

    while (factory->EnumAdapters1(adapterIdx, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software renderers (WARP/Basic Render)
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapterIdx++;
            continue;
        }

        if (discreteCount == deviceIndex) {
            metrics.isAvailable = true;
            if (metrics.name.empty()) {
                metrics.name = WideToUtf8(desc.Description);
            }

            ComPtr<IDXGIAdapter3> adapter3;
            if (SUCCEEDED(adapter.As(&adapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    if (metrics.memoryUsedBytes == 0) {
                        metrics.memoryUsedBytes = memInfo.CurrentUsage;
                    }
                    if (metrics.memoryTotalBytes == 0) {
                        metrics.memoryTotalBytes = (desc.DedicatedVideoMemory > 0) ? desc.DedicatedVideoMemory : memInfo.Budget;
                    }
                }
            } else {
                if (metrics.memoryTotalBytes == 0) {
                    metrics.memoryTotalBytes = desc.DedicatedVideoMemory;
                }
            }

            return true;
        }

        discreteCount++;
        adapterIdx++;
    }

    return false;
}

void GpuCollector::Recover() {
    retryCount_ = 0;
    nvmlCollector_.Recover();
}

void GpuCollector::Shutdown() {
    nvmlCollector_.Shutdown();
}

GpuMetrics GpuCollector::Collect(uint32_t deviceIndex) {
    GpuMetrics metrics;

    if (!nvmlCollector_.IsAvailable()) {
        retryCount_++;
        if (retryCount_ >= 10) { // Probe NVML re-initialization every ~10s
            retryCount_ = 0;
            nvmlCollector_.Recover();
        }
    } else {
        retryCount_ = 0;
    }

    if (nvmlCollector_.IsAvailable()) {
        if (nvmlCollector_.Collect(metrics, deviceIndex)) {
            // NVML succeeded!
            return metrics;
        }
    }

    // Fall back to DXGI
    TryCollectDxgi(metrics, deviceIndex);

    return metrics;
}

} // namespace BareStats
