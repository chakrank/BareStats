#include "linux_sysfs_gpu_collector.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cctype>

namespace BareStats {

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::string LinuxSysfsGpuCollector::ReadSysfsString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    if (std::getline(file, line)) {
        return Trim(line);
    }
    return "";
}

uint64_t LinuxSysfsGpuCollector::ReadSysfsUint64(const std::string& path, uint64_t defaultVal) {
    std::string str = ReadSysfsString(path);
    if (str.empty()) return defaultVal;
    try {
        return std::stoull(str, nullptr, 0); // Handles decimal and 0x hex
    } catch (...) {
        return defaultVal;
    }
}

int64_t LinuxSysfsGpuCollector::ReadSysfsInt64(const std::string& path, int64_t defaultVal) {
    std::string str = ReadSysfsString(path);
    if (str.empty()) return defaultVal;
    try {
        return std::stoll(str, nullptr, 0);
    } catch (...) {
        return defaultVal;
    }
}

std::string LinuxSysfsGpuCollector::FindHwmonSubdirectory(const std::string& devicePath) {
    std::string hwmonRoot = devicePath + "/hwmon";
    std::error_code ec;
    if (!std::filesystem::exists(hwmonRoot, ec)) {
        return "";
    }

    for (const auto& entry : std::filesystem::directory_iterator(hwmonRoot, ec)) {
        if (entry.is_directory(ec)) {
            std::string filename = entry.path().filename().string();
            if (filename.compare(0, 5, "hwmon") == 0) {
                return entry.path().string();
            }
        }
    }

    return "";
}

LinuxSysfsGpuCollector::LinuxSysfsGpuCollector(std::string drmBasePath)
    : drmBasePath_(std::move(drmBasePath)) {
}

std::vector<LinuxGpuCardInfo> LinuxSysfsGpuCollector::EnumerateCards() {
    std::vector<LinuxGpuCardInfo> cards;
    std::error_code ec;

    if (!std::filesystem::exists(drmBasePath_, ec)) {
        return cards;
    }

    for (const auto& entry : std::filesystem::directory_iterator(drmBasePath_, ec)) {
        std::string filename = entry.path().filename().string();

        // Match card[0-9]+ only (reject card0-DP-1, card0-HDMI-A-1, etc.)
        if (filename.compare(0, 4, "card") != 0) continue;
        if (filename.size() <= 4) continue;

        bool allDigits = true;
        for (size_t i = 4; i < filename.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(filename[i]))) {
                allDigits = false;
                break;
            }
        }
        if (!allDigits) continue;

        uint32_t cardIdx = 0;
        try {
            cardIdx = static_cast<uint32_t>(std::stoul(filename.substr(4)));
        } catch (...) {
            continue;
        }

        std::string cardDir = entry.path().string();
        std::string devicePath = cardDir + "/device";

        LinuxGpuCardInfo card;
        card.index = cardIdx;
        card.cardName = filename;
        card.devicePath = devicePath;

        card.vendorId = static_cast<uint32_t>(ReadSysfsUint64(devicePath + "/vendor", 0));
        card.deviceId = static_cast<uint32_t>(ReadSysfsUint64(devicePath + "/device", 0));
        card.hwmonPath = FindHwmonSubdirectory(devicePath);

        card.isNvidia = (card.vendorId == 0x10DE);
        card.isAmd = (card.vendorId == 0x1002);
        card.isIntel = (card.vendorId == 0x8086);

        std::string customLabel = ReadSysfsString(devicePath + "/label");
        if (!customLabel.empty()) {
            card.name = customLabel;
        } else {
            if (card.isNvidia) {
                card.name = "NVIDIA Graphics (" + filename + ")";
            } else if (card.isAmd) {
                card.name = "AMD Radeon Graphics (" + filename + ")";
            } else if (card.isIntel) {
                card.name = "Intel Graphics (" + filename + ")";
            } else {
                card.name = "DRM KMS Display (" + filename + ")";
            }
        }

        if (card.isAmd) {
            card.totalVramBytes = ReadSysfsUint64(devicePath + "/mem_info_vram_total", 0);
        } else if (card.isIntel) {
            card.totalVramBytes = ReadSysfsUint64(devicePath + "/lmem_total_bytes", 0);
        }

        cards.push_back(std::move(card));
    }

    std::sort(cards.begin(), cards.end(), [](const LinuxGpuCardInfo& a, const LinuxGpuCardInfo& b) {
        return a.index < b.index;
    });

    return cards;
}

