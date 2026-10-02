#include "nvml_collector.h"
#include <iostream>

namespace BareStats {

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

NvmlCollector::NvmlCollector() {
    LoadNvml();
}

NvmlCollector::~NvmlCollector() {
    UnloadNvml();
}

bool NvmlCollector::LoadNvml() {
    // Try system32 nvml.dll or default PATH
    nvmlDll_ = LoadLibraryA("nvml.dll");
    if (!nvmlDll_) {
        // Fallback common path
        nvmlDll_ = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    }
    if (!nvmlDll_) {
        nvmlDll_ = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }

    if (!nvmlDll_) {
        isAvailable_ = false;
        return false;
    }

    // Resolve functions
    fnNvmlInit_ = (nvmlInit_t)GetProcAddress(nvmlDll_, "nvmlInit_v2");
    if (!fnNvmlInit_) {
        fnNvmlInit_ = (nvmlInit_t)GetProcAddress(nvmlDll_, "nvmlInit");
    }

    fnNvmlShutdown_ = (nvmlShutdown_t)GetProcAddress(nvmlDll_, "nvmlShutdown");
    
    fnNvmlDeviceGetCount_ = (nvmlDeviceGetCount_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetCount_v2");
    if (!fnNvmlDeviceGetCount_) {
        fnNvmlDeviceGetCount_ = (nvmlDeviceGetCount_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetCount");
    }

    fnNvmlDeviceGetHandleByIndex_ = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetHandleByIndex_v2");
    if (!fnNvmlDeviceGetHandleByIndex_) {
        fnNvmlDeviceGetHandleByIndex_ = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetHandleByIndex");
    }

    fnNvmlDeviceGetName_ = (nvmlDeviceGetName_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetName");
    fnNvmlDeviceGetUtilizationRates_ = (nvmlDeviceGetUtilizationRates_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetUtilizationRates");
    fnNvmlDeviceGetMemoryInfo_ = (nvmlDeviceGetMemoryInfo_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetMemoryInfo");
    fnNvmlDeviceGetTemperature_ = (nvmlDeviceGetTemperature_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetTemperature");
    fnNvmlDeviceGetPowerUsage_ = (nvmlDeviceGetPowerUsage_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetPowerUsage");
    fnNvmlDeviceGetFanSpeed_ = (nvmlDeviceGetFanSpeed_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetFanSpeed");
    fnNvmlDeviceGetFanSpeed_v2_ = (nvmlDeviceGetFanSpeed_v2_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetFanSpeed_v2");
    fnNvmlDeviceGetEnforcedPowerLimit_ = (nvmlDeviceGetEnforcedPowerLimit_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetEnforcedPowerLimit");
    fnNvmlDeviceGetPowerManagementLimit_ = (nvmlDeviceGetPowerManagementLimit_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetPowerManagementLimit");
    fnNvmlDeviceGetTemperatureThreshold_ = (nvmlDeviceGetTemperatureThreshold_t)GetProcAddress(nvmlDll_, "nvmlDeviceGetTemperatureThreshold");

    if (!fnNvmlInit_ || !fnNvmlDeviceGetHandleByIndex_) {
        UnloadNvml();
        return false;
    }

    int res = fnNvmlInit_();
    if (res != 0) { // NVML_SUCCESS = 0
        UnloadNvml();
        return false;
    }

    isAvailable_ = true;
    return true;
}

void NvmlCollector::UnloadNvml() {
    if (isAvailable_ && fnNvmlShutdown_) {
        fnNvmlShutdown_();
    }
    if (nvmlDll_) {
        FreeLibrary(nvmlDll_);
        nvmlDll_ = nullptr;
    }
    isAvailable_ = false;
    deviceHandle_ = nullptr;
    activeDeviceIndex_ = 0xFFFFFFFF;
}

bool NvmlCollector::Recover() {
    UnloadNvml();
    return LoadNvml();
}

void NvmlCollector::Shutdown() {
    UnloadNvml();
}

bool NvmlCollector::Collect(GpuMetrics& metrics, uint32_t deviceIndex) {
    if (!isAvailable_ || !fnNvmlDeviceGetHandleByIndex_) {
        return false;
    }

    if (!deviceHandle_ || activeDeviceIndex_ != deviceIndex) {
        void* handle = nullptr;
        int res = fnNvmlDeviceGetHandleByIndex_(deviceIndex, &handle);
        if (res != 0 || !handle) {
            if (res == 1 || res == 9 || res == 15 || res == 16) {
                // Driver reset, GPU lost, or uninitialized
                UnloadNvml();
            }
            return false;
        }
        deviceHandle_ = handle;
        activeDeviceIndex_ = deviceIndex;
    }

    metrics.isAvailable = true;

    // Get device name if not set
    if (metrics.name.empty() && fnNvmlDeviceGetName_) {
        char nameBuf[96] = {0};
        if (fnNvmlDeviceGetName_(deviceHandle_, nameBuf, sizeof(nameBuf)) == 0) {
            metrics.name = nameBuf;
        }
    }

    // Utilization
    if (fnNvmlDeviceGetUtilizationRates_) {
        NvmlUtilization util{};
        int res = fnNvmlDeviceGetUtilizationRates_(deviceHandle_, &util);
        if (res == 0) {
            metrics.utilizationPercent = static_cast<double>(util.gpu);
        } else if (res == 1 || res == 9 || res == 15 || res == 16) {
            UnloadNvml();
            return false;
        }
    }

    // Memory
    if (fnNvmlDeviceGetMemoryInfo_) {
        NvmlMemory mem{};
        if (fnNvmlDeviceGetMemoryInfo_(deviceHandle_, &mem) == 0) {
            metrics.memoryTotalBytes = mem.total;
            metrics.memoryUsedBytes = mem.used;
        }
    }

    // Temperature (0 = NVML_TEMPERATURE_GPU)
    if (fnNvmlDeviceGetTemperature_) {
        unsigned int temp = 0;
        if (fnNvmlDeviceGetTemperature_(deviceHandle_, 0, &temp) == 0) {
            metrics.temperatureC = temp;
        }
    }

    // Temperature Slowdown Threshold (1 = NVML_TEMPERATURE_THRESHOLD_SLOWDOWN)
    if (fnNvmlDeviceGetTemperatureThreshold_) {
        unsigned int slowdownTemp = 0;
        if (fnNvmlDeviceGetTemperatureThreshold_(deviceHandle_, 1, &slowdownTemp) == 0) {
            metrics.tempSlowdownLimitC = slowdownTemp;
        }
    }

    // Power (mW -> W)
    if (fnNvmlDeviceGetPowerUsage_) {
        unsigned int powerMw = 0;
        if (fnNvmlDeviceGetPowerUsage_(deviceHandle_, &powerMw) == 0) {
            metrics.powerWatts = (powerMw + 500) / 1000; // Rounded to nearest Watt
        }
    }

    // Power Limit (mW -> W)
    if (fnNvmlDeviceGetEnforcedPowerLimit_) {
        unsigned int limitMw = 0;
        if (fnNvmlDeviceGetEnforcedPowerLimit_(deviceHandle_, &limitMw) == 0 && limitMw > 0) {
            metrics.powerLimitWatts = (limitMw + 500) / 1000;
        }
    }
    if (!metrics.powerLimitWatts.has_value() && fnNvmlDeviceGetPowerManagementLimit_) {
        unsigned int limitMw = 0;
        if (fnNvmlDeviceGetPowerManagementLimit_(deviceHandle_, &limitMw) == 0 && limitMw > 0) {
            metrics.powerLimitWatts = (limitMw + 500) / 1000;
        }
    }

    // Fan Speed
    // Try FanSpeed (percentage)
    if (fnNvmlDeviceGetFanSpeed_) {
        unsigned int fanPct = 0;
        if (fnNvmlDeviceGetFanSpeed_(deviceHandle_, &fanPct) == 0) {
            metrics.fanSpeedPercent = fanPct;
        }
    }

    // Try FanSpeed_v2 (fan 0)
    if (fnNvmlDeviceGetFanSpeed_v2_) {
        unsigned int fanSpeedVal = 0;
        if (fnNvmlDeviceGetFanSpeed_v2_(deviceHandle_, 0, &fanSpeedVal) == 0) {
            // Some drivers return RPM or % in fanSpeedVal
            if (fanSpeedVal > 100) {
                metrics.fanRpm = fanSpeedVal;
            } else if (!metrics.fanSpeedPercent.has_value()) {
                metrics.fanSpeedPercent = fanSpeedVal;
            }
        }
    }

    return true;
}

} // namespace BareStats
