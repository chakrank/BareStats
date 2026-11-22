#include "logger.h"
#include "os_utils.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace BareStats {

std::mutex Logger::mutex_;
std::filesystem::path Logger::logPath_;
std::ofstream Logger::fileStream_;
uint64_t Logger::maxSizeBytes_ = 2 * 1024 * 1024; // 2 MB default cap
size_t Logger::maxRotatedArchives_ = 5; // Default maximum rotated log archives to retain
bool Logger::initialized_ = false;

std::filesystem::path Logger::ResolveDefaultLogPath() {
    std::filesystem::path exeDir = GetExecutableDirectory();
    std::error_code ec;

    // 1. Portable Mode: if config.ini or satellite.ini exists next to executable
    if (std::filesystem::exists(exeDir / "config.ini", ec) ||
        std::filesystem::exists(exeDir / "satellite.ini", ec)) {
        return exeDir / "barestats.log";
    }

#if defined(_WIN32)
    char localAppData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::filesystem::path p = std::filesystem::path(localAppData) / "BareStats" / "barestats.log";
        return p;
    }
    return exeDir / "barestats.log";
#elif defined(__APPLE__)
    const char* envHome = std::getenv("HOME");
    if (envHome && envHome[0] != '\0') {
        std::filesystem::path p = std::filesystem::path(envHome) / "Library" / "Logs" / "BareStats" / "barestats.log";
        return p;
    }
    return exeDir / "barestats.log";
#else
    const char* envState = std::getenv("XDG_STATE_HOME");
    if (envState && envState[0] != '\0') {
        return std::filesystem::path(envState) / "barestats" / "barestats.log";
    }
    const char* envHome = std::getenv("HOME");
    if (envHome && envHome[0] != '\0') {
        return std::filesystem::path(envHome) / ".local" / "state" / "barestats" / "barestats.log";
    }
    return exeDir / "barestats.log";
#endif
}

void Logger::Init(const std::filesystem::path& customLogPath) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!customLogPath.empty()) {
        logPath_ = customLogPath;
    } else if (logPath_.empty()) {
        logPath_ = ResolveDefaultLogPath();
    }

    std::error_code ec;
    std::filesystem::create_directories(logPath_.parent_path(), ec);

    if (fileStream_.is_open()) {
        fileStream_.close();
    }

    fileStream_.open(logPath_, std::ios::out | std::ios::app);

    if (!fileStream_.is_open()) {
        // Fallback 1: ~/.config/barestats/barestats.log
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            std::filesystem::path pCfg = std::filesystem::path(home) / ".config" / "barestats" / "barestats.log";
            std::filesystem::create_directories(pCfg.parent_path(), ec);
            fileStream_.open(pCfg, std::ios::out | std::ios::app);
            if (fileStream_.is_open()) {
                logPath_ = pCfg;
            }
        }
    }

    if (!fileStream_.is_open()) {
        // Fallback 2: ~/.cache/barestats/barestats.log
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            std::filesystem::path pCache = std::filesystem::path(home) / ".cache" / "barestats" / "barestats.log";
            std::filesystem::create_directories(pCache.parent_path(), ec);
            fileStream_.open(pCache, std::ios::out | std::ios::app);
            if (fileStream_.is_open()) {
                logPath_ = pCache;
            }
        }
    }

#if !defined(_WIN32)
    if (!fileStream_.is_open()) {
        // Fallback 3: /tmp/barestats_<uid>.log
        std::filesystem::path pTmp = "/tmp/barestats_" + std::to_string(geteuid()) + ".log";
        fileStream_.open(pTmp, std::ios::out | std::ios::app);
        if (fileStream_.is_open()) {
            logPath_ = pTmp;
        }
    }
#endif

    initialized_ = true;
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileStream_.is_open()) {
        fileStream_.flush();
        fileStream_.close();
    }
    initialized_ = false;
}

std::string Logger::GetTimestampIso8601() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t timer = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &timer);
#else
    gmtime_r(&timer, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "INFO ";
    }
}

void Logger::Rotate() {
    if (fileStream_.is_open()) {
        fileStream_.close();
    }

    auto now = std::chrono::system_clock::now();
    std::time_t timer = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &timer);
#else
    gmtime_r(&timer, &tm_buf);
#endif

    char dateBuf[32];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm_buf);

    std::filesystem::path dir = logPath_.parent_path();
    std::filesystem::path stem = logPath_.stem(); // e.g. barestats
    std::filesystem::path ext = logPath_.extension(); // e.g. .log

    std::filesystem::path target = dir / (stem.string() + ext.string() + "." + dateBuf);
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        int idx = 1;
        while (true) {
            std::filesystem::path indexed = dir / (stem.string() + ext.string() + "." + dateBuf + "." + std::to_string(idx));
            if (!std::filesystem::exists(indexed, ec)) {
                target = indexed;
                break;
            }
            idx++;
        }
    }

    std::filesystem::rename(logPath_, target, ec);
    fileStream_.open(logPath_, std::ios::out | std::ios::trunc);

    // BS-VULN-08: Cap accumulated rotated log archives to prevent disk exhaustion
    std::string prefix = stem.string() + ext.string() + ".";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> rotatedFiles;
    for (const auto& entry : std::filesystem::directory_iterator(dir.empty() ? "." : dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::string filename = entry.path().filename().string();
            if (filename.rfind(prefix, 0) == 0) {
                rotatedFiles.push_back({entry.last_write_time(ec), entry.path()});
            }
        }
    }

    if (rotatedFiles.size() > maxRotatedArchives_) {
        std::sort(rotatedFiles.begin(), rotatedFiles.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second.string() < b.second.string();
        });
        size_t toDelete = rotatedFiles.size() - maxRotatedArchives_;
        for (size_t i = 0; i < toDelete; ++i) {
            std::filesystem::remove(rotatedFiles[i].second, ec);
        }
    }
}

