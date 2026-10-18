#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "overlay/windows/overlay_window.h"
#elif defined(__APPLE__)
#include <csignal>
#include "overlay/macos/macos_overlay.h"
#else
#include <csignal>
#include "overlay/linux/linux_overlay.h"
#endif

#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include "metrics/cpu_collector.h"
#include "metrics/memory_collector.h"
#include "metrics/gpu_collector.h"
#include "metrics/network_collector.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "util/formatter.h"
#include "metrics/telemetry_engine.h"

using namespace BareStats;

#ifdef _WIN32

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
        OutputToConsole("\nBareStats CLI Verification (v1.3.0)\n");
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
            bool isTarget = config.targetAllGpus ||
                (std::find(config.targetGpuIndices.begin(), config.targetGpuIndices.end(), g.index) != config.targetGpuIndices.end());
            std::string targetTag = isTarget ? " [Active Target]" : "";
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
            metrics.gpus = gpuCollector.CollectMultiple(config.targetGpuIndices, config.targetAllGpus);
            if (!metrics.gpus.empty()) {
                metrics.gpu = metrics.gpus[0];
            }
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

                    std::string vramModeStr = (config.vramMode == VramMode::Usable) ? "usable" : "physical";
                    uint64_t usableBytes = (metrics.gpu.memoryTotalBytes > metrics.gpu.memoryReservedBytes) 
                        ? (metrics.gpu.memoryTotalBytes - metrics.gpu.memoryReservedBytes) 
                        : metrics.gpu.memoryTotalBytes;
                    std::string vramInfo = "  [VRAM Mode: " + vramModeStr + "] Total: " + 
                        std::to_string(metrics.gpu.memoryTotalBytes / (1024 * 1024)) + "MB | Reserved: " + 
                        std::to_string(metrics.gpu.memoryReservedBytes / (1024 * 1024)) + "MB | Usable: " + 
                        std::to_string(usableBytes / (1024 * 1024)) + "MB\n";
                    OutputToConsole(vramInfo);
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

    // Connect config reload callback to wake telemetry engine immediately with new config
    overlay.SetConfigReloadCallback([&overlay, &telemetryEngine]() {
        telemetryEngine.UpdateConfig(overlay.GetConfig());
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

#elif defined(__APPLE__)

#include <fstream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <signal.h>
#include <thread>
#include <iostream>
#include <dispatch/dispatch.h>

static void HandleMacosSignal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        MacosOverlay::RequestStop();
    } else if (sig == SIGHUP) {
        MacosOverlay::RequestReloadConfig();
    } else if (sig == SIGUSR1) {
        MacosOverlay::RequestToggleVisibility();
    } else if (sig == SIGUSR2) {
        MacosOverlay::RequestTogglePositioningMode();
    }
}

static std::string GetMacosSocketPath() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/Library/Application Support/BareStats/barestats.sock";
    }
    return "/tmp/barestats.sock";
}

static std::string GetMacosLockFilePath() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/Library/Application Support/BareStats/barestats.lock";
    }
    return "/tmp/barestats.lock";
}

static bool SendCommandToRunningInstance(const std::string& cmd) {
    std::string sockPath = GetMacosSocketPath();
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        std::string payload = cmd + "\n";
        (void)write(sock, payload.data(), payload.size());
        close(sock);
        return true;
    }
    close(sock);
    return false;
}

