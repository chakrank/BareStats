#include "linux_proc_collector.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <climits>

namespace BareStats {

static std::string TrimStr(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

static std::wstring StrToWstr(const std::string& str) {
    std::wstring out;
    out.reserve(str.size());
    for (char c : str) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

// -----------------------------------------------------------------------------
// LinuxProcCpuCollector
// -----------------------------------------------------------------------------

LinuxProcCpuCollector::LinuxProcCpuCollector(std::string procStatPath)
    : procStatPath_(std::move(procStatPath)) {
}

void LinuxProcCpuCollector::Reset() {
    initialized_ = false;
    prevIdleAll_ = 0;
    prevTotal_ = 0;
}

CpuMetrics LinuxProcCpuCollector::Collect() {
    CpuMetrics metrics;

    std::ifstream file(procStatPath_);
    if (!file.is_open()) {
        return metrics;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 3, "cpu") == 0 && (line.size() == 3 || line[3] == ' ')) {
            std::istringstream iss(line);
            std::string cpuLabel;
            uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
            uint64_t irq = 0, softirq = 0, steal = 0, guest = 0, guestNice = 0;

            iss >> cpuLabel >> user >> nice >> system >> idle >> iowait
                >> irq >> softirq >> steal >> guest >> guestNice;

            uint64_t idleAll = idle + iowait;
            uint64_t systemAll = system + irq + softirq;
            uint64_t virtAll = guest + guestNice;
            uint64_t total = user + nice + systemAll + idleAll + steal + virtAll;

            if (!initialized_) {
                prevIdleAll_ = idleAll;
                prevTotal_ = total;
                initialized_ = true;
                return metrics;
            }

            if (total >= prevTotal_ && idleAll >= prevIdleAll_) {
                uint64_t totalDelta = total - prevTotal_;
                uint64_t idleDelta = idleAll - prevIdleAll_;

                if (totalDelta > 0 && totalDelta >= idleDelta) {
                    double activeDelta = static_cast<double>(totalDelta - idleDelta);
                    double util = (activeDelta / static_cast<double>(totalDelta)) * 100.0;
                    metrics.utilizationPercent = std::clamp(util, 0.0, 100.0);
                }
            }

            prevIdleAll_ = idleAll;
            prevTotal_ = total;
            break;
        }
    }

    return metrics;
}

// -----------------------------------------------------------------------------
// LinuxProcMemoryCollector
// -----------------------------------------------------------------------------

LinuxProcMemoryCollector::LinuxProcMemoryCollector(std::string procMeminfoPath)
    : procMeminfoPath_(std::move(procMeminfoPath)) {
}

MemoryMetrics LinuxProcMemoryCollector::Collect() {
    MemoryMetrics metrics;

    std::ifstream file(procMeminfoPath_);
    if (!file.is_open()) {
        return metrics;
    }

    uint64_t memTotalKb = 0;
    uint64_t memAvailKb = 0;
    uint64_t memFreeKb = 0;
    uint64_t buffersKb = 0;
    uint64_t cachedKb = 0;
    bool hasAvail = false;

    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 9, "MemTotal:") == 0) {
            std::istringstream iss(line.substr(9));
            iss >> memTotalKb;
        } else if (line.compare(0, 13, "MemAvailable:") == 0) {
            std::istringstream iss(line.substr(13));
            iss >> memAvailKb;
            hasAvail = true;
        } else if (line.compare(0, 8, "MemFree:") == 0) {
            std::istringstream iss(line.substr(8));
            iss >> memFreeKb;
        } else if (line.compare(0, 8, "Buffers:") == 0) {
            std::istringstream iss(line.substr(8));
            iss >> buffersKb;
        } else if (line.compare(0, 7, "Cached:") == 0) {
            std::istringstream iss(line.substr(7));
            iss >> cachedKb;
        }
    }

    metrics.totalBytes = memTotalKb * 1024ULL;

    if (hasAvail) {
        metrics.usedBytes = (memTotalKb > memAvailKb) ? (memTotalKb - memAvailKb) * 1024ULL : 0;
    } else {
        uint64_t freeTotal = memFreeKb + buffersKb + cachedKb;
        metrics.usedBytes = (memTotalKb > freeTotal) ? (memTotalKb - freeTotal) * 1024ULL : 0;
    }

    return metrics;
}

// -----------------------------------------------------------------------------
// LinuxProcNetworkCollector
// -----------------------------------------------------------------------------

LinuxProcNetworkCollector::LinuxProcNetworkCollector(std::string procNetDevPath,
                                                     std::string procNetRoutePath)
    : procNetDevPath_(std::move(procNetDevPath)),
      procNetRoutePath_(std::move(procNetRoutePath)) {
}

void LinuxProcNetworkCollector::Reset() {
    initialized_ = false;
    activeInterface_.clear();
    prevRxBytes_ = 0;
    prevTxBytes_ = 0;
    prevTimePoint_ = {};
}

