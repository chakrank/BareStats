#include <windows.h>
#include <cstdint>
#include <cstring>

#define NVML_SUCCESS 0
#define NVML_ERROR_GENERIC 1

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

extern "C" {

__declspec(dllexport) int __cdecl nvmlInit_v2() {
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlInit() {
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlShutdown() {
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetCount_v2(unsigned int* deviceCount) {
    if (!deviceCount) return NVML_ERROR_GENERIC;
    *deviceCount = 1;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetCount(unsigned int* deviceCount) {
    return nvmlDeviceGetCount_v2(deviceCount);
}

__declspec(dllexport) int __cdecl nvmlDeviceGetHandleByIndex_v2(unsigned int index, void** device) {
    (void)index;
    if (!device) return NVML_ERROR_GENERIC;
    *device = (void*)0x10DE0001;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetHandleByIndex(unsigned int index, void** device) {
    return nvmlDeviceGetHandleByIndex_v2(index, device);
}

__declspec(dllexport) int __cdecl nvmlDeviceGetName(void* device, char* name, unsigned int length) {
    (void)device;
    if (!name || length == 0) return NVML_ERROR_GENERIC;
    strncpy_s(name, length, "NVIDIA GeForce RTX 4090", _TRUNCATE);
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetUtilizationRates(void* device, void* utilization) {
    (void)device;
    if (!utilization) return NVML_ERROR_GENERIC;
    auto* u = reinterpret_cast<NvmlUtilization*>(utilization);
    u->gpu = 85;
    u->memory = 42;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetMemoryInfo_v2(void* device, void* memory) {
    (void)device;
    if (!memory) return NVML_ERROR_GENERIC;
    auto* m = reinterpret_cast<NvmlMemory_v2*>(memory);
    m->total = 24ULL * 1024 * 1024 * 1024;
    m->reserved = 280ULL * 1024 * 1024;
    m->used = 6ULL * 1024 * 1024 * 1024;
    m->free = m->total - m->used;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetMemoryInfo(void* device, void* memory) {
    (void)device;
    if (!memory) return NVML_ERROR_GENERIC;
    auto* m = reinterpret_cast<NvmlMemory*>(memory);
    m->total = 24ULL * 1024 * 1024 * 1024;
    m->used = 6ULL * 1024 * 1024 * 1024;
    m->free = m->total - m->used;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetTemperature(void* device, int sensorType, unsigned int* temp) {
    (void)device;
    (void)sensorType;
    if (!temp) return NVML_ERROR_GENERIC;
    *temp = 62;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetPowerUsage(void* device, unsigned int* power) {
    (void)device;
    if (!power) return NVML_ERROR_GENERIC;
    *power = 285000; // 285,000 mW = 285 W
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetFanSpeed(void* device, unsigned int* speed) {
    (void)device;
    if (!speed) return NVML_ERROR_GENERIC;
    *speed = 55;
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetFanSpeed_v2(void* device, unsigned int fan, unsigned int* speed) {
    (void)fan;
    return nvmlDeviceGetFanSpeed(device, speed);
}

__declspec(dllexport) int __cdecl nvmlDeviceGetEnforcedPowerLimit(void* device, unsigned int* limit) {
    (void)device;
    if (!limit) return NVML_ERROR_GENERIC;
    *limit = 450000; // 450 W
    return NVML_SUCCESS;
}

__declspec(dllexport) int __cdecl nvmlDeviceGetPowerManagementLimit(void* device, unsigned int* limit) {
    return nvmlDeviceGetEnforcedPowerLimit(device, limit);
}

__declspec(dllexport) int __cdecl nvmlDeviceGetTemperatureThreshold(void* device, int thresholdType, unsigned int* temp) {
    (void)device;
    (void)thresholdType;
    if (!temp) return NVML_ERROR_GENERIC;
    *temp = 88;
    return NVML_SUCCESS;
}

} // extern "C"
