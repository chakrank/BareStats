#pragma once

#include "metrics.h"
#include <windows.h>
#include <string>

namespace BareStats {

class NvmlCollector {
public:
    NvmlCollector();
    ~NvmlCollector();

    bool IsAvailable() const { return isAvailable_; }
    bool Collect(GpuMetrics& metrics, uint32_t deviceIndex = 0);

private:
    HMODULE nvmlDll_ = nullptr;
    bool isAvailable_ = false;
    void* deviceHandle_ = nullptr;
    uint32_t activeDeviceIndex_ = 0;

    // Function pointer types
    typedef int (*nvmlInit_t)();
    typedef int (*nvmlShutdown_t)();
    typedef int (*nvmlDeviceGetCount_t)(unsigned int*);
    typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, void**);
    typedef int (*nvmlDeviceGetName_t)(void*, char*, unsigned int);
    typedef int (*nvmlDeviceGetUtilizationRates_t)(void*, void*);
    typedef int (*nvmlDeviceGetMemoryInfo_t)(void*, void*);
    typedef int (*nvmlDeviceGetTemperature_t)(void*, int, unsigned int*);
    typedef int (*nvmlDeviceGetPowerUsage_t)(void*, unsigned int*);
    typedef int (*nvmlDeviceGetFanSpeed_t)(void*, unsigned int*);
    typedef int (*nvmlDeviceGetFanSpeed_v2_t)(void*, unsigned int, unsigned int*);

    nvmlInit_t fnNvmlInit_ = nullptr;
    nvmlShutdown_t fnNvmlShutdown_ = nullptr;
    nvmlDeviceGetCount_t fnNvmlDeviceGetCount_ = nullptr;
    nvmlDeviceGetHandleByIndex_t fnNvmlDeviceGetHandleByIndex_ = nullptr;
    nvmlDeviceGetName_t fnNvmlDeviceGetName_ = nullptr;
    nvmlDeviceGetUtilizationRates_t fnNvmlDeviceGetUtilizationRates_ = nullptr;
    nvmlDeviceGetMemoryInfo_t fnNvmlDeviceGetMemoryInfo_ = nullptr;
    nvmlDeviceGetTemperature_t fnNvmlDeviceGetTemperature_ = nullptr;
    nvmlDeviceGetPowerUsage_t fnNvmlDeviceGetPowerUsage_ = nullptr;
    nvmlDeviceGetFanSpeed_t fnNvmlDeviceGetFanSpeed_ = nullptr;
    nvmlDeviceGetFanSpeed_v2_t fnNvmlDeviceGetFanSpeed_v2_ = nullptr;

    bool LoadNvml();
    void UnloadNvml();
};

} // namespace BareStats
