#pragma once

#include "metrics.h"
#include <string>
#include <vector>
#include <optional>

namespace BareStats {

struct LinuxGpuCardInfo {
    uint32_t index = 0;
    std::string cardName;       // e.g. "card0"
    std::string devicePath;     // e.g. "/sys/class/drm/card0/device"
    std::string hwmonPath;      // e.g. "/sys/class/drm/card0/device/hwmon/hwmon0"
    std::string name;           // Human readable e.g. "AMD Radeon RX 7900 XTX"
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t totalVramBytes = 0;
    bool isNvidia = false;
    bool isAmd = false;
    bool isIntel = false;
};

class LinuxSysfsGpuCollector {
public:
    explicit LinuxSysfsGpuCollector(std::string drmBasePath = "/sys/class/drm");

    // Enumerate DRM cards from sysfs
    std::vector<LinuxGpuCardInfo> EnumerateCards();

    // Collect telemetry from a specific card
    bool Collect(const LinuxGpuCardInfo& card, GpuMetrics& metrics);
    bool Collect(uint32_t cardIndex, GpuMetrics& metrics);

    // Helpers to read sysfs attributes (used by collector and tests)
    static std::string ReadSysfsString(const std::string& path);
    static uint64_t ReadSysfsUint64(const std::string& path, uint64_t defaultVal = 0);
    static int64_t ReadSysfsInt64(const std::string& path, int64_t defaultVal = 0);
    static std::string FindHwmonSubdirectory(const std::string& devicePath);

private:
    std::string drmBasePath_;

    bool CollectAmdSysfs(const LinuxGpuCardInfo& card, GpuMetrics& metrics);
    bool CollectIntelSysfs(const LinuxGpuCardInfo& card, GpuMetrics& metrics);
    bool CollectGenericDrmSysfs(const LinuxGpuCardInfo& card, GpuMetrics& metrics);
    void CollectHwmonSensors(const std::string& hwmonPath, GpuMetrics& metrics);
};

} // namespace BareStats
