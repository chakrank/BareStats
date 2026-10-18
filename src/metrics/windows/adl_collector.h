#pragma once

#include "metrics.h"
#include <windows.h>
#include <vector>
#include <string>

namespace BareStats {

struct AdlAdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[256];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[256];
    char strDisplayName[256];
    int iPresent;
    int iExist;
    char strDriverPath[256];
    char strDriverPathExt[256];
    char strPNPString[256];
    int iOSDisplayIndex;
};

struct AdlTemperature {
    int iSize;
    int iTemperature; // 1/1000 of a degree Celsius
};

struct AdlFanSpeedValue {
    int iSize;
    int iSpeedType;  // 1 = percent, 2 = RPM
    int iFanSpeed;
    int iFlags;
};

struct AdlPMActivity {
    int iSize;
    int iEngineClock;
    int iMemoryClock;
    int iVddc;
    int iActivityPercent;
    int iCurrentPerformanceLevel;
    int iCurrentBusSpeed;
    int iCurrentBusLanes;
    int iMaximumBusLanes;
    int iReserved;
};

struct AdlMemoryInfo {
    long long iMemorySize;
    char strMemoryType[256];
    long long iMemoryBandwidth;
};

class AdlCollector {
public:
    explicit AdlCollector(const char* customDllPath = nullptr);
    ~AdlCollector();

    bool IsAvailable() const { return isAvailable_; }
    bool Collect(GpuMetrics& metrics, uint32_t deviceIndex = 0);
    bool Recover();
    void Shutdown();
    bool LoadAdl(const char* customDllPath = nullptr);

private:
    HMODULE adlDll_ = nullptr;
    std::string customDllPath_;
    void* context_ = nullptr;
    bool isAvailable_ = false;

    // Memory callbacks for ADL
    typedef void* (__cdecl *ADL_MAIN_MALLOC_CALLBACK)(int);
    typedef int (*ADL2_Main_Control_Create_t)(ADL_MAIN_MALLOC_CALLBACK, int, void**);
    typedef int (*ADL2_Main_Control_Destroy_t)(void*);
    typedef int (*ADL_Main_Control_Create_t)(ADL_MAIN_MALLOC_CALLBACK, int);
    typedef int (*ADL_Main_Control_Destroy_t)();

    typedef int (*ADL2_Adapter_NumberOfAdapters_Get_t)(void*, int*);
    typedef int (*ADL_Adapter_NumberOfAdapters_Get_t)(int*);
    typedef int (*ADL2_Adapter_AdapterInfo_Get_t)(void*, AdlAdapterInfo*, int);
    typedef int (*ADL_Adapter_AdapterInfo_Get_t)(AdlAdapterInfo*, int);

    typedef int (*ADL2_OverdriveN_Temperature_Get_t)(void*, int, int, int*);
    typedef int (*ADL_Overdrive6_Temperature_Get_t)(int, AdlTemperature*);
    typedef int (*ADL2_Overdrive6_CurrentPower_Get_t)(void*, int, int, int*);
    typedef int (*ADL_Overdrive6_CurrentPower_Get_t)(int, int, int*);
    typedef int (*ADL2_OverdriveN_FanSpeed_Get_t)(void*, int, int, AdlFanSpeedValue*);
    typedef int (*ADL_Overdrive6_FanSpeed_Get_t)(int, AdlFanSpeedValue*);
    typedef int (*ADL2_Overdrive6_CurrentStatus_Get_t)(void*, int, AdlPMActivity*);
    typedef int (*ADL_Overdrive6_CurrentStatus_Get_t)(int, AdlPMActivity*);
    typedef int (*ADL2_Overdrive5_CurrentActivity_Get_t)(void*, int, AdlPMActivity*);
    typedef int (*ADL_Overdrive5_CurrentActivity_Get_t)(int, AdlPMActivity*);
    typedef int (*ADL2_Adapter_MemoryInfo_Get_t)(void*, int, AdlMemoryInfo*);
    typedef int (*ADL_Adapter_MemoryInfo_Get_t)(int, AdlMemoryInfo*);

    ADL2_Main_Control_Create_t fnAdl2MainControlCreate_ = nullptr;
    ADL2_Main_Control_Destroy_t fnAdl2MainControlDestroy_ = nullptr;
    ADL_Main_Control_Create_t fnAdlMainControlCreate_ = nullptr;
    ADL_Main_Control_Destroy_t fnAdlMainControlDestroy_ = nullptr;

    ADL2_Adapter_NumberOfAdapters_Get_t fnAdl2AdapterNumberOfAdaptersGet_ = nullptr;
    ADL_Adapter_NumberOfAdapters_Get_t fnAdlAdapterNumberOfAdaptersGet_ = nullptr;
    ADL2_Adapter_AdapterInfo_Get_t fnAdl2AdapterAdapterInfoGet_ = nullptr;
    ADL_Adapter_AdapterInfo_Get_t fnAdlAdapterAdapterInfoGet_ = nullptr;

    ADL2_OverdriveN_Temperature_Get_t fnAdl2OverdriveNTemperatureGet_ = nullptr;
    ADL_Overdrive6_Temperature_Get_t fnAdlOverdrive6TemperatureGet_ = nullptr;
    ADL2_Overdrive6_CurrentPower_Get_t fnAdl2Overdrive6CurrentPowerGet_ = nullptr;
    ADL_Overdrive6_CurrentPower_Get_t fnAdlOverdrive6CurrentPowerGet_ = nullptr;
    ADL2_OverdriveN_FanSpeed_Get_t fnAdl2OverdriveNFanSpeedGet_ = nullptr;
    ADL_Overdrive6_FanSpeed_Get_t fnAdlOverdrive6FanSpeedGet_ = nullptr;
    ADL2_Overdrive6_CurrentStatus_Get_t fnAdl2Overdrive6CurrentStatusGet_ = nullptr;
    ADL_Overdrive6_CurrentStatus_Get_t fnAdlOverdrive6CurrentStatusGet_ = nullptr;
    ADL2_Overdrive5_CurrentActivity_Get_t fnAdl2Overdrive5CurrentActivityGet_ = nullptr;
    ADL_Overdrive5_CurrentActivity_Get_t fnAdlOverdrive5CurrentActivityGet_ = nullptr;
    ADL2_Adapter_MemoryInfo_Get_t fnAdl2AdapterMemoryInfoGet_ = nullptr;
    ADL_Adapter_MemoryInfo_Get_t fnAdlAdapterMemoryInfoGet_ = nullptr;

    bool LoadAdl();
    void UnloadAdl();
    int ResolveAmdAdapterIndex(uint32_t deviceIndex);
};

} // namespace BareStats