bool LinuxProcNetworkCollector::IsVirtualOrIgnoredInterface(const std::string& iface) {
    if (iface.empty() || iface == "lo") return true;

    static const char* const ignoredPrefixes[] = {
        "docker", "veth", "br-", "virbr", "tun", "tap",
        "wg", "dummy", "bond_slave", "kube", "flannel",
        "cni", "tailscale", "vmnet"
    };

    for (const char* prefix : ignoredPrefixes) {
        size_t len = std::string(prefix).size();
        if (iface.compare(0, len, prefix) == 0) {
            return true;
        }
    }

    return false;
}

std::string LinuxProcNetworkCollector::DetectDefaultGatewayInterface(const std::string& routePath) {
    std::ifstream file(routePath);
    if (!file.is_open()) {
        return "";
    }

    std::string line;
    // Skip header line
    if (!std::getline(file, line)) {
        return "";
    }

    std::string bestIface;
    uint32_t lowestMetric = UINT32_MAX;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string iface, destination, gateway;
        uint32_t flags = 0, refCnt = 0, use = 0, metric = 0;

        if (iss >> iface >> destination >> gateway >> std::hex >> flags >> std::dec >> refCnt >> use >> metric) {
            // Destination 00000000 and RTF_GATEWAY (0x0002)
            if (destination == "00000000" && (flags & 0x0002) != 0) {
                if (!IsVirtualOrIgnoredInterface(iface) && metric < lowestMetric) {
                    lowestMetric = metric;
                    bestIface = iface;
                }
            }
        }
    }

    return bestIface;
}

NetworkMetrics LinuxProcNetworkCollector::Collect() {
    NetworkMetrics metrics;

    if (activeInterface_.empty()) {
        activeInterface_ = DetectDefaultGatewayInterface(procNetRoutePath_);
    }

    std::ifstream file(procNetDevPath_);
    if (!file.is_open()) {
        return metrics;
    }

    std::string line;
    // Skip two header lines
    std::getline(file, line);
    std::getline(file, line);

    struct IfaceRecord {
        std::string name;
        uint64_t rxBytes = 0;
        uint64_t txBytes = 0;
    };

    std::vector<IfaceRecord> records;
    while (std::getline(file, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string iface = TrimStr(line.substr(0, colon));
        std::istringstream iss(line.substr(colon + 1));

        uint64_t rxBytes = 0, rxPackets = 0, rxErrs = 0, rxDrop = 0, rxFifo = 0, rxFrame = 0, rxComp = 0, rxMulti = 0;
        uint64_t txBytes = 0;

        if (iss >> rxBytes >> rxPackets >> rxErrs >> rxDrop >> rxFifo >> rxFrame >> rxComp >> rxMulti >> txBytes) {
            records.push_back({ iface, rxBytes, txBytes });
        }
    }

    if (records.empty()) {
        return metrics;
    }

    // Match primary adapter:
    // 1. Exact match with activeInterface_
    // 2. First non-virtual adapter with non-zero traffic
    // 3. First non-virtual adapter
    // 4. First adapter in list
    const IfaceRecord* matched = nullptr;

    if (!activeInterface_.empty()) {
        for (const auto& r : records) {
            if (r.name == activeInterface_) {
                matched = &r;
                break;
            }
        }
    }

    if (!matched) {
        for (const auto& r : records) {
            if (!IsVirtualOrIgnoredInterface(r.name) && (r.rxBytes > 0 || r.txBytes > 0)) {
                matched = &r;
                activeInterface_ = r.name;
                break;
            }
        }
    }

    if (!matched) {
        for (const auto& r : records) {
            if (!IsVirtualOrIgnoredInterface(r.name)) {
                matched = &r;
                activeInterface_ = r.name;
                break;
            }
        }
    }

    if (!matched) {
        matched = &records.front();
        activeInterface_ = matched->name;
    }

    auto now = std::chrono::steady_clock::now();

    metrics.isAvailable = true;
    metrics.primaryAdapterName = matched->name;
    metrics.primaryAdapter = StrToWstr(matched->name);
    metrics.totalRxBytes = matched->rxBytes;
    metrics.totalTxBytes = matched->txBytes;

    if (!initialized_) {
        prevRxBytes_ = matched->rxBytes;
        prevTxBytes_ = matched->txBytes;
        prevTimePoint_ = now;
        initialized_ = true;
        return metrics;
    }

    std::chrono::duration<double> elapsedSec = now - prevTimePoint_;
    double dt = elapsedSec.count();

    if (dt > 0.0001) {
        if (matched->rxBytes >= prevRxBytes_) {
            metrics.rxBytesPerSec = static_cast<uint64_t>((matched->rxBytes - prevRxBytes_) / dt);
        }
        if (matched->txBytes >= prevTxBytes_) {
            metrics.txBytesPerSec = static_cast<uint64_t>((matched->txBytes - prevTxBytes_) / dt);
        }
    }

    prevRxBytes_ = matched->rxBytes;
    prevTxBytes_ = matched->txBytes;
    prevTimePoint_ = now;

    return metrics;
}

} // namespace BareStats
