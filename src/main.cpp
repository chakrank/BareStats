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
#include "util/formatter.h"

using namespace BareStats;

static const UINT_PTR TIMER_UPDATE_ID = 2001;

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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    std::wstring fullCmd = GetCommandLineW();

    // Support --test or --cli for verification
    if (fullCmd.find(L"--test") != std::wstring::npos || fullCmd.find(L"--cli") != std::wstring::npos) {
        OutputToConsole("\nBareStats (TinyMonitor) CLI Verification\n");
        OutputToConsole("Initializing telemetry collectors...\n");

        std::string configPath = ConfigManager::GetDefaultConfigPath();
        AppConfig config = ConfigManager::Load(configPath);

        CpuCollector cpuCollector;
        MemoryCollector memoryCollector;
        GpuCollector gpuCollector;

        // Warm up CPU delta
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            SystemMetrics metrics;
            metrics.cpu = cpuCollector.Collect();
            metrics.memory = memoryCollector.Collect();
            metrics.gpu = gpuCollector.Collect();

            std::string line = "[" + std::to_string(i + 1) + "/3] " + Formatter::FormatUtf8(metrics, &config) + "\n";
            OutputToConsole(line);

            if (i == 0 && metrics.gpu.isAvailable) {
                std::string gpuInfo = "  [GPU Details] Name: " + metrics.gpu.name + 
                    " | TDP Limit: " + (metrics.gpu.powerLimitWatts.has_value() ? std::to_string(*metrics.gpu.powerLimitWatts) + "W" : "N/A") +
                    " | Slowdown Temp: " + (metrics.gpu.tempSlowdownLimitC.has_value() ? std::to_string(*metrics.gpu.tempSlowdownLimitC) + "C" : "N/A") + "\n";
                OutputToConsole(gpuInfo);
            }
        }

        OutputToConsole("Test completed successfully.\n\n");
        return 0;
    }

    // Normal GUI Overlay mode
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"BareStats_SingleInstance_Mutex");
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

    CpuCollector cpuCollector;
    MemoryCollector memoryCollector;
    GpuCollector gpuCollector;

    OverlayWindow overlay(config);
    if (!overlay.Initialize(hInstance)) {
        if (hMutex) CloseHandle(hMutex);
        MessageBoxW(nullptr, L"Failed to initialize BareStats overlay window.", L"BareStats Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    overlay.Show();

    // Initial query
    SystemMetrics initialMetrics;
    initialMetrics.cpu = cpuCollector.Collect();
    initialMetrics.memory = memoryCollector.Collect();
    initialMetrics.gpu = gpuCollector.Collect();
    overlay.UpdateMetrics(initialMetrics);
    overlay.TriggerLaunchAnimation(initialMetrics);

    // Setup 1-second refresh timer
    SetTimer(overlay.GetHwnd(), TIMER_UPDATE_ID, config.refreshIntervalMs, nullptr);

    // Message Loop
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER && msg.wParam == TIMER_UPDATE_ID) {
            SystemMetrics metrics;
            if (config.showCpu) metrics.cpu = cpuCollector.Collect();
            if (config.showRam) metrics.memory = memoryCollector.Collect();
            if (config.showGpu) metrics.gpu = gpuCollector.Collect();
            overlay.UpdateMetrics(metrics);
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    KillTimer(overlay.GetHwnd(), TIMER_UPDATE_ID);
    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
