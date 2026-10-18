#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BareStats {

class ConfigWatcher {
public:
    ConfigWatcher();
    ~ConfigWatcher();

    bool Start(const std::string& configFilePath, std::function<void()> onChangeCallback);
    void Stop();

private:
    void WatcherThreadFunc();

    std::string configFilePath_;
    std::function<void()> onChangeCallback_;
    std::thread watcherThread_;
    std::atomic<bool> isRunning_{ false };

#if defined(_WIN32)
    std::wstring watchDir_;
    std::wstring targetFileName_;
    HANDLE hStopEvent_ = nullptr;
#elif defined(__APPLE__)
    std::string watchDir_;
    std::string targetFileName_;
    int kqFd_ = -1;
    int stopPipe_[2] = { -1, -1 };
#else
    std::string watchDir_;
    std::string targetFileName_;
    int inotifyFd_ = -1;
    int watchWd_ = -1;
    int stopPipe_[2] = { -1, -1 };
#endif
};

} // namespace BareStats
