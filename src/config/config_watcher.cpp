#include "config_watcher.h"
#include <chrono>

namespace BareStats {

ConfigWatcher::ConfigWatcher() = default;

ConfigWatcher::~ConfigWatcher() {
    Stop();
}

static std::wstring Utf8ToWideString(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

bool ConfigWatcher::Start(const std::string& configFilePath, std::function<void()> onChangeCallback) {
    Stop();

    configFilePath_ = configFilePath;
    onChangeCallback_ = std::move(onChangeCallback);

    std::wstring fullPath = Utf8ToWideString(configFilePath_);
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        watchDir_ = fullPath.substr(0, lastSlash);
        targetFileName_ = fullPath.substr(lastSlash + 1);
    } else {
        watchDir_ = L".";
        targetFileName_ = fullPath;
    }

    hStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hStopEvent_) {
        return false;
    }

    isRunning_.store(true);
    watcherThread_ = std::thread(&ConfigWatcher::WatcherThreadFunc, this);
    return true;
}

void ConfigWatcher::Stop() {
    if (!isRunning_.load()) return;

    isRunning_.store(false);
    if (hStopEvent_) {
        SetEvent(hStopEvent_);
    }

    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    if (hStopEvent_) {
        CloseHandle(hStopEvent_);
        hStopEvent_ = nullptr;
    }
}

void ConfigWatcher::WatcherThreadFunc() {
    HANDLE hDir = CreateFileW(
        watchDir_.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        return;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        CloseHandle(hDir);
        return;
    }

    alignas(DWORD) BYTE buffer[2048];

    while (isRunning_.load()) {
        ResetEvent(overlapped.hEvent);
        DWORD bytesReturned = 0;

        BOOL success = ReadDirectoryChangesW(
            hDir,
            buffer,
            sizeof(buffer),
            FALSE, // Shallow watch, single directory
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned,
            &overlapped,
            nullptr
        );

        if (!success) {
            break;
        }

        HANDLE waitHandles[2] = { hStopEvent_, overlapped.hEvent };
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitRes == WAIT_OBJECT_0) {
            // Stop event was signaled
            CancelIoEx(hDir, &overlapped);
            break;
        }

        if (waitRes == WAIT_OBJECT_0 + 1) {
            DWORD transferred = 0;
            if (GetOverlappedResult(hDir, &overlapped, &transferred, FALSE) && transferred > 0) {
                FILE_NOTIFY_INFORMATION* pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                bool targetChanged = false;

                while (pNotify) {
                    std::wstring changedFile(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
                    if (_wcsicmp(changedFile.c_str(), targetFileName_.c_str()) == 0) {
                        targetChanged = true;
                        break;
                    }

                    if (pNotify->NextEntryOffset == 0) break;
                    pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(pNotify) + pNotify->NextEntryOffset);
                }

                if (targetChanged) {
                    // Debounce: wait 200ms for editor file saving (e.g., Notepad, VS Code) to finalize
                    if (WaitForSingleObject(hStopEvent_, 200) == WAIT_TIMEOUT) {
                        if (isRunning_.load() && onChangeCallback_) {
                            onChangeCallback_();
                        }
                    }
                }
            }
        }
    }

    CancelIoEx(hDir, &overlapped);
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
}

} // namespace BareStats