void LinuxSysfsGpuCollector::CollectHwmonSensors(const std::string& hwmonPath, GpuMetrics& metrics) {
    if (hwmonPath.empty()) return;

    // Temperature (temp1_input is in millidegrees C)
    int64_t tempInput = ReadSysfsInt64(hwmonPath + "/temp1_input", -1);
    if (tempInput > 0) {
        metrics.temperatureC = static_cast<uint32_t>(tempInput / 1000);
    }

    int64_t tempCrit = ReadSysfsInt64(hwmonPath + "/temp1_crit", -1);
    if (tempCrit <= 0) {
        tempCrit = ReadSysfsInt64(hwmonPath + "/temp1_max", -1);
    }
    if (tempCrit > 0) {
        metrics.tempSlowdownLimitC = static_cast<uint32_t>(tempCrit / 1000);
    }

    // Power (power1_average / power1_input is in microwatts)
    int64_t powerAvg = ReadSysfsInt64(hwmonPath + "/power1_average", -1);
    if (powerAvg <= 0) {
        powerAvg = ReadSysfsInt64(hwmonPath + "/power1_input", -1);
    }
    if (powerAvg > 0) {
        metrics.powerWatts = static_cast<uint32_t>(powerAvg / 1000000);
    }

    int64_t powerCap = ReadSysfsInt64(hwmonPath + "/power1_cap", -1);
    if (powerCap > 0) {
        metrics.powerLimitWatts = static_cast<uint32_t>(powerCap / 1000000);
    }

    // Fan Speed (fan1_input is in RPM, pwm1 is 0-255)
    int64_t fanRpm = ReadSysfsInt64(hwmonPath + "/fan1_input", -1);
    if (fanRpm >= 0) {
        metrics.fanRpm = static_cast<uint32_t>(fanRpm);
    }

    int64_t pwm = ReadSysfsInt64(hwmonPath + "/pwm1", -1);
    if (pwm >= 0) {
        int pct = static_cast<int>(std::round((static_cast<double>(pwm) / 255.0) * 100.0));
        metrics.fanSpeedPercent = static_cast<uint32_t>(std::clamp(pct, 0, 100));
    }
}

bool LinuxSysfsGpuCollector::CollectAmdSysfs(const LinuxGpuCardInfo& card, GpuMetrics& metrics) {
    // AMD Core Utilization via gpu_busy_percent (0-100)
    int64_t busy = ReadSysfsInt64(card.devicePath + "/gpu_busy_percent", -1);
    if (busy >= 0) {
        metrics.utilizationPercent = std::clamp(static_cast<double>(busy), 0.0, 100.0);
    }

    // AMD VRAM
    uint64_t vramUsed = ReadSysfsUint64(card.devicePath + "/mem_info_vram_used", 0);
    uint64_t vramTotal = ReadSysfsUint64(card.devicePath + "/mem_info_vram_total", 0);
    if (vramTotal > 0) {
        metrics.memoryUsedBytes = vramUsed;
        metrics.memoryTotalBytes = vramTotal;
    }

    return true;
}

bool LinuxSysfsGpuCollector::CollectIntelSysfs(const LinuxGpuCardInfo& card, GpuMetrics& metrics) {
    std::string cardRoot = drmBasePath_ + "/" + card.cardName;

    // Read frequency
    uint64_t actFreq = ReadSysfsUint64(cardRoot + "/gt_act_freq_mhz", 0);
    uint64_t maxFreq = ReadSysfsUint64(cardRoot + "/gt_max_freq_mhz", 0);
    if (maxFreq > 0 && actFreq > 0) {
        double util = (static_cast<double>(actFreq) / static_cast<double>(maxFreq)) * 100.0;
        metrics.utilizationPercent = std::clamp(util, 0.0, 100.0);
    }

    // Read lmem for Arc dedicated GPUs if available
    uint64_t lmem = ReadSysfsUint64(card.devicePath + "/lmem_total_bytes", 0);
    if (lmem > 0) {
        metrics.memoryTotalBytes = lmem;
    }

    return true;
}

bool LinuxSysfsGpuCollector::CollectGenericDrmSysfs(const LinuxGpuCardInfo& /*card*/, GpuMetrics& /*metrics*/) {
    // Base DRM presence confirmed
    return true;
}

bool LinuxSysfsGpuCollector::Collect(const LinuxGpuCardInfo& card, GpuMetrics& metrics) {
    metrics.isAvailable = true;
    metrics.name = card.name;

    if (card.totalVramBytes > 0) {
        metrics.memoryTotalBytes = card.totalVramBytes;
    }

    if (card.isAmd) {
        CollectAmdSysfs(card, metrics);
    } else if (card.isIntel) {
        CollectIntelSysfs(card, metrics);
    } else {
        CollectGenericDrmSysfs(card, metrics);
    }

    CollectHwmonSensors(card.hwmonPath, metrics);
    return true;
}

bool LinuxSysfsGpuCollector::Collect(uint32_t cardIndex, GpuMetrics& metrics) {
    auto cards = EnumerateCards();
    for (const auto& card : cards) {
        if (card.index == cardIndex) {
            return Collect(card, metrics);
        }
    }
    return false;
}

} // namespace BareStats