void Logger::Log(LogLevel level, const std::string& subsystem, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !fileStream_.is_open()) {
        if (logPath_.empty()) {
            logPath_ = ResolveDefaultLogPath();
        }
        std::error_code ec;
        std::filesystem::create_directories(logPath_.parent_path(), ec);
        if (fileStream_.is_open()) {
            fileStream_.close();
        }
        fileStream_.open(logPath_, std::ios::out | std::ios::app);

        if (!fileStream_.is_open()) {
            const char* home = std::getenv("HOME");
            if (home && home[0] != '\0') {
                std::filesystem::path pCfg = std::filesystem::path(home) / ".config" / "barestats" / "barestats.log";
                std::filesystem::create_directories(pCfg.parent_path(), ec);
                fileStream_.open(pCfg, std::ios::out | std::ios::app);
                if (fileStream_.is_open()) {
                    logPath_ = pCfg;
                }
            }
        }

        if (!fileStream_.is_open()) {
            const char* home = std::getenv("HOME");
            if (home && home[0] != '\0') {
                std::filesystem::path pCache = std::filesystem::path(home) / ".cache" / "barestats" / "barestats.log";
                std::filesystem::create_directories(pCache.parent_path(), ec);
                fileStream_.open(pCache, std::ios::out | std::ios::app);
                if (fileStream_.is_open()) {
                    logPath_ = pCache;
                }
            }
        }

#if !defined(_WIN32)
        if (!fileStream_.is_open()) {
            std::filesystem::path pTmp = "/tmp/barestats_" + std::to_string(geteuid()) + ".log";
            fileStream_.open(pTmp, std::ios::out | std::ios::app);
            if (fileStream_.is_open()) {
                logPath_ = pTmp;
            }
        }
#endif

        initialized_ = true;
    }

    // Check size limit before write
    std::error_code ec;
    uintmax_t curSize = std::filesystem::file_size(logPath_, ec);
    if (!ec && curSize >= maxSizeBytes_) {
        Rotate();
    }

    std::string timestamp = GetTimestampIso8601();
    std::string lvl = LevelToString(level);

    std::string sub = subsystem;
    if (sub.size() < 8) {
        sub.append(8 - sub.size(), ' ');
    } else if (sub.size() > 8) {
        sub = sub.substr(0, 8);
    }

    std::string clean_message;
    clean_message.reserve(message.size());
    for (char c : message) {
        if (c == '\r' || c == '\n') {
            clean_message.push_back(' ');
        } else if (static_cast<unsigned char>(c) >= 32 || c == '\t') {
            clean_message.push_back(c);
        } else {
            clean_message.push_back('?');
        }
    }

    std::string line = timestamp + " | " + lvl + " | " + sub + " | " + clean_message + "\n";

    if (fileStream_.is_open()) {
        fileStream_ << line;
        fileStream_.flush();
    }

    // Mirror Warnings and Errors to stderr so running from CLI / terminal is informative
    if (level == LogLevel::Error || level == LogLevel::Warn) {
        std::cerr << line;
    } else if (!fileStream_.is_open()) {
        // Fallback to stdout if file stream could not be opened on disk
        std::cout << line;
    }
}

void Logger::Info(const std::string& subsystem, const std::string& message) {
    Log(LogLevel::Info, subsystem, message);
}

void Logger::Warn(const std::string& subsystem, const std::string& message) {
    Log(LogLevel::Warn, subsystem, message);
}

void Logger::Error(const std::string& subsystem, const std::string& message) {
    Log(LogLevel::Error, subsystem, message);
}

std::filesystem::path Logger::GetLogPath() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logPath_.empty()) {
        return ResolveDefaultLogPath();
    }
    return logPath_;
}

void Logger::SetLogPath(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    logPath_ = path;
    if (fileStream_.is_open()) {
        fileStream_.close();
    }
    if (!logPath_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(logPath_.parent_path(), ec);
        fileStream_.open(logPath_, std::ios::out | std::ios::app);
        initialized_ = fileStream_.is_open();
    }
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileStream_.is_open()) {
        fileStream_.flush();
    }
}

uint64_t Logger::GetMaxSizeBytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxSizeBytes_;
}

void Logger::SetMaxSizeBytes(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxSizeBytes_ = bytes;
}

size_t Logger::GetMaxRotatedArchives() {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxRotatedArchives_;
}

void Logger::SetMaxRotatedArchives(size_t maxArchives) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxRotatedArchives_ = maxArchives;
}

} // namespace BareStats
