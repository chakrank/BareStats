#include <windows.h>
#include <cstring>

#define ADL_OK 0
#define ADL_ERR -1

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
    int iTemperature;
};

struct AdlFanSpeedValue {
    int iSize;
    int iSpeedType;
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

typedef void* (__cdecl *ADL_MAIN_MALLOC_CALLBACK)(int);

extern "C" {

__declspec(dllexport) int __cdecl ADL2_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int enumConnectedAdapters, void** context) {
    (void)callback;
    (void)enumConnectedAdapters;
    if (!context) return ADL_ERR;
    *context = (void*)0x10020000;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL2_Main_Control_Destroy(void* context) {
    (void)context;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int enumConnectedAdapters) {
    (void)callback;
    (void)enumConnectedAdapters;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Main_Control_Destroy() {
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL2_Adapter_NumberOfAdapters_Get(void* context, int* numAdapters) {
    (void)context;
    if (!numAdapters) return ADL_ERR;
    *numAdapters = 1;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Adapter_NumberOfAdapters_Get(int* numAdapters) {
    if (!numAdapters) return ADL_ERR;
    *numAdapters = 1;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL2_Adapter_AdapterInfo_Get(void* context, AdlAdapterInfo* info, int size) {
    (void)context;
    if (!info || size < (int)sizeof(AdlAdapterInfo)) return ADL_ERR;
    memset(info, 0, sizeof(AdlAdapterInfo));
    info->iSize = sizeof(AdlAdapterInfo);
    info->iAdapterIndex = 0;
    info->iVendorID = 0x1002;
    info->iPresent = 1;
    info->iExist = 1;
    strcpy_s(info->strUDID, "PCI_VEN_1002_DEV_744C");
    strcpy_s(info->strAdapterName, "AMD Radeon RX 7900 XTX");
    strcpy_s(info->strDisplayName, "\\\\.\\DISPLAY1");
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Adapter_AdapterInfo_Get(AdlAdapterInfo* info, int size) {
    return ADL2_Adapter_AdapterInfo_Get(nullptr, info, size);
}

__declspec(dllexport) int __cdecl ADL2_OverdriveN_Temperature_Get(void* context, int adapterIndex, int thermalControllerIndex, int* temperature) {
    (void)context;
    (void)adapterIndex;
    (void)thermalControllerIndex;
    if (!temperature) return ADL_ERR;
    *temperature = 65000; // 65.0 C (millidegrees)
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Overdrive6_Temperature_Get(int adapterIndex, AdlTemperature* temperature) {
    (void)adapterIndex;
    if (!temperature) return ADL_ERR;
    temperature->iTemperature = 65000;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL2_Overdrive6_CurrentPower_Get(void* context, int adapterIndex, int powerType, int* currentPower) {
    (void)context;
    (void)adapterIndex;
    (void)powerType;
    if (!currentPower) return ADL_ERR;
    *currentPower = (185 << 8); // 185 W in 8.8 fixed-point
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Overdrive6_CurrentPower_Get(int adapterIndex, int powerType, int* currentPower) {
    return ADL2_Overdrive6_CurrentPower_Get(nullptr, adapterIndex, powerType, currentPower);
}

__declspec(dllexport) int __cdecl ADL2_OverdriveN_FanSpeed_Get(void* context, int adapterIndex, int thermalControllerIndex, AdlFanSpeedValue* fanSpeed) {
    (void)context;
    (void)adapterIndex;
    (void)thermalControllerIndex;
    if (!fanSpeed) return ADL_ERR;
    if (fanSpeed->iSpeedType == 2) {
        fanSpeed->iFanSpeed = 1600; // RPM
        return ADL_OK;
    } else if (fanSpeed->iSpeedType == 1) {
        fanSpeed->iFanSpeed = 48; // 48%
        return ADL_OK;
    }
    fanSpeed->iSpeedType = 2;
    fanSpeed->iFanSpeed = 1600;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Overdrive6_FanSpeed_Get(int adapterIndex, AdlFanSpeedValue* fanSpeed) {
    return ADL2_OverdriveN_FanSpeed_Get(nullptr, adapterIndex, 0, fanSpeed);
}

__declspec(dllexport) int __cdecl ADL2_Overdrive6_CurrentStatus_Get(void* context, int adapterIndex, AdlPMActivity* activity) {
    (void)context;
    (void)adapterIndex;
    if (!activity) return ADL_ERR;
    activity->iActivityPercent = 78;
    activity->iEngineClock = 250000;
    activity->iMemoryClock = 250000;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Overdrive6_CurrentStatus_Get(int adapterIndex, AdlPMActivity* activity) {
    return ADL2_Overdrive6_CurrentStatus_Get(nullptr, adapterIndex, activity);
}

__declspec(dllexport) int __cdecl ADL2_Overdrive5_CurrentActivity_Get(void* context, int adapterIndex, AdlPMActivity* activity) {
    return ADL2_Overdrive6_CurrentStatus_Get(context, adapterIndex, activity);
}

__declspec(dllexport) int __cdecl ADL_Overdrive5_CurrentActivity_Get(int adapterIndex, AdlPMActivity* activity) {
    return ADL2_Overdrive6_CurrentStatus_Get(nullptr, adapterIndex, activity);
}

__declspec(dllexport) int __cdecl ADL2_Adapter_MemoryInfo_Get(void* context, int adapterIndex, AdlMemoryInfo* memInfo) {
    (void)context;
    (void)adapterIndex;
    if (!memInfo) return ADL_ERR;
    memInfo->iMemorySize = 24ULL * 1024 * 1024 * 1024; // 24 GB
    strcpy_s(memInfo->strMemoryType, "GDDR6");
    memInfo->iMemoryBandwidth = 960;
    return ADL_OK;
}

__declspec(dllexport) int __cdecl ADL_Adapter_MemoryInfo_Get(int adapterIndex, AdlMemoryInfo* memInfo) {
    return ADL2_Adapter_MemoryInfo_Get(nullptr, adapterIndex, memInfo);
}

} // extern "C"
