#include "igcl_collector.h"
#include <cmath>

namespace BareStats {

IgclCollector::IgclCollector(const char* customDllPath) {
    LoadIgcl(customDllPath);
}

IgclCollector::~IgclCollector() {
    Shutdown();
}

bool IgclCollector::LoadIgcl(const char* customDllPath) {
    if (isAvailable_) return true;

    if (customDllPath && customDllPath[0] != '\0') {
        customDllPath_ = customDllPath;
    }

    if (!customDllPath_.empty()) {
        igclDll_ = LoadLibraryA(customDllPath_.c_str());
    } else {
        char envBuf[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("BARESTATS_MOCK_IGCL_DLL", envBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            igclDll_ = LoadLibraryA(envBuf);
        } else {
            igclDll_ = LoadLibraryA("ControlLib.dll");
        }
    }

    if (!igclDll_) {
        return false;
    }

    fnCtlInit_ = (ctlInit_t)GetProcAddress(igclDll_, "ctlInit");
    fnCtlClose_ = (ctlClose_t)GetProcAddress(igclDll_, "ctlClose");
    fnCtlEnumerateDevices_ = (ctlEnumerateDevices_t)GetProcAddress(igclDll_, "ctlEnumerateDevices");
    fnCtlGetTemperature_ = (ctlGetTemperature_t)GetProcAddress(igclDll_, "ctlGetTemperature");
    fnCtlGetPowerTelemetry_ = (ctlGetPowerTelemetry_t)GetProcAddress(igclDll_, "ctlGetPowerTelemetry");
    fnCtlGetFanSpeed_ = (ctlGetFanSpeed_t)GetProcAddress(igclDll_, "ctlGetFanSpeed");

    if (!fnCtlInit_ || !fnCtlClose_ || !fnCtlEnumerateDevices_) {
        UnloadIgcl();
        return false;
    }

    CtlInitArgs initArgs{};
    initArgs.Size = sizeof(CtlInitArgs);
    initArgs.Version = 0;
    initArgs.AppVersion = 0x00010000; // 1.0

    if (fnCtlInit_(&initArgs, &apiHandle_) != 0 || !apiHandle_) {
        UnloadIgcl();
        return false;
    }

    isAvailable_ = true;
    return true;
}

void IgclCollector::UnloadIgcl() {
    if (fnCtlClose_ && apiHandle_) {
        fnCtlClose_(apiHandle_);
        apiHandle_ = nullptr;
    }

    if (igclDll_) {
        FreeLibrary(igclDll_);
        igclDll_ = nullptr;
    }

    isAvailable_ = false;
}

bool IgclCollector::Recover() {
    Shutdown();
    return LoadIgcl(customDllPath_.empty() ? nullptr : customDllPath_.c_str());
}

void IgclCollector::Shutdown() {
    UnloadIgcl();
}

bool IgclCollector::Collect(GpuMetrics& metrics, uint32_t deviceIndex) {
    if (!isAvailable_ || !apiHandle_) {
        return false;
    }

    uint32_t deviceCount = 0;
    if (fnCtlEnumerateDevices_(apiHandle_, &deviceCount, nullptr) != 0 || deviceCount == 0) {
        return false;
    }

    std::vector<void*> devices(deviceCount, nullptr);
    if (fnCtlEnumerateDevices_(apiHandle_, &deviceCount, devices.data()) != 0) {
        return false;
    }

    uint32_t targetIdx = (deviceIndex < deviceCount) ? deviceIndex : 0;
    void* hDevice = devices[targetIdx];
    if (!hDevice) {
        return false;
    }

    metrics.isAvailable = true;

    // 1. Temperature
    if (fnCtlGetTemperature_) {
        CtlThermalArgs thermArgs{};
        thermArgs.Size = sizeof(CtlThermalArgs);
        thermArgs.Version = 0;
        thermArgs.SensorType = 0; // Global/Package sensor
        double tempVal = 0.0;
        if (fnCtlGetTemperature_(hDevice, &thermArgs, &tempVal) == 0 && tempVal > 0.0) {
            metrics.temperatureC = static_cast<uint32_t>(std::round(tempVal));
        }
    }

    // 2. Power Telemetry
    if (fnCtlGetPowerTelemetry_) {
        CtlPowerArgs pwrArgs{};
        pwrArgs.Size = sizeof(CtlPowerArgs);
        pwrArgs.Version = 0;
        CtlPowerTelemetry pwrTelem{};
        pwrTelem.Size = sizeof(CtlPowerTelemetry);
        pwrTelem.Version = 0;
        if (fnCtlGetPowerTelemetry_(hDevice, &pwrArgs, &pwrTelem) == 0 && pwrTelem.bSupported) {
            if (pwrTelem.CurrentPowerWatts > 0.0) {
                metrics.powerWatts = static_cast<uint32_t>(std::round(pwrTelem.CurrentPowerWatts));
            }
            if (pwrTelem.SustainedPowerLimitWatts > 0.0) {
                metrics.powerLimitWatts = static_cast<uint32_t>(std::round(pwrTelem.SustainedPowerLimitWatts));
            }
        }
    }

    // 3. Fan Speed
    if (fnCtlGetFanSpeed_) {
        CtlFanSpeed fanSpeed{};
        fanSpeed.Size = sizeof(CtlFanSpeed);
        fanSpeed.Version = 0;
        if (fnCtlGetFanSpeed_(hDevice, nullptr, &fanSpeed) == 0) {
            if (fanSpeed.SpeedRPM > 0) {
                metrics.fanRpm = static_cast<uint32_t>(fanSpeed.SpeedRPM);
            }
            if (fanSpeed.SpeedPercent >= 0) {
                metrics.fanSpeedPercent = static_cast<uint32_t>(fanSpeed.SpeedPercent);
            }
        }
    }

    return true;
}

} // namespace BareStats
