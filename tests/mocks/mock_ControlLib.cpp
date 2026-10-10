#include <windows.h>
#include <cstdint>
#include <cstring>

#define CTL_RESULT_SUCCESS 0
#define CTL_RESULT_ERROR_GENERIC -1

struct CtlInitArgs {
    uint32_t Size;
    uint8_t Version;
    uint8_t Reserved[3];
    uint32_t AppVersion;
    uint32_t Flags;
    void* SupportedDeviceTypes;
};

struct CtlThermalArgs {
    uint32_t Size;
    uint8_t Version;
    uint8_t Reserved[3];
    uint32_t SensorType;
};

struct CtlPowerArgs {
    uint32_t Size;
    uint8_t Version;
    uint8_t Reserved[3];
};

struct CtlPowerTelemetry {
    uint32_t Size;
    uint8_t Version;
    uint8_t Reserved[3];
    bool bSupported;
    double CurrentPowerWatts;
    double SustainedPowerLimitWatts;
    double BurstPowerLimitWatts;
};

struct CtlFanSpeed {
    uint32_t Size;
    uint8_t Version;
    uint8_t Reserved[3];
    int32_t SpeedPercent;
    int32_t SpeedRPM;
};

extern "C" {

__declspec(dllexport) int __cdecl ctlInit(CtlInitArgs* pArgs, void** phAPIHandle) {
    if (!pArgs || !phAPIHandle) return CTL_RESULT_ERROR_GENERIC;
    *phAPIHandle = (void*)0x80860000;
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) int __cdecl ctlClose(void* hAPIHandle) {
    (void)hAPIHandle;
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) int __cdecl ctlEnumerateDevices(void* hAPIHandle, uint32_t* pCount, void** phDevices) {
    (void)hAPIHandle;
    if (!pCount) return CTL_RESULT_ERROR_GENERIC;
    if (!phDevices) {
        *pCount = 1;
        return CTL_RESULT_SUCCESS;
    }
    if (*pCount > 0) {
        phDevices[0] = (void*)0x80860001;
        *pCount = 1;
    }
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) int __cdecl ctlGetTemperature(void* hDevice, CtlThermalArgs* pArgs, double* pTemperature) {
    (void)hDevice;
    (void)pArgs;
    if (!pTemperature) return CTL_RESULT_ERROR_GENERIC;
    *pTemperature = 58.0; // 58.0 C
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) int __cdecl ctlGetPowerTelemetry(void* hDevice, CtlPowerArgs* pArgs, CtlPowerTelemetry* pPowerTelemetry) {
    (void)hDevice;
    (void)pArgs;
    if (!pPowerTelemetry) return CTL_RESULT_ERROR_GENERIC;
    pPowerTelemetry->bSupported = true;
    pPowerTelemetry->CurrentPowerWatts = 140.0;
    pPowerTelemetry->SustainedPowerLimitWatts = 225.0;
    pPowerTelemetry->BurstPowerLimitWatts = 250.0;
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) int __cdecl ctlGetFanSpeed(void* hDevice, void* pArgs, CtlFanSpeed* pFanSpeed) {
    (void)hDevice;
    (void)pArgs;
    if (!pFanSpeed) return CTL_RESULT_ERROR_GENERIC;
    pFanSpeed->SpeedRPM = 1200;
    pFanSpeed->SpeedPercent = 45;
    return CTL_RESULT_SUCCESS;
}

} // extern "C"
