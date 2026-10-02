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

std::vector<GpuDeviceInfo> GpuCollector::GetAvailableGpus() {
    std::vector<GpuDeviceInfo> devices;
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return devices;
    }

    ComPtr<IDXGIAdapter1> adapter;
    UINT adapterIdx = 0;
    uint32_t hwIndex = 0;
    uint32_t nvidiaCount = 0;

    while (factory->EnumAdapters1(adapterIdx, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        // Filter out software emulators / WARP drivers
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            GpuDeviceInfo info;
            info.index = hwIndex;
            info.name = WideToUtf8(desc.Description);
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            info.sharedSystemMemory = desc.SharedSystemMemory;
            info.isNvidia = (desc.VendorId == 0x10DE);
            if (info.isNvidia) {
                info.nvidiaDeviceIndex = nvidiaCount++;
            }
            devices.push_back(info);
            hwIndex++;
        }
        adapterIdx++;
    }

    return devices;
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

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return metrics;
    }

    struct HardwareAdapter {
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc;
        bool isNvidia;
        uint32_t nvidiaIndex;
    };

    std::vector<HardwareAdapter> adapters;
    ComPtr<IDXGIAdapter1> curAdapter;
    UINT adapterIdx = 0;
    uint32_t nvidiaCount = 0;

    while (factory->EnumAdapters1(adapterIdx, &curAdapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        curAdapter->GetDesc1(&desc);

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            bool isNv = (desc.VendorId == 0x10DE);
            uint32_t nvIdx = isNv ? (nvidiaCount++) : 0;
            adapters.push_back({ curAdapter, desc, isNv, nvIdx });
        }
        adapterIdx++;
    }

    if (adapters.empty()) {
        return metrics;
    }

    // Clamp to valid adapter range (fallback to primary adapter 0)
    uint32_t targetIdx = (deviceIndex < adapters.size()) ? deviceIndex : 0;
    const auto& target = adapters[targetIdx];

    // HYBRID LAPTOP BATTERY GUARD:
    // If the selected GPU is NOT NVIDIA (e.g. Intel UHD/Iris or AMD Radeon),
    // we bypass NVML completely to keep the mobile NVIDIA dGPU in deep D3-cold sleep.
    if (target.isNvidia) {
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
            if (nvmlCollector_.Collect(metrics, target.nvidiaIndex)) {
                // NVML succeeded!
                return metrics;
            }
        }
    }

    // Universal DXGI fallback (Intel UHD/Arc, AMD Radeon, or NVIDIA if NVML is offline)
    metrics.isAvailable = true;
    if (metrics.name.empty()) {
        metrics.name = WideToUtf8(target.desc.Description);
    }

    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(target.adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO localMem{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localMem))) {
            metrics.memoryUsedBytes = localMem.CurrentUsage;
            metrics.memoryTotalBytes = (target.desc.DedicatedVideoMemory > 0)
                ? target.desc.DedicatedVideoMemory
                : localMem.Budget;
        }

        // On integrated graphics (Intel UHD / Iris, AMD APU), dedicated VRAM may be minimal or 0
        // with graphics workloads backed by non-local shared system memory.
        if (target.desc.DedicatedVideoMemory <= 512 * 1024 * 1024) {
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMem{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalMem))) {
                if (nonLocalMem.CurrentUsage > 0 && metrics.memoryUsedBytes == 0) {
                    metrics.memoryUsedBytes = nonLocalMem.CurrentUsage;
                }
                if (metrics.memoryTotalBytes == 0 || metrics.memoryTotalBytes <= 512 * 1024 * 1024) {
                    if (nonLocalMem.Budget > 0) {
                        metrics.memoryTotalBytes = nonLocalMem.Budget;
                    }
                }
            }
        }
    } else {
        metrics.memoryTotalBytes = target.desc.DedicatedVideoMemory;
    }

    return metrics;
}

} // namespace BareStats
