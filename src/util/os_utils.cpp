#include "os_utils.h"
#include "config/config.h"
#include <fstream>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace BareStats {

namespace fs = std::filesystem;

fs::path GetExecutableDirectory() {
#if defined(_WIN32)
    wchar_t path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path_buf, MAX_PATH);
    if (len > 0) {
        std::error_code ec;
        fs::path p = fs::canonical(fs::path(path_buf), ec);
        if (!ec) {
            return p.parent_path();
        }
        return fs::path(path_buf).parent_path();
    }
#elif defined(__APPLE__)
    char path_buf[2048];
    uint32_t size = sizeof(path_buf);
    if (_NSGetExecutablePath(path_buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::canonical(fs::path(path_buf), ec);
        if (!ec) {
            return p.parent_path();
        }
        return fs::path(path_buf).parent_path();
    }
#elif defined(__linux__)
    char path_buf[2048];
    ssize_t count = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (count > 0) {
        path_buf[count] = '\0';
        std::error_code ec;
        fs::path p = fs::canonical(fs::path(path_buf), ec);
        if (!ec) {
            return p.parent_path();
        }
        return fs::path(path_buf).parent_path();
    }
#endif
    return fs::current_path();
}

fs::path ResolveSatelliteExportPath(const std::string& configPath) {
    std::string effectiveConfig = !configPath.empty() ? configPath : ConfigManager::GetDefaultConfigPath();
    fs::path p(effectiveConfig);
    if (!p.empty()) {
        fs::path parent = p.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            fs::create_directories(parent, ec);
            return parent / "satellite.ini";
        }
    }
    return "satellite.ini";
}

void SafeNotifySend(const std::string& title, const std::string& message, const std::string& urgency) {
#if defined(__linux__)
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            int devNull = open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                dup2(devNull, STDOUT_FILENO);
                dup2(devNull, STDERR_FILENO);
                close(devNull);
            }
            char* const args[] = {
                const_cast<char*>("notify-send"),
                const_cast<char*>("-u"),
                const_cast<char*>(urgency.c_str()),
                const_cast<char*>("-i"),
                const_cast<char*>("barestats"),
                const_cast<char*>(title.c_str()),
                const_cast<char*>(message.c_str()),
                nullptr
            };
            execvp("notify-send", args);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
#else
    (void)title;
    (void)message;
    (void)urgency;
#endif
}

} // namespace BareStats
