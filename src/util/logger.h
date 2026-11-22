#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <cstdint>

namespace BareStats {

enum class LogLevel {
    Info,
    Warn,
    Error
};

class Logger {
public:
    static void Init(const std::filesystem::path& customLogPath = {});
    static void Shutdown();

    static void Log(LogLevel level, const std::string& subsystem, const std::string& message);
    static void Info(const std::string& subsystem, const std::string& message);
    static void Warn(const std::string& subsystem, const std::string& message);
    static void Error(const std::string& subsystem, const std::string& message);

    static std::filesystem::path GetLogPath();
    static void SetLogPath(const std::filesystem::path& path);
    static uint64_t GetMaxSizeBytes();
    static void SetMaxSizeBytes(uint64_t bytes);
    static size_t GetMaxRotatedArchives();
    static void SetMaxRotatedArchives(size_t maxArchives);
    static void Flush();

    static void Rotate();

private:
    static std::string GetTimestampIso8601();
    static std::string LevelToString(LogLevel level);
    static std::filesystem::path ResolveDefaultLogPath();

    static std::mutex mutex_;
    static std::filesystem::path logPath_;
    static std::ofstream fileStream_;
    static uint64_t maxSizeBytes_;
    static size_t maxRotatedArchives_;
    static bool initialized_;
};

#define LOG_INFO(subsys, msg)  ::BareStats::Logger::Info(subsys, msg)
#define LOG_WARN(subsys, msg)  ::BareStats::Logger::Warn(subsys, msg)
#define LOG_ERROR(subsys, msg) ::BareStats::Logger::Error(subsys, msg)

} // namespace BareStats