int main(int argc, char* argv[]) {
    std::string explicitConfigPath;
    bool isTestMode = false;

    // First pass: extract explicit config path if present
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            explicitConfigPath = argv[++i];
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test" || arg == "--cli") {
            isTestMode = true;
            break;
        }
        if (arg == "--config" || arg == "-c") {
            ++i; // already handled
            continue;
        }
        if (arg == "--toggle") {
            bool ok = SendCommandToRunningInstance("TOGGLE");
            return ok ? 0 : 1;
        }
        if (arg == "--move") {
            bool ok = SendCommandToRunningInstance("MOVE");
            return ok ? 0 : 1;
        }
        if (arg == "--reload") {
            bool ok = SendCommandToRunningInstance("RELOAD");
            return ok ? 0 : 1;
        }
        if (arg == "--quit" || arg == "--exit") {
            bool ok = SendCommandToRunningInstance("QUIT");
            return ok ? 0 : 1;
        }
        if (arg == "--preset" && i + 1 < argc) {
            std::string presetStr = argv[++i];
            bool ok = SendCommandToRunningInstance("PRESET " + presetStr);
            if (!ok) {
                std::string configPath = !explicitConfigPath.empty() ? explicitConfigPath : ConfigManager::GetDefaultConfigPath();
                AppConfig cfg = ConfigManager::Load(configPath);
                if (presetStr == "next") {
                    static const PositionPreset kPresets[] = {
                        PositionPreset::BottomCenter, PositionPreset::BottomRight,
                        PositionPreset::TopRight, PositionPreset::TopCenter,
                        PositionPreset::TopLeft, PositionPreset::BottomLeft
                    };
                    int count = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
                    int nextIdx = 0;
                    for (int p = 0; p < count; ++p) {
                        if (cfg.preset == kPresets[p]) { nextIdx = (p + 1) % count; break; }
                    }
                    cfg.preset = kPresets[nextIdx];
                } else {
                    cfg.preset = MonitorHelper::StringToPreset(presetStr);
                }
                cfg.customX = -1;
                cfg.customY = -1;
                ConfigManager::Save(configPath, cfg);
                std::cout << "Position preset set to: " << MonitorHelper::PresetToString(cfg.preset) << " in " << configPath << "\n";
            }
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "BareStats v1.3.0 (macOS) - Ultra-lightweight native telemetry overlay\n"
                      << "Usage: barestats [OPTIONS]\n\n"
                      << "Configuration Loading Priority:\n"
                      << "  1. CLI parameter:   -c, --config <PATH>\n"
                      << "  2. Local directory: ./config.ini (current working directory)\n"
                      << "  3. Portable Mode:   <bundle_dir>/config.ini\n"
                      << "  4. Application:     ~/Library/Application Support/BareStats/config.ini\n"
                      << "  5. Fallback:        ~/.config/barestats/config.ini\n\n"
                      << "Menu Bar Hotkeys (macOS):\n"
                      << "  Ctrl + Opt + M    Toggle Move / Positioning mode (drag with mouse)\n"
                      << "  Ctrl + Opt + H    Toggle overlay visibility (Hide / Show)\n"
                      << "  Ctrl + Opt + R    Reload configuration\n"
                      << "  Escape            Exit Move mode\n\n"
                      << "CLI Options:\n"
                      << "  -c, --config <PATH> Specify custom configuration file path\n"
                      << "  --toggle            Toggle overlay visibility of running instance\n"
                      << "  --move              Toggle move / positioning mode of running instance\n"
                      << "  --preset <NAME>     Set preset (TopLeft, TopCenter, TopRight,\n"
                      << "                      BottomLeft, BottomCenter, BottomRight, next)\n"
                      << "  --test, --cli       Run telemetry verification in terminal and exit\n"
                      << "  --help, -h          Display this help message and exit\n";
            return 0;
        }
    }

    std::string configPath = !explicitConfigPath.empty() ? explicitConfigPath : ConfigManager::GetDefaultConfigPath();

    if (isTestMode) {
        std::cout << "\nBareStats CLI Verification (v1.3.0 - macOS)\n";
        std::cout << "Using configuration: " << configPath << "\n";
        std::cout << "Initializing telemetry collectors...\n";

        AppConfig config = ConfigManager::Load(configPath);

        CpuCollector cpuCollector;
        MemoryCollector memoryCollector;
        GpuCollector gpuCollector;
        NetworkCollector networkCollector;

        auto gpus = gpuCollector.GetAvailableGpus();
        std::cout << "Discovered " << gpus.size() << " hardware GPU adapter(s):\n";
        for (const auto& g : gpus) {
            std::string vendorName = (g.vendorId == 0x10DE) ? "NVIDIA" :
                                     (g.vendorId == 0x8086) ? "Intel" :
                                     (g.vendorId == 0x1002) ? "AMD" :
                                     (g.vendorId == 0x106B) ? "Apple Silicon" : "Generic";
            std::cout << "  [GPU " << g.index << "] " << g.name << " (" << vendorName << ")\n";
        }

        // Warm up delta baselines
        cpuCollector.Collect();
        networkCollector.Collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        SystemMetrics sample;
        sample.cpu = cpuCollector.Collect();
        sample.memory = memoryCollector.Collect();
        sample.gpus = gpuCollector.CollectMultiple(config.targetGpuIndices, config.targetAllGpus);
        if (!sample.gpus.empty()) sample.gpu = sample.gpus[0];
        sample.network = networkCollector.Collect();

        std::cout << "CPU: " << sample.cpu.utilizationPercent << "%\n";
        std::cout << "RAM: " << (sample.memory.usedBytes / (1024 * 1024)) << " / "
                  << (sample.memory.totalBytes / (1024 * 1024)) << " MB\n";
        if (sample.gpu.isAvailable) {
            std::cout << "GPU: " << sample.gpu.name << " (Load: "
                      << (sample.gpu.utilizationPercent ? std::to_string(static_cast<int>(*sample.gpu.utilizationPercent)) + "%" : "N/A")
                      << ", VRAM: " << (sample.gpu.memoryUsedBytes / (1024 * 1024)) << " / "
                      << (sample.gpu.memoryTotalBytes / (1024 * 1024)) << " MB)\n";
        }
        if (sample.network.isAvailable) {
            std::cout << "NET: " << sample.network.primaryAdapterName << " (Down: "
                      << (sample.network.rxBytesPerSec / 1024) << " KB/s, Up: "
                      << (sample.network.txBytesPerSec / 1024) << " KB/s)\n";
        }
        std::cout << "Formatted Ribbon: " << Formatter::FormatUtf8(sample, &config) << "\n";
        std::cout << "Verification completed successfully.\n\n";
        return 0;
    }

    // Single-instance enforcement via POSIX file lock
    std::string lockPath = GetMacosLockFilePath();
    int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        // Another instance is already active! Wake it up with launch animation
        std::cout << "BareStats instance already active. Waking existing overlay...\n";
        SendCommandToRunningInstance("WAKE");
        close(lockFd);
        return 0;
    }

    AppConfig config = ConfigManager::Load(configPath);
    TelemetryEngine telemetryEngine(config);

    MacosOverlay overlay(config, configPath);
    if (!overlay.Initialize()) {
        std::cerr << "Failed to initialize macOS overlay surface." << std::endl;
        if (lockFd >= 0) close(lockFd);
        return 1;
    }

    std::signal(SIGINT, HandleMacosSignal);
    std::signal(SIGTERM, HandleMacosSignal);
    std::signal(SIGUSR1, HandleMacosSignal);
    std::signal(SIGUSR2, HandleMacosSignal);
    std::signal(SIGHUP, HandleMacosSignal);

    // Setup local IPC domain socket for commands from secondary instances / CLI
    std::string sockPath = GetMacosSocketPath();
    unlink(sockPath.c_str());

    int serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
    std::thread ipcThread;
    if (serverSock >= 0) {
        struct sockaddr_un saddr{};
        saddr.sun_family = AF_UNIX;
        std::strncpy(saddr.sun_path, sockPath.c_str(), sizeof(saddr.sun_path) - 1);
        if (bind(serverSock, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) == 0) {
            listen(serverSock, 5);
            ipcThread = std::thread([serverSock, &overlay, &telemetryEngine, configPath]() {
                while (true) {
                    int client = accept(serverSock, nullptr, nullptr);
                    if (client < 0) break;
                    char buf[256] = { 0 };
                    ssize_t n = read(client, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        std::string req(buf);
                        while (!req.empty() && (req.back() == '\r' || req.back() == '\n')) req.pop_back();

                        if (req == "TOGGLE") {
                            overlay.ToggleVisibility();
                        } else if (req == "MOVE") {
                            overlay.TogglePositioningMode();
                        } else if (req == "RELOAD") {
                            AppConfig reloaded = ConfigManager::Load(configPath);
                            overlay.ApplyConfig(reloaded);
                            telemetryEngine.UpdateConfig(reloaded);
                            overlay.TriggerLaunchAnimation(overlay.GetLastMetrics());
                        } else if (req.rfind("PRESET", 0) == 0) {
                            std::string pStr = (req.size() > 7) ? req.substr(7) : "";
                            if (pStr == "next") {
                                overlay.CyclePositionPreset();
                            } else {
                                overlay.SetPreset(MonitorHelper::StringToPreset(pStr));
                            }
                        } else if (req == "QUIT" || req == "EXIT") {
                            overlay.Stop();
                            close(client);
                            break;
                        } else if (req == "WAKE") {
                            overlay.Show();
                            overlay.TriggerLaunchAnimation(overlay.GetLastMetrics());
                        }
                    }
                    close(client);
                }
            });
        }
    }

    ConfigWatcher configWatcher;
    configWatcher.Start(configPath, [&overlay, &telemetryEngine, configPath]() {
        dispatch_async(dispatch_get_main_queue(), ^{
            AppConfig reloaded = ConfigManager::Load(configPath);
            overlay.ApplyConfig(reloaded);
            telemetryEngine.UpdateConfig(reloaded);
            overlay.TriggerLaunchAnimation(overlay.GetLastMetrics());
        });
    });

    bool firstTelemetryFrame = true;
    telemetryEngine.Start([&overlay, &telemetryEngine, &firstTelemetryFrame]() {
        auto m = telemetryEngine.GetLatestMetrics();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (firstTelemetryFrame) {
                firstTelemetryFrame = false;
                overlay.TriggerLaunchAnimation(m);
            } else {
                overlay.UpdateMetrics(m);
            }
        });
    });

    overlay.Show();
    overlay.Run();

    configWatcher.Stop();
    telemetryEngine.Shutdown();

    if (serverSock >= 0) {
        close(serverSock);
        unlink(sockPath.c_str());
    }
    if (ipcThread.joinable()) {
        ipcThread.join();
    }

    if (lockFd >= 0) {
        flock(lockFd, LOCK_UN);
        close(lockFd);
        unlink(lockPath.c_str());
    }

    return 0;
}

