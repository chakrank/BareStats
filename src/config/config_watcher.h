#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

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
    std::wstring watchDir_;
    std::wstring targetFileName_;
    std::function<void()> onChangeCallback_;

    std::thread watcherThread_;
    HANDLE hStopEvent_ = nullptr;
    std::atomic<bool> isRunning_{ false };
};

} // namespace BareStats
