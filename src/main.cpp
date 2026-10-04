#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include "metrics/cpu_collector.h"
#include "metrics/memory_collector.h"
#include "metrics/gpu_collector.h"
#include "overlay/overlay_window.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "util/formatter.h"

#include "metrics/telemetry_engine.h"

using namespace BareStats;

static void OutputToConsole(const std::string& msg) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD fileType = GetFileType(hOut);
    if (!hOut || hOut == INVALID_HANDLE_VALUE || fileType == FILE_TYPE_UNKNOWN) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    }
}

static std::string WideToUtf8Str(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    std::wstring fullCmd = GetCommandLineW();

    // Support --test or --cli for verification
    if (fullCmd.find(L"--test") != std::wstring::npos || fullCmd.find(L"--cli") != std::wstring::npos) {
        OutputToConsole("\nBareStats CLI Verification (v1.0.0)\n");
        OutputToConsole("Initializing telemetry collectors...\n");

        std::string configPath = ConfigManager::GetDefaultConfigPath();
        AppConfig config = ConfigManager::Load(configPath);

        CpuCollector cpuCollector;
        MemoryCollector memoryCollector;
        GpuCollector gpuCollector;
        NetworkCollector networkCollector;

        auto gpus = gpuCollector.GetAvailableGpus();
        OutputToConsole("Discovered " + std::to_string(gpus.size()) + " hardware GPU adapter(s):\n");
        for (const auto& g : gpus) {
            std::string vendorName = (g.vendorId == 0x10DE) ? "NVIDIA" :
                                     (g.vendorId == 0x8086) ? "Intel" :
                                     (g.vendorId == 0x1002) ? "AMD" :
                                     (g.vendorId == 0x5143) ? "Qualcomm" : "Generic";
            std::string targetTag = (static_cast<int>(g.index) == config.gpuIndex) ? " [Active Target]" : "";
            OutputToConsole("  [GPU " + std::to_string(g.index) + "] " + g.name + " (" + vendorName + ", " +
                std::to_string(g.dedicatedVideoMemory / (1024 * 1024)) + "MB VRAM)" + targetTag + "\n");
        }

        std::string orderStr;
        for (size_t o = 0; o < config.metricOrder.size(); ++o) {
            if (o > 0) orderStr += ", ";
            orderStr += config.metricOrder[o];
        }
        OutputToConsole("Active Settings: Order=[" + orderStr + "], Separator='" +
            WideToUtf8Str(config.separator) + "', Opacity=" + std::to_string(config.opacity) + "%\n\n");

        // Warm up delta baselines
        cpuCollector.Collect();
        networkCollector.Collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        double totalCpuUs = 0, totalMemUs = 0, totalGpuUs = 0, totalNetUs = 0, totalFmtUs = 0;
        int sampleCount = 3;

        for (int i = 0; i < sampleCount; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            SystemMetrics metrics;

            auto t0 = std::chrono::high_resolution_clock::now();
            metrics.cpu = cpuCollector.Collect();
            auto t1 = std::chrono::high_resolution_clock::now();
            metrics.memory = memoryCollector.Collect();
            auto t2 = std::chrono::high_resolution_clock::now();
            metrics.gpu = gpuCollector.Collect(config.gpuIndex);
            auto t3 = std::chrono::high_resolution_clock::now();
            metrics.network = networkCollector.Collect();
            auto t4 = std::chrono::high_resolution_clock::now();

            std::string line = "[" + std::to_string(i + 1) + "/3] " + Formatter::FormatUtf8(metrics, &config) + "\n";
            auto t5 = std::chrono::high_resolution_clock::now();

            totalCpuUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
            totalMemUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
            totalGpuUs += std::chrono::duration<double, std::micro>(t3 - t2).count();
            totalNetUs += std::chrono::duration<double, std::micro>(t4 - t3).count();
            totalFmtUs += std::chrono::duration<double, std::micro>(t5 - t4).count();

            OutputToConsole(line);

            if (i == 0) {
                if (metrics.gpu.isAvailable) {
                    std::string gpuInfo = "  [GPU Details] Name: " + metrics.gpu.name + 
                        " | TDP Limit: " + (metrics.gpu.powerLimitWatts.has_value() ? std::to_string(*metrics.gpu.powerLimitWatts) + "W" : "N/A") +
                        " | Slowdown Temp: " + (metrics.gpu.tempSlowdownLimitC.has_value() ? std::to_string(*metrics.gpu.tempSlowdownLimitC) + "C" : "N/A") + "\n";
                    OutputToConsole(gpuInfo);
                }
                if (metrics.network.isAvailable) {
                    std::string netInfo = "  [Network Details] Primary Adapter: " + WideToUtf8Str(metrics.network.primaryAdapter) + "\n";
                    OutputToConsole(netInfo);
                }
            }
        }

        double avgCpuUs = totalCpuUs / sampleCount;
        double avgMemUs = totalMemUs / sampleCount;
        double avgGpuUs = totalGpuUs / sampleCount;
        double avgNetUs = totalNetUs / sampleCount;
        double avgFmtUs = totalFmtUs / sampleCount;
        double avgTotalUs = avgCpuUs + avgMemUs + avgGpuUs + avgNetUs + avgFmtUs;
        double dutyCyclePct = (avgTotalUs / (config.refreshIntervalMs * 1000.0)) * 100.0;

        OutputToConsole("\n--------------------------------------------------------------------------------\n");
        OutputToConsole("Empirical Sampling Latencies (Microsecond Precision Timing)\n");
        OutputToConsole("--------------------------------------------------------------------------------\n");
        OutputToConsole("  CPU Sampling:       " + std::to_string(static_cast<int>(avgCpuUs)) + " us (GetSystemTimes kernel delta)\n");
        OutputToConsole("  Memory Sampling:    " + std::to_string(static_cast<int>(avgMemUs)) + " us (GlobalMemoryStatusEx)\n");
        OutputToConsole("  GPU Sampling:       " + std::to_string(static_cast<int>(avgGpuUs)) + " us (NVML / DXGI query)\n");
        OutputToConsole("  Network Sampling:   " + std::to_string(static_cast<int>(avgNetUs)) + " us (GetIfTable2 filtering)\n");
        OutputToConsole("  String Formatting:  " + std::to_string(static_cast<int>(avgFmtUs)) + " us (Layout & Unicode assembly)\n");
        OutputToConsole("--------------------------------------------------------------------------------\n");
        OutputToConsole("  Total Cycle Time:   " + std::to_string(static_cast<int>(avgTotalUs)) + " us (" +
            std::to_string(avgTotalUs / 1000.0).substr(0, 5) + " ms)\n");
        OutputToConsole("  Active Duty Cycle:  " + std::to_string(dutyCyclePct).substr(0, 6) + "% at " +
            std::to_string(config.refreshIntervalMs) + "ms polling interval\n");
        OutputToConsole("--------------------------------------------------------------------------------\n");
        OutputToConsole("Verification completed successfully.\n\n");
        return 0;
    }

    // Normal GUI Overlay mode
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\BareStats_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already active! Wake it up with a launch roll-up flip animation
        HWND existingWnd = FindWindowW(L"BareStatsOverlayClass", nullptr);
        if (existingWnd) {
            PostMessageW(existingWnd, OverlayWindow::WM_TRIGGER_LAUNCH_ANIM, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0; // Clean exit without duplicate instance
    }

    std::string configPath = ConfigManager::GetDefaultConfigPath();
    AppConfig config = ConfigManager::Load(configPath);

    TelemetryEngine telemetryEngine(config);

    OverlayWindow overlay(config);
    if (!overlay.Initialize(hInstance)) {
        if (hMutex) CloseHandle(hMutex);
        MessageBoxW(nullptr, L"Failed to initialize BareStats overlay window.", L"BareStats Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // Connect display state change callback to suspend/resume background telemetry polling
    overlay.SetDisplayStateCallback([&telemetryEngine](bool screenActive) {
        telemetryEngine.SetSuspended(!screenActive);
    });

    // Connect system resume callback (sleep / modern standby wake & TDR driver reset recovery)
    overlay.SetSystemResumeCallback([&telemetryEngine]() {
        telemetryEngine.Recover();
    });

    // Connect end session callback (clean Windows shutdown / reboot)
    overlay.SetEndSessionCallback([&telemetryEngine]() {
        telemetryEngine.Shutdown();
    });

    // Connect telemetry ready callback from worker thread
    overlay.SetTelemetryCallback([&overlay, &telemetryEngine]() {
        SystemMetrics metrics = telemetryEngine.GetLatestMetrics();
        overlay.UpdateMetrics(metrics);
    });

    // Connect config reload callback to wake telemetry engine immediately
    overlay.SetConfigReloadCallback([&telemetryEngine]() {
        telemetryEngine.Wake();
    });

    // Start background configuration file watcher for live zero-restart hot reloading
    ConfigWatcher configWatcher;
    configWatcher.Start(configPath, [&overlay]() {
        PostMessageW(overlay.GetHwnd(), OverlayWindow::WM_CONFIG_RELOADED, 0, 0);
    });

    overlay.Show();

    // Initial query for launch roll-up animation
    SystemMetrics initialMetrics = telemetryEngine.CollectImmediate();
    overlay.UpdateMetrics(initialMetrics);
    overlay.TriggerLaunchAnimation(initialMetrics);

    // Start background telemetry worker thread posting WM_TELEMETRY_UPDATED to overlay window
    telemetryEngine.Start(overlay.GetHwnd(), OverlayWindow::WM_TELEMETRY_UPDATED);

    // Pure Win32 UI message loop - completely unblocked from hardware polling!
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    configWatcher.Stop();

    telemetryEngine.Shutdown();
    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
