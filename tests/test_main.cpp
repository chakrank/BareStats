#include "test_common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

std::string GetTestModuleDir() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t lastSlash = s.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return s.substr(0, lastSlash + 1);
    }
    return ".\\";
}
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>

std::string GetTestModuleDir() {
    char path[PATH_MAX]{};
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string s(path);
        size_t lastSlash = s.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return s.substr(0, lastSlash + 1);
        }
    }
    return "./";
}
#else
#include <unistd.h>
#include <climits>

std::string GetTestModuleDir() {
    char path[PATH_MAX]{};
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    if (count != -1) {
        std::string s(path, count);
        size_t lastSlash = s.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return s.substr(0, lastSlash + 1);
        }
    }
    return "./";
}
#endif

int g_totalTests = 0;
int g_passedTests = 0;
int g_failedTests = 0;

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "BareStats Automated Modular Verification Test Suite (v1.3.0)" << std::endl;
    std::cout << "============================================================" << std::endl;

    // 1. Common Metrics & Telemetry Engine Tests
    RUN_TEST(Test_CpuCollector_RealHardware);
    RUN_TEST(Test_MemoryCollector_RealHardware);
    RUN_TEST(Test_NetworkCollector_RealHardware);
    RUN_TEST(Test_NvidiaNvml_MockPipeline);
    RUN_TEST(Test_TelemetryEngine_CallbackNotification);
    RUN_TEST(Test_TelemetryEngine_RapidBurstStressAndReload);

#ifdef _WIN32
    // 2. Windows-Specific Driver & Graphic Tests
    RUN_TEST(Test_UniversalGraphics_D3dKmtAndDxgi);
    RUN_TEST(Test_AmdAdl_MockPipeline);
    RUN_TEST(Test_IntelIgcl_MockPipeline);
    RUN_TEST(Test_DriverFallback_GracefulRecovery);
    RUN_TEST(Test_WindowsMocks_NullAndErrorInjection);
#elif defined(__APPLE__)
    // 2. macOS-Specific Telemetry Tests
    RUN_TEST(Test_Macos_MachCpuCollector);
    RUN_TEST(Test_Macos_MachMemoryCollector);
    RUN_TEST(Test_Macos_BsdNetworkCollector);
    RUN_TEST(Test_Macos_MetalAndIOKitGpu);
#endif

    // 3. Linux-Specific Synthetic Tests (Agnostic - runnable anywhere)
    RUN_TEST(Test_LinuxProc_CpuCollector_Synthetic);
    RUN_TEST(Test_LinuxProc_MemoryCollector_Synthetic);
    RUN_TEST(Test_LinuxProc_NetworkCollector_Synthetic);
    RUN_TEST(Test_LinuxSysfs_AmdGpu_Synthetic);
    RUN_TEST(Test_LinuxSysfs_IntelGpu_Synthetic);
    RUN_TEST(Test_LinuxSysfs_MultiCardEnumeration_Synthetic);
    RUN_TEST(Test_LinuxProc_TruncatedAndCorrupted_Resilience);
    RUN_TEST(Test_LinuxSysfs_MissingHwmonAndMultiGateway_Resilience);

    // 4. Formatter & Telemetry Layout Tests
    RUN_TEST(Test_PrecisionVram_FormattingMath);
    RUN_TEST(Test_MultiGpu_FormattingLayout);
    RUN_TEST(Test_PerGpu_AsymmetricFormatting);
    RUN_TEST(Test_FormatCell_ModularWidgetsAndSubStats);

    // 5. Overlay, Matrix Grid & Geometry Tests
    RUN_TEST(Test_GridLayout_MultiRowWrapping);
    RUN_TEST(Test_TrueMatrixGrid_ParsingAndColspans);
    RUN_TEST(Test_IslandsGrid_AutoTargetingAndModeSeamlessness);
    RUN_TEST(Test_LinuxOverlay_DirtyHashBypass);
    RUN_TEST(Test_LinuxOverlay_IslandsLayoutMath);
    RUN_TEST(Test_LinuxOverlay_CustomGridWithEmptyCells);

    // 6. Configuration, Migration & INI Parser Tests
    RUN_TEST(Test_Config_ParsingAndHotReload);
    RUN_TEST(Test_Config_SchemaMigration);
    RUN_TEST(Test_HardwareIntrospection_ConfigAutoGeneration);
    RUN_TEST(Test_IniParser_RoundtripAndComments);
    RUN_TEST(Test_Config_MalformedAndBoundaryClamping);
    RUN_TEST(Test_Config_PriorityAndCustomPath);

    std::cout << "============================================================" << std::endl;
    std::cout << "Test Summary: " << g_passedTests << "/" << g_totalTests << " passed";
    if (g_failedTests > 0) {
        std::cout << " (" << g_failedTests << " FAILED)";
    }
    std::cout << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
