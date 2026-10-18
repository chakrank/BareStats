#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cassert>
#include <cmath>
#include <fstream>
#include <filesystem>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] Line " << __LINE__ << ": " << msg << " (" #cond ")" << std::endl; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        g_totalTests++; \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        bool ok = fn(); \
        if (ok) { \
            g_passedTests++; \
            std::cout << "[PASS] " << #fn << std::endl; \
        } else { \
            g_failedTests++; \
            std::cerr << "[FAIL] " << #fn << " FAILED!" << std::endl; \
        } \
    } while (0)

extern int g_totalTests;
extern int g_passedTests;
extern int g_failedTests;

std::string GetTestModuleDir();

// Common Metrics Tests
bool Test_CpuCollector_RealHardware();
bool Test_MemoryCollector_RealHardware();
bool Test_NetworkCollector_RealHardware();
bool Test_NvidiaNvml_MockPipeline();
bool Test_TelemetryEngine_CallbackNotification();
bool Test_TelemetryEngine_RapidBurstStressAndReload();

// Windows Metrics Tests
#ifdef _WIN32
bool Test_UniversalGraphics_D3dKmtAndDxgi();
bool Test_AmdAdl_MockPipeline();
bool Test_IntelIgcl_MockPipeline();
bool Test_DriverFallback_GracefulRecovery();
bool Test_WindowsMocks_NullAndErrorInjection();
#elif defined(__APPLE__)
bool Test_Macos_MachCpuCollector();
bool Test_Macos_MachMemoryCollector();
bool Test_Macos_BsdNetworkCollector();
bool Test_Macos_MetalAndIOKitGpu();
#endif

// Linux Metrics Tests
bool Test_LinuxProc_CpuCollector_Synthetic();
bool Test_LinuxProc_MemoryCollector_Synthetic();
bool Test_LinuxProc_NetworkCollector_Synthetic();
bool Test_LinuxSysfs_AmdGpu_Synthetic();
bool Test_LinuxSysfs_IntelGpu_Synthetic();
bool Test_LinuxSysfs_MultiCardEnumeration_Synthetic();
bool Test_LinuxProc_TruncatedAndCorrupted_Resilience();
bool Test_LinuxSysfs_MissingHwmonAndMultiGateway_Resilience();

// Formatters Tests
bool Test_PrecisionVram_FormattingMath();
bool Test_MultiGpu_FormattingLayout();
bool Test_PerGpu_AsymmetricFormatting();
bool Test_FormatCell_ModularWidgetsAndSubStats();

// Overlay & Grid Tests
bool Test_GridLayout_MultiRowWrapping();
bool Test_TrueMatrixGrid_ParsingAndColspans();
bool Test_IslandsGrid_AutoTargetingAndModeSeamlessness();
bool Test_LinuxOverlay_DirtyHashBypass();
bool Test_LinuxOverlay_IslandsLayoutMath();
bool Test_LinuxOverlay_CustomGridWithEmptyCells();

// Config Tests
bool Test_Config_ParsingAndHotReload();
bool Test_Config_SchemaMigration();
bool Test_HardwareIntrospection_ConfigAutoGeneration();
bool Test_IniParser_RoundtripAndComments();
bool Test_Config_MalformedAndBoundaryClamping();
bool Test_Config_PriorityAndCustomPath();