#else // Linux Entry Point

#include <fstream>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>

static void HandleSignal(int) {
    LinuxOverlay::RequestStop();
}

static void HandleToggleSignal(int) {
    LinuxOverlay::RequestToggleVisibility();
}

static void HandleMoveSignal(int) {
    LinuxOverlay::RequestTogglePositioningMode();
}

static void HandleReloadSignal(int) {
    LinuxOverlay::RequestReloadConfig();
}

static std::string GetPidFilePath() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir && runtimeDir[0] != '\0') {
        return std::string(runtimeDir) + "/barestats.pid";
    }
    return "/tmp/barestats.pid";
}

static bool SendSignalToRunningInstance(int sig) {
    std::string pidFile = GetPidFilePath();
    std::ifstream f(pidFile);
    if (f.is_open()) {
        pid_t pid = 0;
        if (f >> pid && pid > 0 && pid != getpid()) {
            if (kill(pid, sig) == 0) {
                return true;
            }
        }
    }
    // Fallback using pkill (ignoring the signal in this process first)
    std::signal(sig, SIG_IGN);
    std::string cmd = (sig == SIGUSR1)
        ? "pkill -USR1 --oldest -x barestats 2>/dev/null"
        : (sig == SIGUSR2)
            ? "pkill -USR2 --oldest -x barestats 2>/dev/null"
            : (sig == SIGHUP)
                ? "pkill -HUP --oldest -x barestats 2>/dev/null"
                : (sig == SIGTERM)
                    ? "pkill -TERM --oldest -x barestats 2>/dev/null"
                    : "pkill -x barestats 2>/dev/null";
    int ret = std::system(cmd.c_str());
    return (ret == 0);
}

