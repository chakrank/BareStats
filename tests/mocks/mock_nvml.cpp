#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define MOCK_EXPORT __declspec(dllexport)
#define MOCK_CDECL __cdecl
#else
#define MOCK_EXPORT __attribute__((visibility("default")))
#define MOCK_CDECL
#endif

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

MOCK_EXPORT int MOCK_CDECL nvmlInit_v2() {
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlInit() {
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlShutdown() {
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetCount_v2(unsigned int* deviceCount) {
    if (!deviceCount) return NVML_ERROR_GENERIC;
    *deviceCount = 1;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetCount(unsigned int* deviceCount) {
    return nvmlDeviceGetCount_v2(deviceCount);
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetHandleByIndex_v2(unsigned int index, void** device) {
    if (!device || index >= 1) return NVML_ERROR_GENERIC;
    *device = (void*)0x10DE0001;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetHandleByIndex(unsigned int index, void** device) {
    return nvmlDeviceGetHandleByIndex_v2(index, device);
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetName(void* device, char* name, unsigned int length) {
    (void)device;
    if (!name || length == 0) return NVML_ERROR_GENERIC;
#ifdef _WIN32
    strncpy_s(name, length, "NVIDIA GeForce RTX 4090", _TRUNCATE);
#else
    strncpy(name, "NVIDIA GeForce RTX 4090", length - 1);
    name[length - 1] = '\0';
#endif
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetUtilizationRates(void* device, void* utilization) {
    (void)device;
    if (!utilization) return NVML_ERROR_GENERIC;
    auto* u = reinterpret_cast<NvmlUtilization*>(utilization);
    u->gpu = 85;
    u->memory = 42;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetMemoryInfo_v2(void* device, void* memory) {
    (void)device;
    if (!memory) return NVML_ERROR_GENERIC;
    auto* m = reinterpret_cast<NvmlMemory_v2*>(memory);
    m->total = 24ULL * 1024 * 1024 * 1024;
    m->reserved = 280ULL * 1024 * 1024;
    m->used = 6ULL * 1024 * 1024 * 1024;
    m->free = m->total - m->used;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetMemoryInfo(void* device, void* memory) {
    (void)device;
    if (!memory) return NVML_ERROR_GENERIC;
    auto* m = reinterpret_cast<NvmlMemory*>(memory);
    m->total = 24ULL * 1024 * 1024 * 1024;
    m->used = 6ULL * 1024 * 1024 * 1024;
    m->free = m->total - m->used;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetTemperature(void* device, int sensorType, unsigned int* temp) {
    (void)device;
    (void)sensorType;
    if (!temp) return NVML_ERROR_GENERIC;
    *temp = 62;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetPowerUsage(void* device, unsigned int* power) {
    (void)device;
    if (!power) return NVML_ERROR_GENERIC;
    *power = 285000; // 285,000 mW = 285 W
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetFanSpeed(void* device, unsigned int* speed) {
    (void)device;
    if (!speed) return NVML_ERROR_GENERIC;
    *speed = 55;
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetFanSpeed_v2(void* device, unsigned int fan, unsigned int* speed) {
    (void)fan;
    return nvmlDeviceGetFanSpeed(device, speed);
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetEnforcedPowerLimit(void* device, unsigned int* limit) {
    (void)device;
    if (!limit) return NVML_ERROR_GENERIC;
    *limit = 450000; // 450 W
    return NVML_SUCCESS;
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetPowerManagementLimit(void* device, unsigned int* limit) {
    return nvmlDeviceGetEnforcedPowerLimit(device, limit);
}

MOCK_EXPORT int MOCK_CDECL nvmlDeviceGetTemperatureThreshold(void* device, int thresholdType, unsigned int* temp) {
    (void)device;
    (void)thresholdType;
    if (!temp) return NVML_ERROR_GENERIC;
    *temp = 88;
    return NVML_SUCCESS;
}

} // extern "C"
