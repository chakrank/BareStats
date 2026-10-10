#include "gpu_collector.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace BareStats {


static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    std::string str(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
    return str;
}

struct InternalAdapter {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc;
    uint32_t globalIndex = 0;
    bool isNvidia = false;
    uint32_t nvidiaIndex = 0;
    bool isAmd = false;
    uint32_t amdIndex = 0;
    bool isIntel = false;
    uint32_t intelIndex = 0;
};

static std::vector<InternalAdapter> EnumerateAdapters() {
    std::vector<InternalAdapter> adapters;
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return adapters;
    }

    ComPtr<IDXGIAdapter1> curAdapter;
    UINT adapterIdx = 0;
    uint32_t globalIdx = 0;
    uint32_t nvidiaCount = 0;
    uint32_t amdCount = 0;
    uint32_t intelCount = 0;

    while (factory->EnumAdapters1(adapterIdx, &curAdapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        curAdapter->GetDesc1(&desc);

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            InternalAdapter item;
            item.adapter = curAdapter;
            item.desc = desc;
            item.globalIndex = globalIdx++;
            item.isNvidia = (desc.VendorId == 0x10DE);
            item.nvidiaIndex = item.isNvidia ? (nvidiaCount++) : 0;
            item.isAmd = (desc.VendorId == 0x1002);
            item.amdIndex = item.isAmd ? (amdCount++) : 0;
            item.isIntel = (desc.VendorId == 0x8086);
            item.intelIndex = item.isIntel ? (intelCount++) : 0;
            adapters.push_back(item);
        }
        adapterIdx++;
    }

    return adapters;
}

std::vector<GpuDeviceInfo> GpuCollector::GetAvailableGpus() {
    std::vector<GpuDeviceInfo> devices;
    auto adapters = EnumerateAdapters();

    for (const auto& a : adapters) {
        GpuDeviceInfo info;
        info.index = a.globalIndex;
        info.name = WideToUtf8(a.desc.Description);
        info.vendorId = a.desc.VendorId;
        info.deviceId = a.desc.DeviceId;
        info.dedicatedVideoMemory = a.desc.DedicatedVideoMemory;
        info.sharedSystemMemory = a.desc.SharedSystemMemory;
        info.isNvidia = a.isNvidia;
        info.nvidiaDeviceIndex = a.nvidiaIndex;
        info.isAmd = a.isAmd;
        info.amdDeviceIndex = a.amdIndex;
        info.isIntel = a.isIntel;
        info.intelDeviceIndex = a.intelIndex;
        devices.push_back(info);
    }

    return devices;
}

GpuCollector::GpuCollector() {
    auto adapters = EnumerateAdapters();
    for (const auto& a : adapters) {
        d3dKmtCollector_.QueryEngineUtilization(a.desc.AdapterLuid);
    }
}

GpuCollector::~GpuCollector() = default;

void GpuCollector::Recover() {
    retryCount_ = 0;
    nvmlCollector_.Recover();
    adlCollector_.Recover();
    igclCollector_.Recover();
    d3dKmtCollector_.Reset();
}

void GpuCollector::Shutdown() {
    nvmlCollector_.Shutdown();
    adlCollector_.Shutdown();
    igclCollector_.Shutdown();
}

