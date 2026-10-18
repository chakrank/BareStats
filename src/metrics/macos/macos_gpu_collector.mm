#include "macos_gpu_collector.h"

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <string>
#include <vector>

#ifndef kIOMainPortDefault
#define kIOMainPortDefault MACH_PORT_NULL
#endif

namespace BareStats {

struct IOKitGpuStats {
    std::optional<double> utilizationPercent;
    uint64_t vramUsedBytes = 0;
    uint64_t vramTotalBytes = 0;
    std::optional<uint32_t> temperatureC;
    std::optional<uint32_t> powerWatts;
};

static std::vector<IOKitGpuStats> QueryIOKitGpuStatistics() {
    std::vector<IOKitGpuStats> results;

    CFMutableDictionaryRef matching = IOServiceMatching("IOAccelerator");
    if (!matching) {
        return results;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS || iterator == IO_OBJECT_NULL) {
        return results;
    }

    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator))) {
        CFMutableDictionaryRef properties = nullptr;
        if (IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) == KERN_SUCCESS && properties) {
            CFDictionaryRef perfStats = (CFDictionaryRef)CFDictionaryGetValue(properties, CFSTR("PerformanceStatistics"));
            if (perfStats && CFGetTypeID(perfStats) == CFDictionaryGetTypeID()) {
                IOKitGpuStats s;

                // 1. Device Utilization %
                CFNumberRef utilNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("Device Utilization %"));
                if (!utilNum) {
                    utilNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("GPU Activity(%)"));
                }
                if (!utilNum) {
                    utilNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("GPU Core Utilization"));
                }
                if (utilNum && CFGetTypeID(utilNum) == CFNumberGetTypeID()) {
                    int val = 0;
                    CFNumberGetValue(utilNum, kCFNumberIntType, &val);
                    if (val >= 0 && val <= 100) {
                        s.utilizationPercent = static_cast<double>(val);
                    }
                }

                // 2. VRAM metrics (discrete GPUs or unified allocations)
                CFNumberRef vramUsedNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("vramUsedBytes"));
                if (!vramUsedNum) {
                    vramUsedNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("In use system memory"));
                }
                if (vramUsedNum && CFGetTypeID(vramUsedNum) == CFNumberGetTypeID()) {
                    int64_t vramVal = 0;
                    CFNumberGetValue(vramUsedNum, kCFNumberSInt64Type, &vramVal);
                    if (vramVal > 0) {
                        s.vramUsedBytes = static_cast<uint64_t>(vramVal);
                    }
                }

                CFNumberRef vramTotalNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("vramTotalBytes"));
                if (vramTotalNum && CFGetTypeID(vramTotalNum) == CFNumberGetTypeID()) {
                    int64_t totalVal = 0;
                    CFNumberGetValue(vramTotalNum, kCFNumberSInt64Type, &totalVal);
                    if (totalVal > 0) {
                        s.vramTotalBytes = static_cast<uint64_t>(totalVal);
                    }
                }

                // 3. Thermal & Power Statistics if exposed
                CFNumberRef tempNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("Temperature(C)"));
                if (!tempNum) {
                    tempNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("GPU Temperature"));
                }
                if (tempNum && CFGetTypeID(tempNum) == CFNumberGetTypeID()) {
                    int32_t t = 0;
                    CFNumberGetValue(tempNum, kCFNumberSInt32Type, &t);
                    if (t > 0 && t < 150) {
                        s.temperatureC = static_cast<uint32_t>(t);
                    }
                }

                CFNumberRef pwrNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("Power(W)"));
                if (!pwrNum) {
                    pwrNum = (CFNumberRef)CFDictionaryGetValue(perfStats, CFSTR("GPU Power"));
                }
                if (pwrNum && CFGetTypeID(pwrNum) == CFNumberGetTypeID()) {
                    int32_t p = 0;
                    CFNumberGetValue(pwrNum, kCFNumberSInt32Type, &p);
                    if (p > 0 && p < 1000) {
                        s.powerWatts = static_cast<uint32_t>(p);
                    }
                }

                results.push_back(s);
            }
            CFRelease(properties);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);

    return results;
}

struct MacosGpuCollector::Impl {
    uint64_t totalSystemRam = 0;

    Impl() {
        size_t len = sizeof(totalSystemRam);
        int mib[2] = { CTL_HW, HW_MEMSIZE };
        sysctl(mib, 2, &totalSystemRam, &len, nullptr, 0);
    }

    void Recover() {}
    void Shutdown() {}

