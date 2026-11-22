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
    std::cout << "BareStats Automated Modular Verification Test Suite (v1.4.0)" << std::endl;
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
    RUN_TEST(Test_LinuxOverlay_ColSpanAndColIndexLayout);
    RUN_TEST(Test_Overlay_PositionPresetCyclingSequence);

    // 6. Configuration, Migration & INI Parser Tests
    RUN_TEST(Test_Config_ParsingAndHotReload);
    RUN_TEST(Test_Config_SchemaMigration);
    RUN_TEST(Test_HardwareIntrospection_ConfigAutoGeneration);
    RUN_TEST(Test_IniParser_RoundtripAndComments);
    RUN_TEST(Test_Config_MalformedAndBoundaryClamping);
    RUN_TEST(Test_Config_PriorityAndCustomPath);
    RUN_TEST(Test_Config_LayoutSectionAndClusterAliases);
    RUN_TEST(Test_Formatter_GridOrderAndRigAliases);

    // 7. Crypto & Distributed Wire Protocol Tests (v1.4)
    RUN_TEST(Test_WireProtocol_StructSizesAndAlignment);
    RUN_TEST(Test_Crypto_RandomAndHexConversion);
    RUN_TEST(Test_Crypto_AeadRoundtripAndTamperRejection);
    RUN_TEST(Test_WireProtocol_PackAndUnpack);
    RUN_TEST(Test_WireProtocol_ReplaySlidingWindow);
    RUN_TEST(Test_WireProtocol_WallClockFreshness);

    // 8. Engine-Agnostic HTTP Scraper Tests (v1.4 Step 2)
    RUN_TEST(Test_HttpClient_ParseUrlAndDechunk);
    RUN_TEST(Test_JsonMini_Parsers);
    RUN_TEST(Test_LlmCollector_PrometheusParsing);
    RUN_TEST(Test_LlmCollector_OllamaParsing);
    RUN_TEST(Test_LlmCollector_LlamaCppSlotsParsing);
    RUN_TEST(Test_LlmCollector_OpenAiModelsParsing);
    RUN_TEST(Test_LlmCollector_3StrikeDegradationAndBurstHold);

    // 9. Model Name Condenser & Multi-Model Residency Tests (v1.4 Step 3)
    RUN_TEST(Test_ModelNameCondenser_TableTraces);
    RUN_TEST(Test_ModelNameCondenser_ParamBadges);
    RUN_TEST(Test_ModelNameCondenser_EmbeddingFilter);
    RUN_TEST(Test_ModelNameCondenser_Formatter7Stages);
    RUN_TEST(Test_ModelNameCondenser_MultiModelCoResidency);
    RUN_TEST(Test_LlmCollector_OllamaEmbeddingFiltering);

    // 10. Dynamic Endpoint Pool & Spatial Anchoring Tests (v1.4 Step 4)
    RUN_TEST(Test_LayoutEngine_ParseEndpoints);
    RUN_TEST(Test_LayoutEngine_ParseGridToken);
    RUN_TEST(Test_LlmResolver_PriorityCascade);
    RUN_TEST(Test_LlmResolver_FormatGridCellText);
    RUN_TEST(Test_IslandWindowManager_Visibility);

    // 11. Single-Binary Architecture & File-Presence Mode Matrix Tests (v1.4 Step 5)
    RUN_TEST(Test_SingleBinary_ExecutableDirectoryResolution);
    RUN_TEST(Test_SingleBinary_EvaluateAppMode);
    RUN_TEST(Test_SingleBinary_TelemetryEngineAppMode);
    RUN_TEST(Test_SingleBinary_StartBackgroundWorkers_CommandCenter);
    RUN_TEST(Test_SingleBinary_FeedDeduplication);
    RUN_TEST(Test_SingleBinary_DynamicLoaderIsolation);

    // 12. Encrypted UDP Satellite Push & Service Installer Tests (v1.4 Step 6)
    RUN_TEST(Test_UdpStreamer_TransmitterAndReceiverRoundtrip);
    RUN_TEST(Test_UdpStreamer_TamperAndWrongKeyRejection);
    RUN_TEST(Test_UdpStreamer_SubnetAutoDiscoveryHandshake);
    RUN_TEST(Test_UdpStreamer_TransactionChallengeReplayRejection);
    RUN_TEST(Test_UdpStreamer_ContinuousBroadcastAndDualPathDeduplication);
    RUN_TEST(Test_UdpStreamer_MultiTargetDualCommandCenter);
    RUN_TEST(Test_ServiceInstaller_Validation);
    RUN_TEST(Test_ServiceInstaller_UnprivilegedInstallation);
    RUN_TEST(Test_ConfigManager_ExportSatelliteIni);
    RUN_TEST(Test_ResolveSatelliteExportPath);

    // 13. Asynchronous Threading & Lock-Free Seqlock Tests (v1.4 Step 7)
    RUN_TEST(Test_Seqlock_TriviallyCopyableGuarantees);
    RUN_TEST(Test_Seqlock_HighContentionStress);
    RUN_TEST(Test_Seqlock_HashSnapshot0PercentIdle);
    RUN_TEST(Test_HttpClient_DnsCachingAndBackoff);

    // 14. In-Memory Defaults, Unified Mode Arbitration & High-Signal Logging (v1.4 Step 9)
    RUN_TEST(Test_SingleBinary_InMemoryDefaultsAndNonDestructiveLoad);
    RUN_TEST(Test_SingleBinary_StrictBinaryHeadlessParsing);
    RUN_TEST(Test_SingleBinary_LoggerRotation);
    RUN_TEST(Test_SingleBinary_NoCrossRigLlmContamination);
    RUN_TEST(Test_SingleBinary_UdpSatelliteTimeoutAndOfflineTransition);
    RUN_TEST(Test_SingleBinary_MultiEndpointAggregation);

    // 15. Phase 2 Comprehensive LLM Verification Suites (Tests 1 to 62)
    RUN_TEST(Test_LLM_2A_OllamaAdapter);
    RUN_TEST(Test_LLM_2B_LlamaCppAdapter);
    RUN_TEST(Test_LLM_2C_PrometheusAdapter);
    RUN_TEST(Test_LLM_2D_OpenAiCompatAdapter);
    RUN_TEST(Test_LLM_2E_AutoNegotiation);
    RUN_TEST(Test_LLM_2F_3StrikeAndBackoff);
    RUN_TEST(Test_LLM_2G_TokenParsingAndResolver);
    RUN_TEST(Test_LLM_2H_MultiGpuCorrelation);
    RUN_TEST(Test_LLM_2I_FormattingAndDisplay);

    // 16. Security Regression Test Suite (Audit Findings VULN-01 to VULN-09)
    RUN_TEST(Test_WireProtocol_ReplaySessionMonotonicity);
    RUN_TEST(Test_UdpStreamer_RejectUnauthorizedSenderRig);
    RUN_TEST(Test_UdpStreamer_TrackerPoolStarvationDefense);
    RUN_TEST(Test_ServiceInstaller_SystemdPermissionConsistency);
    RUN_TEST(Test_ServiceInstaller_LaunchdPathWhitespaceHandling);
    RUN_TEST(Test_Config_PrivateKeyRestrictedFileAcl);
    RUN_TEST(Test_UdpStreamer_UnauthenticatedHeaderLogSanitization);
    RUN_TEST(Test_LlmCollector_PrometheusNegativeCounterUnderflow);
    RUN_TEST(Test_JsonMini_EscapedBackslashStringParsing);
    RUN_TEST(Test_LlmCollector_NonTerminatedWireStringSafety);
    RUN_TEST(Test_ServiceInstaller_WorkingDirectorySatelliteDiscovery);
    RUN_TEST(Test_Config_ClusterAllowedRigsPropagation);
    RUN_TEST(Test_WireProtocol_CapCounterAdvanceStep);
    RUN_TEST(Test_UdpStreamer_EvictionReplayResistance);
    RUN_TEST(Test_TelemetryEngine_ConcurrentClusterWriters);
    RUN_TEST(Test_ServiceInstaller_LaunchdShellMetacharacterSanitization);
    RUN_TEST(Test_Logger_MaxRotatedArchivesRetention);
    RUN_TEST(Test_LlmCollector_PrometheusNegativeAndOverflowMetricsClamping);
    RUN_TEST(Test_Crypto_RejectLowOrderSharedSecrets);
    RUN_TEST(Test_UdpStreamer_DiscoverAllowedRigsAndWireRigIdMismatch);
    RUN_TEST(Test_ServiceInstaller_SystemdQuoting);
    RUN_TEST(Test_Cluster_CaseInsensitiveRigAndRemoteGpuCompleteness);
    RUN_TEST(Test_TelemetryEngine_DynamicReceiverActivationAndFanPack);
    RUN_TEST(Test_UdpStreamer_DiscoveryRateLimitingAndTargetIsolation);
    RUN_TEST(Test_Config_ShowTrayIconPriorityAndMigration);
    RUN_TEST(Test_LinuxProc_NetworkInterfaceSwitchAndGatewayReevaluation);
    RUN_TEST(Test_UnifiedLayout_CaseSensitivityAndDeduplication);

    std::cout << "============================================================" << std::endl;
    std::cout << "Test Summary: " << g_passedTests << "/" << g_totalTests << " passed";
    if (g_failedTests > 0) {
        std::cout << " (" << g_failedTests << " FAILED)";
    }
    std::cout << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
