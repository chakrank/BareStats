#include "adl_collector.h"
#include <cstdlib>
#include <vector>
#include <set>

namespace BareStats {

static void* __cdecl AdlMalloc(int size) {
    return std::malloc(size);
}

AdlCollector::AdlCollector(const char* customDllPath) {
    LoadAdl(customDllPath);
}

AdlCollector::~AdlCollector() {
    Shutdown();
}

bool AdlCollector::LoadAdl(const char* customDllPath) {
    if (isAvailable_) return true;

    if (customDllPath && customDllPath[0] != '\0') {
        customDllPath_ = customDllPath;
    }

    if (!customDllPath_.empty()) {
        adlDll_ = LoadLibraryA(customDllPath_.c_str());
    } else {
        char envBuf[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("BARESTATS_MOCK_ADL_DLL", envBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            adlDll_ = LoadLibraryA(envBuf);
        } else {
            adlDll_ = LoadLibraryA("atiadlxx.dll");
            if (!adlDll_) {
                adlDll_ = LoadLibraryA("atiadlxy.dll");
            }
        }
    }

    if (!adlDll_) {
        return false;
    }

    fnAdl2MainControlCreate_ = (ADL2_Main_Control_Create_t)GetProcAddress(adlDll_, "ADL2_Main_Control_Create");
    fnAdl2MainControlDestroy_ = (ADL2_Main_Control_Destroy_t)GetProcAddress(adlDll_, "ADL2_Main_Control_Destroy");
    fnAdlMainControlCreate_ = (ADL_Main_Control_Create_t)GetProcAddress(adlDll_, "ADL_Main_Control_Create");
    fnAdlMainControlDestroy_ = (ADL_Main_Control_Destroy_t)GetProcAddress(adlDll_, "ADL_Main_Control_Destroy");

    fnAdl2AdapterNumberOfAdaptersGet_ = (ADL2_Adapter_NumberOfAdapters_Get_t)GetProcAddress(adlDll_, "ADL2_Adapter_NumberOfAdapters_Get");
    fnAdlAdapterNumberOfAdaptersGet_ = (ADL_Adapter_NumberOfAdapters_Get_t)GetProcAddress(adlDll_, "ADL_Adapter_NumberOfAdapters_Get");
    fnAdl2AdapterAdapterInfoGet_ = (ADL2_Adapter_AdapterInfo_Get_t)GetProcAddress(adlDll_, "ADL2_Adapter_AdapterInfo_Get");
    fnAdlAdapterAdapterInfoGet_ = (ADL_Adapter_AdapterInfo_Get_t)GetProcAddress(adlDll_, "ADL_Adapter_AdapterInfo_Get");

    fnAdl2OverdriveNTemperatureGet_ = (ADL2_OverdriveN_Temperature_Get_t)GetProcAddress(adlDll_, "ADL2_OverdriveN_Temperature_Get");
    fnAdlOverdrive6TemperatureGet_ = (ADL_Overdrive6_Temperature_Get_t)GetProcAddress(adlDll_, "ADL_Overdrive6_Temperature_Get");
    fnAdl2Overdrive6CurrentPowerGet_ = (ADL2_Overdrive6_CurrentPower_Get_t)GetProcAddress(adlDll_, "ADL2_Overdrive6_CurrentPower_Get");
    fnAdlOverdrive6CurrentPowerGet_ = (ADL_Overdrive6_CurrentPower_Get_t)GetProcAddress(adlDll_, "ADL_Overdrive6_CurrentPower_Get");
    fnAdl2OverdriveNFanSpeedGet_ = (ADL2_OverdriveN_FanSpeed_Get_t)GetProcAddress(adlDll_, "ADL2_OverdriveN_FanSpeed_Get");
    fnAdlOverdrive6FanSpeedGet_ = (ADL_Overdrive6_FanSpeed_Get_t)GetProcAddress(adlDll_, "ADL_Overdrive6_FanSpeed_Get");
    fnAdl2Overdrive6CurrentStatusGet_ = (ADL2_Overdrive6_CurrentStatus_Get_t)GetProcAddress(adlDll_, "ADL2_Overdrive6_CurrentStatus_Get");
    fnAdlOverdrive6CurrentStatusGet_ = (ADL_Overdrive6_CurrentStatus_Get_t)GetProcAddress(adlDll_, "ADL_Overdrive6_CurrentStatus_Get");
    fnAdl2Overdrive5CurrentActivityGet_ = (ADL2_Overdrive5_CurrentActivity_Get_t)GetProcAddress(adlDll_, "ADL2_Overdrive5_CurrentActivity_Get");
    fnAdlOverdrive5CurrentActivityGet_ = (ADL_Overdrive5_CurrentActivity_Get_t)GetProcAddress(adlDll_, "ADL_Overdrive5_CurrentActivity_Get");
    fnAdl2AdapterMemoryInfoGet_ = (ADL2_Adapter_MemoryInfo_Get_t)GetProcAddress(adlDll_, "ADL2_Adapter_MemoryInfo_Get");
    fnAdlAdapterMemoryInfoGet_ = (ADL_Adapter_MemoryInfo_Get_t)GetProcAddress(adlDll_, "ADL_Adapter_MemoryInfo_Get");

    int initRes = -1;
    if (fnAdl2MainControlCreate_) {
        initRes = fnAdl2MainControlCreate_(AdlMalloc, 1, &context_);
    } else if (fnAdlMainControlCreate_) {
        initRes = fnAdlMainControlCreate_(AdlMalloc, 1);
    }

    if (initRes != 0) {
        UnloadAdl();
        return false;
    }

    isAvailable_ = true;
    return true;
}

void AdlCollector::UnloadAdl() {
    if (isAvailable_) {
        if (fnAdl2MainControlDestroy_ && context_) {
            fnAdl2MainControlDestroy_(context_);
            context_ = nullptr;
        } else if (fnAdlMainControlDestroy_) {
            fnAdlMainControlDestroy_();
        }
    }
    context_ = nullptr;

    if (adlDll_) {
        FreeLibrary(adlDll_);
        adlDll_ = nullptr;
    }

    fnAdl2MainControlCreate_ = nullptr;
    fnAdl2MainControlDestroy_ = nullptr;
    fnAdlMainControlCreate_ = nullptr;
    fnAdlMainControlDestroy_ = nullptr;

    fnAdl2AdapterNumberOfAdaptersGet_ = nullptr;
    fnAdlAdapterNumberOfAdaptersGet_ = nullptr;
    fnAdl2AdapterAdapterInfoGet_ = nullptr;
    fnAdlAdapterAdapterInfoGet_ = nullptr;

    fnAdl2OverdriveNTemperatureGet_ = nullptr;
    fnAdlOverdrive6TemperatureGet_ = nullptr;
    fnAdl2Overdrive6CurrentPowerGet_ = nullptr;
    fnAdlOverdrive6CurrentPowerGet_ = nullptr;
    fnAdl2OverdriveNFanSpeedGet_ = nullptr;
    fnAdlOverdrive6FanSpeedGet_ = nullptr;
    fnAdl2Overdrive6CurrentStatusGet_ = nullptr;
    fnAdlOverdrive6CurrentStatusGet_ = nullptr;
    fnAdl2Overdrive5CurrentActivityGet_ = nullptr;
    fnAdlOverdrive5CurrentActivityGet_ = nullptr;
    fnAdl2AdapterMemoryInfoGet_ = nullptr;
    fnAdlAdapterMemoryInfoGet_ = nullptr;

    isAvailable_ = false;
}

bool AdlCollector::Recover() {
    Shutdown();
    return LoadAdl(customDllPath_.empty() ? nullptr : customDllPath_.c_str());
}

void AdlCollector::Shutdown() {
    UnloadAdl();
}

int AdlCollector::ResolveAmdAdapterIndex(uint32_t deviceIndex) {
    int numAdapters = 0;
    if (fnAdl2AdapterNumberOfAdaptersGet_ && context_) {
        if (fnAdl2AdapterNumberOfAdaptersGet_(context_, &numAdapters) != 0 || numAdapters <= 0) {
            return -1;
        }
    } else if (fnAdlAdapterNumberOfAdaptersGet_) {
        if (fnAdlAdapterNumberOfAdaptersGet_(&numAdapters) != 0 || numAdapters <= 0) {
            return -1;
        }
    } else {
        return -1;
    }

    std::vector<AdlAdapterInfo> adapterInfos(numAdapters);
    for (auto& info : adapterInfos) {
        info.iSize = sizeof(AdlAdapterInfo);
    }

    int getInfoRes = -1;
    if (fnAdl2AdapterAdapterInfoGet_ && context_) {
        getInfoRes = fnAdl2AdapterAdapterInfoGet_(context_, adapterInfos.data(), static_cast<int>(sizeof(AdlAdapterInfo) * numAdapters));
    } else if (fnAdlAdapterAdapterInfoGet_) {
        getInfoRes = fnAdlAdapterAdapterInfoGet_(adapterInfos.data(), static_cast<int>(sizeof(AdlAdapterInfo) * numAdapters));
    }

    if (getInfoRes != 0) {
        return -1;
    }

    // Filter to unique physical AMD adapters by bus number
    std::set<int> seenBuses;
    std::vector<int> amdAdapterIndices;

    for (const auto& info : adapterInfos) {
        if (info.iPresent && info.iVendorID == 0x1002) {
            if (seenBuses.insert(info.iBusNumber).second) {
                amdAdapterIndices.push_back(info.iAdapterIndex);
            }
        }
    }

    if (amdAdapterIndices.empty()) {
        return -1;
    }

    if (deviceIndex < amdAdapterIndices.size()) {
        return amdAdapterIndices[deviceIndex];
    }
    return amdAdapterIndices[0];
}

bool AdlCollector::Collect(GpuMetrics& metrics, uint32_t deviceIndex) {
    if (!isAvailable_) {
        return false;
    }

    int adlAdapterIndex = ResolveAmdAdapterIndex(deviceIndex);
    if (adlAdapterIndex < 0) {
        return false;
    }

    metrics.isAvailable = true;

    // 1. Utilization
    if (fnAdl2Overdrive6CurrentStatusGet_ && context_) {
        AdlPMActivity act{};
        act.iSize = sizeof(AdlPMActivity);
        if (fnAdl2Overdrive6CurrentStatusGet_(context_, adlAdapterIndex, &act) == 0) {
            metrics.utilizationPercent = static_cast<double>(act.iActivityPercent);
        }
    } else if (fnAdlOverdrive6CurrentStatusGet_) {
        AdlPMActivity act{};
        act.iSize = sizeof(AdlPMActivity);
        if (fnAdlOverdrive6CurrentStatusGet_(adlAdapterIndex, &act) == 0) {
            metrics.utilizationPercent = static_cast<double>(act.iActivityPercent);
        }
    } else if (fnAdl2Overdrive5CurrentActivityGet_ && context_) {
        AdlPMActivity act{};
        act.iSize = sizeof(AdlPMActivity);
        if (fnAdl2Overdrive5CurrentActivityGet_(context_, adlAdapterIndex, &act) == 0) {
            metrics.utilizationPercent = static_cast<double>(act.iActivityPercent);
        }
    } else if (fnAdlOverdrive5CurrentActivityGet_) {
        AdlPMActivity act{};
        act.iSize = sizeof(AdlPMActivity);
        if (fnAdlOverdrive5CurrentActivityGet_(adlAdapterIndex, &act) == 0) {
            metrics.utilizationPercent = static_cast<double>(act.iActivityPercent);
        }
    }

    // 2. Temperature (OverdriveN thermal controller index 1 is core edge temperature)
    if (fnAdl2OverdriveNTemperatureGet_ && context_) {
        int tempMilliC = 0;
        if (fnAdl2OverdriveNTemperatureGet_(context_, adlAdapterIndex, 1, &tempMilliC) == 0) {
            metrics.temperatureC = static_cast<uint32_t>(tempMilliC / 1000);
        }
    } else if (fnAdlOverdrive6TemperatureGet_) {
        AdlTemperature temp{};
        temp.iSize = sizeof(AdlTemperature);
        if (fnAdlOverdrive6TemperatureGet_(adlAdapterIndex, &temp) == 0) {
            metrics.temperatureC = static_cast<uint32_t>(temp.iTemperature / 1000);
        }
    }

    // 3. Board Power Wattage
    if (fnAdl2Overdrive6CurrentPowerGet_ && context_) {
        int powerValue = 0; // Type 0 = current power
        if (fnAdl2Overdrive6CurrentPowerGet_(context_, adlAdapterIndex, 0, &powerValue) == 0) {
            if (powerValue > 50000) {
                metrics.powerWatts = static_cast<uint32_t>((powerValue + 500) / 1000);
            } else if (powerValue > 256) {
                metrics.powerWatts = static_cast<uint32_t>(powerValue >> 8);
            } else {
                metrics.powerWatts = static_cast<uint32_t>(powerValue);
            }
        }
    } else if (fnAdlOverdrive6CurrentPowerGet_) {
        int powerValue = 0;
        if (fnAdlOverdrive6CurrentPowerGet_(adlAdapterIndex, 0, &powerValue) == 0) {
            if (powerValue > 50000) {
                metrics.powerWatts = static_cast<uint32_t>((powerValue + 500) / 1000);
            } else if (powerValue > 256) {
                metrics.powerWatts = static_cast<uint32_t>(powerValue >> 8);
            } else {
                metrics.powerWatts = static_cast<uint32_t>(powerValue);
            }
        }
    }

    // 4. Fan Speed
    if (fnAdl2OverdriveNFanSpeedGet_ && context_) {
        AdlFanSpeedValue fanVal{};
        fanVal.iSize = sizeof(AdlFanSpeedValue);
        fanVal.iSpeedType = 2; // Query RPM
        if (fnAdl2OverdriveNFanSpeedGet_(context_, adlAdapterIndex, 1, &fanVal) == 0 && fanVal.iFanSpeed >= 0) {
            metrics.fanRpm = static_cast<uint32_t>(fanVal.iFanSpeed);
        }

        fanVal.iSpeedType = 1; // Query percentage
        if (fnAdl2OverdriveNFanSpeedGet_(context_, adlAdapterIndex, 1, &fanVal) == 0 && fanVal.iFanSpeed >= 0 && fanVal.iFanSpeed <= 100) {
            metrics.fanSpeedPercent = static_cast<uint32_t>(fanVal.iFanSpeed);
        }
    } else if (fnAdlOverdrive6FanSpeedGet_) {
        AdlFanSpeedValue fanVal{};
        fanVal.iSize = sizeof(AdlFanSpeedValue);
        fanVal.iSpeedType = 2; // Query RPM
        if (fnAdlOverdrive6FanSpeedGet_(adlAdapterIndex, &fanVal) == 0 && fanVal.iFanSpeed >= 0) {
            metrics.fanRpm = static_cast<uint32_t>(fanVal.iFanSpeed);
        }

        fanVal.iSpeedType = 1; // Query percentage
        if (fnAdlOverdrive6FanSpeedGet_(adlAdapterIndex, &fanVal) == 0 && fanVal.iFanSpeed >= 0 && fanVal.iFanSpeed <= 100) {
            metrics.fanSpeedPercent = static_cast<uint32_t>(fanVal.iFanSpeed);
        }
    }

    // 5. Memory Info
    if (metrics.memoryTotalBytes == 0) {
        if (fnAdl2AdapterMemoryInfoGet_ && context_) {
            AdlMemoryInfo memInfo{};
            if (fnAdl2AdapterMemoryInfoGet_(context_, adlAdapterIndex, &memInfo) == 0 && memInfo.iMemorySize > 0) {
                metrics.memoryTotalBytes = static_cast<uint64_t>(memInfo.iMemorySize);
            }
        } else if (fnAdlAdapterMemoryInfoGet_) {
            AdlMemoryInfo memInfo{};
            if (fnAdlAdapterMemoryInfoGet_(adlAdapterIndex, &memInfo) == 0 && memInfo.iMemorySize > 0) {
                metrics.memoryTotalBytes = static_cast<uint64_t>(memInfo.iMemorySize);
            }
        }
    }

    return true;
}

} // namespace BareStats
