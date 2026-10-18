#include "config_watcher.h"
#include <chrono>

#ifdef _WIN32

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
#elif defined(__APPLE__)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>

namespace BareStats {

ConfigWatcher::ConfigWatcher() = default;

ConfigWatcher::~ConfigWatcher() {
    Stop();
}

bool ConfigWatcher::Start(const std::string& configFilePath, std::function<void()> onChangeCallback) {
    Stop();

    configFilePath_ = configFilePath;
    onChangeCallback_ = std::move(onChangeCallback);

    size_t lastSlash = configFilePath_.find_last_of('/');
    if (lastSlash != std::string::npos) {
        watchDir_ = configFilePath_.substr(0, lastSlash);
        targetFileName_ = configFilePath_.substr(lastSlash + 1);
    } else {
        watchDir_ = ".";
        targetFileName_ = configFilePath_;
    }

    if (pipe(stopPipe_) != 0) {
        return false;
    }
    fcntl(stopPipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(stopPipe_[1], F_SETFL, O_NONBLOCK);

    kqFd_ = kqueue();
    if (kqFd_ < 0) {
        close(stopPipe_[0]);
        close(stopPipe_[1]);
        stopPipe_[0] = stopPipe_[1] = -1;
        return false;
    }

    isRunning_.store(true);
    watcherThread_ = std::thread(&ConfigWatcher::WatcherThreadFunc, this);
    return true;
}

void ConfigWatcher::Stop() {
    if (!isRunning_.load()) return;

    isRunning_.store(false);
    if (stopPipe_[1] >= 0) {
        char sig = 1;
        ssize_t written = write(stopPipe_[1], &sig, 1);
        (void)written;
    }

    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    if (kqFd_ >= 0) {
        close(kqFd_);
        kqFd_ = -1;
    }

    if (stopPipe_[0] >= 0) {
        close(stopPipe_[0]);
        close(stopPipe_[1]);
        stopPipe_[0] = stopPipe_[1] = -1;
    }
}

void ConfigWatcher::WatcherThreadFunc() {
    struct kevent changeList[3];
    int changeCount = 0;

    EV_SET(&changeList[changeCount++], stopPipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);

    int dirFd = open(watchDir_.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        EV_SET(&changeList[changeCount++], dirFd, EVFILT_VNODE,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME | NOTE_REVOKE,
               0, nullptr);
    }

    int fileFd = open(configFilePath_.c_str(), O_RDONLY);
    if (fileFd >= 0) {
        EV_SET(&changeList[changeCount++], fileFd, EVFILT_VNODE,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE,
               0, nullptr);
    }

    kevent(kqFd_, changeList, changeCount, nullptr, 0, nullptr);

    struct kevent eventList[4];

    while (isRunning_.load()) {
        int nev = kevent(kqFd_, nullptr, 0, eventList, 4, nullptr);
        if (nev <= 0) break;

        bool shouldStop = false;
        bool fileTriggered = false;

        for (int i = 0; i < nev; ++i) {
            if (static_cast<int>(eventList[i].ident) == stopPipe_[0]) {
                shouldStop = true;
                break;
            }
            if ((dirFd >= 0 && static_cast<int>(eventList[i].ident) == dirFd) ||
                (fileFd >= 0 && static_cast<int>(eventList[i].ident) == fileFd)) {
                fileTriggered = true;
            }
        }

        if (shouldStop) break;

        if (fileTriggered) {
            // Debounce 200ms
            struct timespec timeout;
            timeout.tv_sec = 0;
            timeout.tv_nsec = 200 * 1000 * 1000;

            struct kevent debounceEv;
            int dNev = kevent(kqFd_, nullptr, 0, &debounceEv, 1, &timeout);
            if (dNev > 0 && static_cast<int>(debounceEv.ident) == stopPipe_[0]) {
                break;
            }

            // Re-arm fileFd if changed/replaced
            if (fileFd >= 0) {
                close(fileFd);
                fileFd = -1;
            }
            fileFd = open(configFilePath_.c_str(), O_RDONLY);
            if (fileFd >= 0) {
                struct kevent armEv;
                EV_SET(&armEv, fileFd, EVFILT_VNODE,
                       EV_ADD | EV_ENABLE | EV_CLEAR,
                       NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE,
                       0, nullptr);
                kevent(kqFd_, &armEv, 1, nullptr, 0, nullptr);
            }

            if (isRunning_.load() && onChangeCallback_) {
                onChangeCallback_();
            }
        }
    }

    if (fileFd >= 0) close(fileFd);
    if (dirFd >= 0) close(dirFd);
}

} // namespace BareStats

#else

#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <climits>
#include <cstring>

namespace BareStats {

ConfigWatcher::ConfigWatcher() = default;

ConfigWatcher::~ConfigWatcher() {
    Stop();
}

bool ConfigWatcher::Start(const std::string& configFilePath, std::function<void()> onChangeCallback) {
    Stop();

    configFilePath_ = configFilePath;
    onChangeCallback_ = std::move(onChangeCallback);

    size_t lastSlash = configFilePath_.find_last_of('/');
    if (lastSlash != std::string::npos) {
        watchDir_ = configFilePath_.substr(0, lastSlash);
        targetFileName_ = configFilePath_.substr(lastSlash + 1);
    } else {
        watchDir_ = ".";
        targetFileName_ = configFilePath_;
    }

    if (pipe(stopPipe_) != 0) {
        return false;
    }
    // Set non-blocking on stop write end
    fcntl(stopPipe_[1], F_SETFL, O_NONBLOCK);

    inotifyFd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotifyFd_ < 0) {
        close(stopPipe_[0]);
        close(stopPipe_[1]);
        stopPipe_[0] = stopPipe_[1] = -1;
        return false;
    }