static GpuMetrics CollectFromAdapter(const InternalAdapter& target, NvmlCollector& nvmlCollector,
                                     AdlCollector& adlCollector, IgclCollector& igclCollector,
                                     D3dKmtCollector& d3dKmtCollector, uint32_t& retryCount) {
    GpuMetrics metrics;

    // 1. HYBRID LAPTOP BATTERY GUARD:
    // If the selected GPU is NVIDIA, probe NVML first.
    // If targeting AMD or Intel, bypass NVML completely to keep mobile dGPUs in D3-cold sleep.
    if (target.isNvidia) {
        if (!nvmlCollector.IsAvailable()) {
            retryCount++;
            if (retryCount >= 10) { // Probe NVML re-initialization every ~10s
                retryCount = 0;
                nvmlCollector.Recover();
            }
        } else {
            retryCount = 0;
        }

        if (nvmlCollector.IsAvailable()) {
            if (nvmlCollector.Collect(metrics, target.nvidiaIndex)) {
                return metrics;
            }
        }
    }

    // 2. Universal DXGI Pipeline (Base VRAM accounting for all GPUs)
    metrics.isAvailable = true;
    metrics.name = WideToUtf8(target.desc.Description);

    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(target.adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO localMem{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localMem))) {
            metrics.memoryUsedBytes = localMem.CurrentUsage;
            metrics.memoryTotalBytes = (target.desc.DedicatedVideoMemory > 0)
                ? target.desc.DedicatedVideoMemory
                : localMem.Budget;
            if (target.desc.DedicatedVideoMemory > localMem.Budget && localMem.Budget > 0) {
                metrics.memoryReservedBytes = target.desc.DedicatedVideoMemory - localMem.Budget;
            }
        }

        // On integrated graphics (Intel UHD / Iris, AMD APU), dedicated VRAM may be minimal or 0
        // with workloads backed by non-local shared system memory.
        if (target.desc.DedicatedVideoMemory <= 512 * 1024 * 1024) {
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMem{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalMem))) {
                if (nonLocalMem.CurrentUsage > 0 && metrics.memoryUsedBytes == 0) {
                    metrics.memoryUsedBytes = nonLocalMem.CurrentUsage;
                }
                if (metrics.memoryTotalBytes == 0 || metrics.memoryTotalBytes <= 512 * 1024 * 1024) {
                    if (nonLocalMem.Budget > 0) {
                        metrics.memoryTotalBytes = (target.desc.SharedSystemMemory > 0)
                            ? target.desc.SharedSystemMemory
                            : nonLocalMem.Budget;
                        if (target.desc.SharedSystemMemory > nonLocalMem.Budget && nonLocalMem.Budget > 0) {
                            metrics.memoryReservedBytes = target.desc.SharedSystemMemory - nonLocalMem.Budget;
                        }
                    }
                }
            }
        }
    } else {
        metrics.memoryTotalBytes = target.desc.DedicatedVideoMemory;
    }

    // 3. AMD ADL Telemetry Enrichment (Core thermals, board power wattage, fan RPM)
    if (target.isAmd) {
        if (!adlCollector.IsAvailable()) {
            adlCollector.Recover();
        }
        if (adlCollector.IsAvailable()) {
            adlCollector.Collect(metrics, target.amdIndex);
        }
    }

    // 4. Intel IGCL Telemetry Enrichment (Package thermals, power draw, fan speed)
    if (target.isIntel) {
        if (!igclCollector.IsAvailable()) {
            igclCollector.Recover();
        }
        if (igclCollector.IsAvailable()) {
            igclCollector.Collect(metrics, target.intelIndex);
        }
    }

    // 5. Universal Windows D3DKMT Engine Utilization Fallback
    // If vendor SDKs (NVML, ADL, IGCL) did not provide utilization percentage (e.g. Intel UHD / Iris,
    // AMD APUs without ADL, Qualcomm Snapdragon ARM64, or generic WDDM drivers):
    if (!metrics.utilizationPercent.has_value() && d3dKmtCollector.IsAvailable()) {
        auto kmtUtil = d3dKmtCollector.QueryEngineUtilization(target.desc.AdapterLuid);
        if (kmtUtil.has_value()) {
            metrics.utilizationPercent = *kmtUtil;
        }
    }

    return metrics;
}

GpuMetrics GpuCollector::Collect(uint32_t deviceIndex) {
    auto adapters = EnumerateAdapters();
    if (adapters.empty()) {
        return GpuMetrics{};
    }

    uint32_t targetIdx = (deviceIndex < adapters.size()) ? deviceIndex : 0;
    return CollectFromAdapter(adapters[targetIdx], nvmlCollector_, adlCollector_, igclCollector_, d3dKmtCollector_, retryCount_);
}

std::vector<GpuMetrics> GpuCollector::CollectMultiple(const std::vector<uint32_t>& targetIndices, bool allGpus) {
    std::vector<GpuMetrics> results;
    auto adapters = EnumerateAdapters();
    if (adapters.empty()) {
        return results;
    }

    if (allGpus) {
        for (const auto& a : adapters) {
            results.push_back(CollectFromAdapter(a, nvmlCollector_, adlCollector_, igclCollector_, d3dKmtCollector_, retryCount_));
        }
    } else {
        std::vector<uint32_t> effective = targetIndices.empty() ? std::vector<uint32_t>{ 0 } : targetIndices;
        for (uint32_t idx : effective) {
            uint32_t targetIdx = (idx < adapters.size()) ? idx : 0;
            results.push_back(CollectFromAdapter(adapters[targetIdx], nvmlCollector_, adlCollector_, igclCollector_, d3dKmtCollector_, retryCount_));
        }
    }

    return results;
}

} // namespace BareStats
