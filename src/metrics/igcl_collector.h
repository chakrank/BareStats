#pragma once

#include "metrics.h"
#include <windows.h>
#include <vector>
#include <string>

namespace BareStats {

class IgclCollector {
public:
    explicit IgclCollector(const char* customDllPath = nullptr);
    ~IgclCollector();

    bool IsAvailable() const { return isAvailable_; }
    bool Collect(GpuMetrics& metrics, uint32_t deviceIndex = 0);
    bool Recover();
    void Shutdown();
    bool LoadIgcl(const char* customDllPath = nullptr);

private:
    HMODULE igclDll_ = nullptr;
    std::string customDllPath_;
    void* apiHandle_ = nullptr;
    bool isAvailable_ = false;

    // Structs for IGCL C API
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

    // Function pointer types
    typedef int (*ctlInit_t)(CtlInitArgs*, void**);
    typedef int (*ctlClose_t)(void*);
    typedef int (*ctlEnumerateDevices_t)(void*, uint32_t*, void**);
    typedef int (*ctlGetTemperature_t)(void*, CtlThermalArgs*, double*);
    typedef int (*ctlGetPowerTelemetry_t)(void*, CtlPowerArgs*, CtlPowerTelemetry*);
    typedef int (*ctlGetFanSpeed_t)(void*, void*, CtlFanSpeed*);

    ctlInit_t fnCtlInit_ = nullptr;
    ctlClose_t fnCtlClose_ = nullptr;
    ctlEnumerateDevices_t fnCtlEnumerateDevices_ = nullptr;
    ctlGetTemperature_t fnCtlGetTemperature_ = nullptr;
    ctlGetPowerTelemetry_t fnCtlGetPowerTelemetry_ = nullptr;
    ctlGetFanSpeed_t fnCtlGetFanSpeed_ = nullptr;

    bool LoadIgcl();
    void UnloadIgcl();
};

} // namespace BareStats
