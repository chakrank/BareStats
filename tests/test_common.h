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
bool Test_LinuxOverlay_ColSpanAndColIndexLayout();
bool Test_Overlay_PositionPresetCyclingSequence();

// Config Tests
bool Test_Config_ParsingAndHotReload();
bool Test_Config_SchemaMigration();
bool Test_HardwareIntrospection_ConfigAutoGeneration();
bool Test_IniParser_RoundtripAndComments();
bool Test_Config_MalformedAndBoundaryClamping();
bool Test_Config_PriorityAndCustomPath();
bool Test_Config_LayoutSectionAndClusterAliases();
bool Test_Formatter_GridOrderAndRigAliases();

// Crypto & Wire Protocol Tests (v1.4)
bool Test_WireProtocol_StructSizesAndAlignment();
bool Test_Crypto_RandomAndHexConversion();
bool Test_Crypto_AeadRoundtripAndTamperRejection();
bool Test_WireProtocol_PackAndUnpack();
bool Test_WireProtocol_ReplaySlidingWindow();
bool Test_WireProtocol_WallClockFreshness();

// Engine-Agnostic HTTP Scraper Tests (v1.4 Step 2)
bool Test_HttpClient_ParseUrlAndDechunk();
bool Test_JsonMini_Parsers();
bool Test_LlmCollector_PrometheusParsing();
bool Test_LlmCollector_OllamaParsing();
bool Test_LlmCollector_LlamaCppSlotsParsing();
bool Test_LlmCollector_OpenAiModelsParsing();
bool Test_LlmCollector_3StrikeDegradationAndBurstHold();

// Model Name Condenser & Multi-Model Residency Tests (v1.4 Step 3)
bool Test_ModelNameCondenser_TableTraces();
bool Test_ModelNameCondenser_ParamBadges();
bool Test_ModelNameCondenser_EmbeddingFilter();
bool Test_ModelNameCondenser_Formatter7Stages();
bool Test_ModelNameCondenser_MultiModelCoResidency();
bool Test_LlmCollector_OllamaEmbeddingFiltering();

// Dynamic Endpoint Pool & Spatial Anchoring Tests (v1.4 Step 4)
bool Test_LayoutEngine_ParseEndpoints();
bool Test_LayoutEngine_ParseGridToken();
bool Test_LlmResolver_PriorityCascade();
bool Test_LlmResolver_FormatGridCellText();
bool Test_IslandWindowManager_Visibility();

// Single-Binary Architecture & File-Presence Mode Matrix Tests (v1.4 Step 5)
bool Test_SingleBinary_ExecutableDirectoryResolution();
bool Test_SingleBinary_EvaluateAppMode();
bool Test_SingleBinary_TelemetryEngineAppMode();
bool Test_SingleBinary_StartBackgroundWorkers_CommandCenter();
bool Test_SingleBinary_FeedDeduplication();
bool Test_SingleBinary_DynamicLoaderIsolation();
bool Test_SingleBinary_InMemoryDefaultsAndNonDestructiveLoad();
bool Test_SingleBinary_StrictBinaryHeadlessParsing();
bool Test_SingleBinary_LoggerRotation();
bool Test_SingleBinary_NoCrossRigLlmContamination();
bool Test_SingleBinary_UdpSatelliteTimeoutAndOfflineTransition();
bool Test_SingleBinary_MultiEndpointAggregation();

// Encrypted UDP Satellite Push & Service Installer Tests (v1.4 Step 6)
bool Test_UdpStreamer_TransmitterAndReceiverRoundtrip();
bool Test_UdpStreamer_TamperAndWrongKeyRejection();
bool Test_UdpStreamer_SubnetAutoDiscoveryHandshake();
bool Test_UdpStreamer_TransactionChallengeReplayRejection();
bool Test_UdpStreamer_ContinuousBroadcastAndDualPathDeduplication();
bool Test_UdpStreamer_MultiTargetDualCommandCenter();
bool Test_ServiceInstaller_Validation();
bool Test_ServiceInstaller_UnprivilegedInstallation();
bool Test_ConfigManager_ExportSatelliteIni();
bool Test_ResolveSatelliteExportPath();

// Asynchronous Threading & Lock-Free Seqlock Tests (v1.4 Step 7)
bool Test_Seqlock_TriviallyCopyableGuarantees();
bool Test_Seqlock_HighContentionStress();
bool Test_Seqlock_HashSnapshot0PercentIdle();
bool Test_HttpClient_DnsCachingAndBackoff();

// Phase 2 Comprehensive LLM Verification Suites (Tests 1 to 62)
bool Test_LLM_2A_OllamaAdapter();
bool Test_LLM_2B_LlamaCppAdapter();
bool Test_LLM_2C_PrometheusAdapter();
bool Test_LLM_2D_OpenAiCompatAdapter();
bool Test_LLM_2E_AutoNegotiation();
bool Test_LLM_2F_3StrikeAndBackoff();
bool Test_LLM_2G_TokenParsingAndResolver();
bool Test_LLM_2H_MultiGpuCorrelation();
bool Test_LLM_2I_FormattingAndDisplay();

// 16. Security Regression Test Suite (Audit Findings VULN-01 to VULN-09)
bool Test_WireProtocol_ReplaySessionMonotonicity();
bool Test_UdpStreamer_RejectUnauthorizedSenderRig();
bool Test_UdpStreamer_TrackerPoolStarvationDefense();
bool Test_ServiceInstaller_SystemdPermissionConsistency();
bool Test_ServiceInstaller_LaunchdPathWhitespaceHandling();
bool Test_Config_PrivateKeyRestrictedFileAcl();
bool Test_UdpStreamer_UnauthenticatedHeaderLogSanitization();
bool Test_LlmCollector_PrometheusNegativeCounterUnderflow();
bool Test_JsonMini_EscapedBackslashStringParsing();
bool Test_LlmCollector_NonTerminatedWireStringSafety();
bool Test_ServiceInstaller_WorkingDirectorySatelliteDiscovery();
bool Test_Config_ClusterAllowedRigsPropagation();
bool Test_WireProtocol_CapCounterAdvanceStep();
bool Test_UdpStreamer_EvictionReplayResistance();
bool Test_TelemetryEngine_ConcurrentClusterWriters();
bool Test_ServiceInstaller_LaunchdShellMetacharacterSanitization();
bool Test_Logger_MaxRotatedArchivesRetention();
bool Test_LlmCollector_PrometheusNegativeAndOverflowMetricsClamping();

// Phase 1 Security Audit Verification Tests
bool Test_Crypto_RejectLowOrderSharedSecrets();
bool Test_UdpStreamer_DiscoverAllowedRigsAndWireRigIdMismatch();
bool Test_ServiceInstaller_SystemdQuoting();

// Phase 2 Distributed Cluster & Multi-GPU Completeness Tests
bool Test_Cluster_CaseInsensitiveRigAndRemoteGpuCompleteness();
bool Test_TelemetryEngine_DynamicReceiverActivationAndFanPack();

// Phase 4 UDP Discovery Rate-Limiting & Target Isolation Tests
bool Test_UdpStreamer_DiscoveryRateLimitingAndTargetIsolation();

// Phase 5 Network & Config Optimizations Tests
bool Test_Config_ShowTrayIconPriorityAndMigration();
bool Test_LinuxProc_NetworkInterfaceSwitchAndGatewayReevaluation();
bool Test_UnifiedLayout_CaseSensitivityAndDeduplication();