    GpuMetrics Collect(uint32_t deviceIndex) {
        GpuMetrics metrics{};

        @autoreleasepool {
            NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
            if (!devices || devices.count == 0) {
                id<MTLDevice> defDevice = MTLCreateSystemDefaultDevice();
                if (defDevice) {
                    devices = @[ defDevice ];
                }
            }

            if (!devices || devices.count == 0) {
                return metrics;
            }

            uint32_t targetIdx = (deviceIndex < devices.count) ? deviceIndex : 0;
            id<MTLDevice> targetDev = devices[targetIdx];

            metrics.isAvailable = true;
            metrics.name = targetDev.name ? [targetDev.name UTF8String] : "Apple Metal GPU";

            // VRAM sizing: recommendedMaxWorkingSetSize reflects usable VRAM quota
            uint64_t maxWorkingSet = targetDev.recommendedMaxWorkingSetSize;
            metrics.memoryTotalBytes = (maxWorkingSet > 0) ? maxWorkingSet : totalSystemRam;

            // Live GPU allocations currently held by Metal
            metrics.memoryUsedBytes = targetDev.currentAllocatedSize;

            // Correlate with IOKit PerformanceStatistics for device load & discrete VRAM
            auto ioStats = QueryIOKitGpuStatistics();
            if (!ioStats.empty()) {
                uint32_t ioIdx = (targetIdx < ioStats.size()) ? targetIdx : 0;
                const auto& s = ioStats[ioIdx];

                if (s.utilizationPercent.has_value()) {
                    metrics.utilizationPercent = *s.utilizationPercent;
                }
                if (s.vramUsedBytes > 0 && metrics.memoryUsedBytes == 0) {
                    metrics.memoryUsedBytes = s.vramUsedBytes;
                }
                if (s.vramTotalBytes > 0) {
                    metrics.memoryTotalBytes = s.vramTotalBytes;
                }
                if (s.temperatureC.has_value()) {
                    metrics.temperatureC = s.temperatureC;
                }
                if (s.powerWatts.has_value()) {
                    metrics.powerWatts = s.powerWatts;
                }
            }
        }

        return metrics;
    }

    std::vector<GpuMetrics> CollectMultiple(const std::vector<uint32_t>& targetIndices, bool allGpus) {
        std::vector<GpuMetrics> results;
        @autoreleasepool {
            NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
            if (!devices || devices.count == 0) {
                id<MTLDevice> defDevice = MTLCreateSystemDefaultDevice();
                if (defDevice) {
                    devices = @[ defDevice ];
                }
            }

            if (!devices || devices.count == 0) {
                return results;
            }

            if (allGpus) {
                for (uint32_t i = 0; i < devices.count; ++i) {
                    results.push_back(Collect(i));
                }
            } else {
                std::vector<uint32_t> effective = targetIndices.empty() ? std::vector<uint32_t>{ 0 } : targetIndices;
                for (uint32_t idx : effective) {
                    uint32_t targetIdx = (idx < devices.count) ? idx : 0;
                    results.push_back(Collect(targetIdx));
                }
            }
        }
        return results;
    }
};

std::vector<GpuDeviceInfo> MacosGpuCollector::GetAvailableGpus() {
    std::vector<GpuDeviceInfo> list;

    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (!devices || devices.count == 0) {
            id<MTLDevice> defDevice = MTLCreateSystemDefaultDevice();
            if (defDevice) {
                devices = @[ defDevice ];
            }
        }

        if (!devices) {
            return list;
        }

        uint64_t totalRam = 0;
        size_t len = sizeof(totalRam);
        int mib[2] = { CTL_HW, HW_MEMSIZE };
        sysctl(mib, 2, &totalRam, &len, nullptr, 0);

        uint32_t amdCount = 0;
        uint32_t intelCount = 0;

        for (NSUInteger i = 0; i < devices.count; ++i) {
            id<MTLDevice> dev = devices[i];
            GpuDeviceInfo info;
            info.index = static_cast<uint32_t>(i);
            info.name = dev.name ? [dev.name UTF8String] : "Apple Metal GPU";
            info.dedicatedVideoMemory = dev.recommendedMaxWorkingSetSize;
            info.sharedSystemMemory = totalRam;

            std::string lowerName = info.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (lowerName.find("amd") != std::string::npos || lowerName.find("radeon") != std::string::npos) {
                info.vendorId = 0x1002;
                info.isAmd = true;
                info.amdDeviceIndex = amdCount++;
            } else if (lowerName.find("intel") != std::string::npos || lowerName.find("iris") != std::string::npos) {
                info.vendorId = 0x8086;
                info.isIntel = true;
                info.intelDeviceIndex = intelCount++;
            } else if (lowerName.find("nvidia") != std::string::npos || lowerName.find("geforce") != std::string::npos) {
                info.vendorId = 0x10DE;
                info.isNvidia = true;
                info.nvidiaDeviceIndex = 0;
            } else {
                // Apple Silicon SoC GPU
                info.vendorId = 0x106B; // Apple Inc.
            }

            list.push_back(info);
        }
    }

    return list;
}

MacosGpuCollector::MacosGpuCollector() : impl_(std::make_unique<Impl>()) {}
MacosGpuCollector::~MacosGpuCollector() = default;

GpuMetrics MacosGpuCollector::Collect(uint32_t deviceIndex) {
    return impl_->Collect(deviceIndex);
}

std::vector<GpuMetrics> MacosGpuCollector::CollectMultiple(const std::vector<uint32_t>& targetIndices, bool allGpus) {
    return impl_->CollectMultiple(targetIndices, allGpus);
}

void MacosGpuCollector::Recover() {
    impl_->Recover();
}

void MacosGpuCollector::Shutdown() {
    impl_->Shutdown();
}

} // namespace BareStats

#endif // __APPLE__
