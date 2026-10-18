#include "nvml_collector.h"
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define LOAD_LIB(name) LoadLibraryA(name)
#define GET_SYM(handle, name) GetProcAddress(static_cast<HMODULE>(handle), name)
#define FREE_LIB(handle) FreeLibrary(static_cast<HMODULE>(handle))
#else
#include <dlfcn.h>
#include <cstdlib>
#define LOAD_LIB(name) dlopen(name, RTLD_LAZY | RTLD_LOCAL)
#define GET_SYM(handle, name) dlsym(handle, name)
#define FREE_LIB(handle) dlclose(handle)
#endif

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

struct NvmlMemory_v2 {
    unsigned int version;
    unsigned long long total;
    unsigned long long reserved;
    unsigned long long free;
    unsigned long long used;
};

NvmlCollector::NvmlCollector(const char* customDllPath) {
    LoadNvml(customDllPath);
}

NvmlCollector::~NvmlCollector() {
    UnloadNvml();
}

bool NvmlCollector::LoadNvml(const char* customDllPath) {
    if (customDllPath && customDllPath[0] != '\0') {
        customDllPath_ = customDllPath;
    }

#ifdef _WIN32
    if (!customDllPath_.empty()) {
        nvmlDll_ = LOAD_LIB(customDllPath_.c_str());
    } else {
        char envBuf[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("BARESTATS_MOCK_NVML_DLL", envBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            nvmlDll_ = LOAD_LIB(envBuf);
        } else {
            nvmlDll_ = LOAD_LIB("nvml.dll");
            if (!nvmlDll_) {
                nvmlDll_ = LOAD_LIB("C:\\Windows\\System32\\nvml.dll");
            }
            if (!nvmlDll_) {
                nvmlDll_ = LOAD_LIB("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
            }
        }
    }
#else
    if (!customDllPath_.empty()) {
        nvmlDll_ = LOAD_LIB(customDllPath_.c_str());
    } else {
        const char* envMock = std::getenv("BARESTATS_MOCK_NVML_SO");
        if (envMock && envMock[0] != '\0') {
            nvmlDll_ = LOAD_LIB(envMock);
        } else {
            nvmlDll_ = LOAD_LIB("libnvidia-ml.so.1");
            if (!nvmlDll_) {
                nvmlDll_ = LOAD_LIB("libnvidia-ml.so");
            }
        }
    }
#endif

    if (!nvmlDll_) {
        isAvailable_ = false;
        return false;
    }

    // Resolve functions
    fnNvmlInit_ = (nvmlInit_t)GET_SYM(nvmlDll_, "nvmlInit_v2");
    if (!fnNvmlInit_) {
        fnNvmlInit_ = (nvmlInit_t)GET_SYM(nvmlDll_, "nvmlInit");
    }

    fnNvmlShutdown_ = (nvmlShutdown_t)GET_SYM(nvmlDll_, "nvmlShutdown");
    
    fnNvmlDeviceGetCount_ = (nvmlDeviceGetCount_t)GET_SYM(nvmlDll_, "nvmlDeviceGetCount_v2");
    if (!fnNvmlDeviceGetCount_) {
        fnNvmlDeviceGetCount_ = (nvmlDeviceGetCount_t)GET_SYM(nvmlDll_, "nvmlDeviceGetCount");
    }

    fnNvmlDeviceGetHandleByIndex_ = (nvmlDeviceGetHandleByIndex_t)GET_SYM(nvmlDll_, "nvmlDeviceGetHandleByIndex_v2");
    if (!fnNvmlDeviceGetHandleByIndex_) {
        fnNvmlDeviceGetHandleByIndex_ = (nvmlDeviceGetHandleByIndex_t)GET_SYM(nvmlDll_, "nvmlDeviceGetHandleByIndex");
    }

    fnNvmlDeviceGetName_ = (nvmlDeviceGetName_t)GET_SYM(nvmlDll_, "nvmlDeviceGetName");
    fnNvmlDeviceGetUtilizationRates_ = (nvmlDeviceGetUtilizationRates_t)GET_SYM(nvmlDll_, "nvmlDeviceGetUtilizationRates");
    fnNvmlDeviceGetMemoryInfo_ = (nvmlDeviceGetMemoryInfo_t)GET_SYM(nvmlDll_, "nvmlDeviceGetMemoryInfo");
    fnNvmlDeviceGetMemoryInfo_v2_ = (nvmlDeviceGetMemoryInfo_v2_t)GET_SYM(nvmlDll_, "nvmlDeviceGetMemoryInfo_v2");
    fnNvmlDeviceGetTemperature_ = (nvmlDeviceGetTemperature_t)GET_SYM(nvmlDll_, "nvmlDeviceGetTemperature");
    fnNvmlDeviceGetPowerUsage_ = (nvmlDeviceGetPowerUsage_t)GET_SYM(nvmlDll_, "nvmlDeviceGetPowerUsage");
    fnNvmlDeviceGetFanSpeed_ = (nvmlDeviceGetFanSpeed_t)GET_SYM(nvmlDll_, "nvmlDeviceGetFanSpeed");
    fnNvmlDeviceGetFanSpeed_v2_ = (nvmlDeviceGetFanSpeed_v2_t)GET_SYM(nvmlDll_, "nvmlDeviceGetFanSpeed_v2");
    fnNvmlDeviceGetEnforcedPowerLimit_ = (nvmlDeviceGetEnforcedPowerLimit_t)GET_SYM(nvmlDll_, "nvmlDeviceGetEnforcedPowerLimit");
    fnNvmlDeviceGetPowerManagementLimit_ = (nvmlDeviceGetPowerManagementLimit_t)GET_SYM(nvmlDll_, "nvmlDeviceGetPowerManagementLimit");
    fnNvmlDeviceGetTemperatureThreshold_ = (nvmlDeviceGetTemperatureThreshold_t)GET_SYM(nvmlDll_, "nvmlDeviceGetTemperatureThreshold");

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
        FREE_LIB(nvmlDll_);
        nvmlDll_ = nullptr;
    }
    isAvailable_ = false;
    deviceHandle_ = nullptr;
    activeDeviceIndex_ = 0xFFFFFFFF;
    fnNvmlDeviceGetMemoryInfo_v2_ = nullptr;
}

bool NvmlCollector::Recover() {
    UnloadNvml();
    return LoadNvml(customDllPath_.empty() ? nullptr : customDllPath_.c_str());
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

    // Memory (prefer v2 to exclude internal driver/hardware reservations)
    if (fnNvmlDeviceGetMemoryInfo_v2_) {
        NvmlMemory_v2 mem2{};
        mem2.version = static_cast<unsigned int>(sizeof(NvmlMemory_v2) | (2 << 24));
        if (fnNvmlDeviceGetMemoryInfo_v2_(deviceHandle_, &mem2) == 0) {
            metrics.memoryTotalBytes = mem2.total;
            metrics.memoryReservedBytes = mem2.reserved;
            metrics.memoryUsedBytes = mem2.used;
        }
    } else if (fnNvmlDeviceGetMemoryInfo_) {
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