int main(int argc, char* argv[]) {
    std::string explicitConfigPath;
    bool isTestMode = false;

    // First pass: extract explicit config path if present
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            explicitConfigPath = argv[++i];
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test" || arg == "--cli") {
            isTestMode = true;
            break;
        }
        if (arg == "--config" || arg == "-c") {
            ++i; // already handled
            continue;
        }
        if (arg == "--toggle") {
            bool ok = SendSignalToRunningInstance(SIGUSR1);
            return ok ? 0 : 1;
        }
        if (arg == "--move") {
            bool ok = SendSignalToRunningInstance(SIGUSR2);
            return ok ? 0 : 1;
        }
        if (arg == "--reload") {
            bool ok = SendSignalToRunningInstance(SIGHUP);
            return ok ? 0 : 1;
        }
        if (arg == "--quit" || arg == "--exit") {
            bool ok = SendSignalToRunningInstance(SIGTERM);
            return ok ? 0 : 1;
        }
        if (arg == "--preset" && i + 1 < argc) {
            std::string presetStr = argv[++i];
            std::string configPath = !explicitConfigPath.empty() ? explicitConfigPath : ConfigManager::GetDefaultConfigPath();
            AppConfig cfg = ConfigManager::Load(configPath);
            if (presetStr == "next") {
                static const PositionPreset kPresets[] = {
                    PositionPreset::BottomCenter, PositionPreset::BottomRight,
                    PositionPreset::TopRight, PositionPreset::TopCenter,
                    PositionPreset::TopLeft, PositionPreset::BottomLeft
                };
                int count = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
                int nextIdx = 0;
                for (int p = 0; p < count; ++p) {
                    if (cfg.preset == kPresets[p]) { nextIdx = (p + 1) % count; break; }
                }
                cfg.preset = kPresets[nextIdx];
            } else {
                cfg.preset = MonitorHelper::StringToPreset(presetStr);
            }
            cfg.customX = -1;
            cfg.customY = -1;
            ConfigManager::Save(configPath, cfg);
            std::cout << "Position preset set to: " << MonitorHelper::PresetToString(cfg.preset) << " in " << configPath << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "BareStats v1.3.0 - Ultra-lightweight native telemetry overlay\n"
                      << "Usage: barestats [OPTIONS]\n\n"
                      << "Configuration Loading Priority:\n"
                      << "  1. CLI parameter:   -c, --config <PATH>\n"
                      << "  2. Local directory: ./config.ini (current working directory)\n"
                      << "  3. Portable Mode:   <exe_dir>/config.ini\n"
                      << "  4. User Profile:    ~/.config/barestats/config.ini\n\n"
                      << "Global Hotkeys (X11):\n"
                      << "  Ctrl + Alt + M    Toggle Move / Positioning mode (drag with mouse)\n"
                      << "  Ctrl + Alt + P    Cycle preset position (BottomCenter, TopRight, etc.)\n"
                      << "  Ctrl + Alt + H    Toggle overlay visibility (Hide / Show)\n"
                      << "  Escape            Exit Move mode\n\n"
                      << "Wayland / Ubuntu Shortcuts:\n"
                      << "  In Wayland, map custom shortcuts in Ubuntu Settings -> Keyboard -> Shortcuts:\n"
                      << "    - Move Mode:      barestats --move\n"
                      << "    - Cycle Preset:   barestats --preset next\n"
                      << "    - Toggle Overlay:  barestats --toggle\n\n"
                      << "CLI Options:\n"
                      << "  -c, --config <PATH> Specify custom configuration file path\n"
                      << "  --toggle            Toggle overlay visibility of running instance\n"
                      << "  --move              Toggle move / positioning mode of running instance\n"
                      << "  --preset <NAME>     Set preset (TopLeft, TopCenter, TopRight,\n"
                      << "                      BottomLeft, BottomCenter, BottomRight, next)\n"
                      << "  --test, --cli       Run telemetry verification in terminal and exit\n"
                      << "  --help, -h          Display this help message and exit\n";
            return 0;
        }
    }

    std::string configPath = !explicitConfigPath.empty() ? explicitConfigPath : ConfigManager::GetDefaultConfigPath();

    if (isTestMode) {
        std::cout << "\nBareStats CLI Verification (v1.3.0)\n";
        std::cout << "Using configuration: " << configPath << "\n";
        std::cout << "Initializing telemetry collectors...\n";

        AppConfig config = ConfigManager::Load(configPath);

        CpuCollector cpuCollector;
        MemoryCollector memoryCollector;
        GpuCollector gpuCollector;
        NetworkCollector networkCollector;

        auto gpus = gpuCollector.GetAvailableGpus();
        std::cout << "Discovered " << gpus.size() << " hardware GPU adapter(s):\n";
        for (const auto& g : gpus) {
            std::string vendorName = (g.vendorId == 0x10DE) ? "NVIDIA" :
                                     (g.vendorId == 0x8086) ? "Intel" :
                                     (g.vendorId == 0x1002) ? "AMD" : "Generic DRM";
            std::cout << "  [GPU " << g.index << "] " << g.name << " (" << vendorName << ")\n";
        }

        // Warm up delta baselines
        cpuCollector.Collect();
        networkCollector.Collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        SystemMetrics sample;
        sample.cpu = cpuCollector.Collect();
        sample.memory = memoryCollector.Collect();
        sample.gpus = gpuCollector.CollectMultiple(config.targetGpuIndices, config.targetAllGpus);
        if (!sample.gpus.empty()) sample.gpu = sample.gpus[0];
        sample.network = networkCollector.Collect();

        std::cout << "CPU: " << sample.cpu.utilizationPercent << "%\n";
        std::cout << "RAM: " << (sample.memory.usedBytes / (1024 * 1024)) << " / "
                  << (sample.memory.totalBytes / (1024 * 1024)) << " MB\n";
        std::cout << "Verification completed successfully.\n\n";
        return 0;
    }

    AppConfig config = ConfigManager::Load(configPath);

    // Single-instance enforcement: check if an existing instance is already running
    std::string pidFile = GetPidFilePath();
    {
        std::ifstream pf(pidFile);
        if (pf.is_open()) {
            pid_t existingPid = 0;
            if (pf >> existingPid && existingPid > 0 && existingPid != getpid()) {
                if (kill(existingPid, 0) == 0) {
                    // Instance already running! Trigger reload or wake it up
                    std::cout << "BareStats instance already active (PID " << existingPid << "). Reloading config...\n";
                    kill(existingPid, SIGUSR1);
                    return 0;
                }
            }
        }
    }

    // Secondary check: scan running processes named barestats to catch previous versions or orphaned processes
    {
        FILE* pipe = popen("pgrep -x barestats 2>/dev/null", "r");
        if (pipe) {
            char buf[64];
            while (fgets(buf, sizeof(buf), pipe)) {
                pid_t p = static_cast<pid_t>(std::atoi(buf));
                if (p > 0 && p != getpid() && kill(p, 0) == 0) {
                    pclose(pipe);
                    std::cout << "BareStats instance already active (PID " << p << "). Reloading config...\n";
                    kill(p, SIGUSR1);
                    return 0;
                }
            }
            pclose(pipe);
        }
    }

    TelemetryEngine telemetryEngine(config);

    LinuxOverlay overlay(config, configPath);
    if (!overlay.Initialize()) {
        std::cerr << "Failed to initialize Linux overlay surface." << std::endl;
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGUSR1, HandleToggleSignal);
    std::signal(SIGUSR2, HandleMoveSignal);
    std::signal(SIGHUP, HandleReloadSignal);

    // Write runtime PID file for single-instance signals
    {
        std::ofstream pf(pidFile, std::ios::trunc);
        if (pf.is_open()) {
            pf << getpid() << "\n";
        }
    }

    ConfigWatcher configWatcher;
    configWatcher.Start(configPath, [&overlay, &telemetryEngine, configPath]() {
        AppConfig reloaded = ConfigManager::Load(configPath);
        overlay.ApplyConfig(reloaded);
        telemetryEngine.UpdateConfig(reloaded);
        overlay.TriggerLaunchAnimation(overlay.GetLastMetrics());
    });

    bool firstTelemetryFrame = true;
    telemetryEngine.Start([&overlay, &telemetryEngine, &firstTelemetryFrame]() {
        auto m = telemetryEngine.GetLatestMetrics();
        if (firstTelemetryFrame) {
            firstTelemetryFrame = false;
            overlay.TriggerLaunchAnimation(m);
        } else {
            overlay.UpdateMetrics(m);
        }
    });

    overlay.Show();
    overlay.Run();

    configWatcher.Stop();
    telemetryEngine.Shutdown();

    // Clean up runtime PID file
    unlink(pidFile.c_str());
    return 0;
}

#endif // _WIN32