    watchWd_ = inotify_add_watch(inotifyFd_, watchDir_.c_str(),
                                 IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (watchWd_ < 0) {
        close(inotifyFd_);
        close(stopPipe_[0]);
        close(stopPipe_[1]);
        inotifyFd_ = -1;
        stopPipe_[0] = stopPipe_[1] = -1;
        return false;
    }

    isRunning_.store(true);
    watcherThread_ = std::thread(&ConfigWatcher::WatcherThreadFunc, this);
    return true;
}

void ConfigWatcher::Stop() {
    if (!isRunning_.load()) return;

    isRunning_.store(false);
    if (stopPipe_[1] >= 0) {
        char sig = 1;
        ssize_t written = write(stopPipe_[1], &sig, 1);
        (void)written;
    }

    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    if (watchWd_ >= 0 && inotifyFd_ >= 0) {
        inotify_rm_watch(inotifyFd_, watchWd_);
        watchWd_ = -1;
    }

    if (inotifyFd_ >= 0) {
        close(inotifyFd_);
        inotifyFd_ = -1;
    }

    if (stopPipe_[0] >= 0) {
        close(stopPipe_[0]);
        close(stopPipe_[1]);
        stopPipe_[0] = stopPipe_[1] = -1;
    }
}

void ConfigWatcher::WatcherThreadFunc() {
    struct pollfd pfd[2];
    pfd[0].fd = inotifyFd_;
    pfd[0].events = POLLIN;
    pfd[1].fd = stopPipe_[0];
    pfd[1].events = POLLIN;

    char buffer[4096];

    while (isRunning_.load()) {
        int ret = poll(pfd, 2, -1);
        if (ret <= 0) break;

        if (pfd[1].revents & POLLIN) {
            // Stop signal received
            break;
        }

        if (pfd[0].revents & POLLIN) {
            ssize_t len = read(inotifyFd_, buffer, sizeof(buffer));
            if (len <= 0) continue;

            bool targetChanged = false;
            ssize_t i = 0;
            while (i < len) {
                struct inotify_event* ev = reinterpret_cast<struct inotify_event*>(&buffer[i]);
                if (ev->len > 0) {
                    if (targetFileName_ == ev->name) {
                        targetChanged = true;
                        break;
                    }
                }
                i += sizeof(struct inotify_event) + ev->len;
            }

            if (targetChanged) {
                // Debounce 200ms
                struct pollfd debouncePfd;
                debouncePfd.fd = stopPipe_[0];
                debouncePfd.events = POLLIN;
                int dRet = poll(&debouncePfd, 1, 200);
                if (dRet == 0) {
                    if (isRunning_.load() && onChangeCallback_) {
                        onChangeCallback_();
                    }
                } else if (dRet > 0) {
                    break;
                }
            }
        }
    }
}

} // namespace BareStats

#endif
