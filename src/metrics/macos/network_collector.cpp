#include "../network_collector.h"

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

namespace BareStats {

static std::wstring Utf8ToWideString(const std::string& str) {
    std::wstring out;
    size_t i = 0;
    while (i < str.size()) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80) {
            out.push_back(static_cast<wchar_t>(c));
            i++;
        } else if ((c >> 5) == 0x6 && i + 1 < str.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < str.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < str.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            i++;
        }
    }
    return out;
}

struct NetworkCollector::Impl {
    struct InterfaceHistory {
        uint64_t prevIn = 0;
        uint64_t prevOut = 0;
        bool hasSample = false;
    };

    bool initialized = false;
    std::chrono::steady_clock::time_point prevTime;
    std::unordered_map<std::string, InterfaceHistory> ifaceHistory;

    Impl() {
        prevTime = std::chrono::steady_clock::now();
    }

    static bool IsVirtualOrFilteredInterface(const char* name) {
        if (!name) return true;
        static const char* const prefixes[] = {
            "lo", "gif", "stf", "bridge", "llw", "awdl", "pktap"
        };
        for (const auto* pfx : prefixes) {
            size_t pfxLen = std::strlen(pfx);
            if (std::strncmp(name, pfx, pfxLen) == 0) {
                return true;
            }
        }
        return false;
    }

    NetworkMetrics Collect() {
        NetworkMetrics metrics{};

        struct ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) != 0 || !ifap) {
            return metrics;
        }

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - prevTime).count();

        uint64_t totalActiveIn = 0;
        uint64_t totalActiveOut = 0;
        uint64_t totalRxAccum = 0;
        uint64_t totalTxAccum = 0;
        uint64_t maxDelta = 0;
        std::string activeIf;

        for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            if (IsVirtualOrFilteredInterface(ifa->ifa_name)) continue;

            if (ifa->ifa_data) {
                struct if_data* ifd = reinterpret_cast<struct if_data*>(ifa->ifa_data);
                std::string ifName = ifa->ifa_name;
                uint64_t curIn = ifd->ifi_ibytes;
                uint64_t curOut = ifd->ifi_obytes;

                totalRxAccum += curIn;
                totalTxAccum += curOut;

                auto& hist = ifaceHistory[ifName];
                if (hist.hasSample && elapsedSec > 0.05) {
                    uint64_t dIn = (curIn >= hist.prevIn) ? (curIn - hist.prevIn) : 0;
                    uint64_t dOut = (curOut >= hist.prevOut) ? (curOut - hist.prevOut) : 0;

                    totalActiveIn += dIn;
                    totalActiveOut += dOut;

                    if ((dIn + dOut) > maxDelta || activeIf.empty()) {
                        maxDelta = dIn + dOut;
                        activeIf = ifName;
                    }
                } else if (activeIf.empty()) {
                    activeIf = ifName;
                }

                hist.prevIn = curIn;
                hist.prevOut = curOut;
                hist.hasSample = true;
            }
        }

        freeifaddrs(ifap);

        if (activeIf.empty()) {
            activeIf = "en0";
        }

        metrics.isAvailable = true;
        metrics.primaryAdapterName = activeIf;
        metrics.primaryAdapter = Utf8ToWideString(activeIf);
        metrics.totalRxBytes = totalRxAccum;
        metrics.totalTxBytes = totalTxAccum;

        if (!initialized) {
            prevTime = now;
            initialized = true;
            return metrics;
        }

        if (elapsedSec > 0.05) {
            metrics.rxBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(totalActiveIn) / elapsedSec));
            metrics.txBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(totalActiveOut) / elapsedSec));
            prevTime = now;
        }

        return metrics;
    }
};

NetworkCollector::NetworkCollector()
    : impl_(std::make_unique<Impl>()) {
}

NetworkCollector::~NetworkCollector() = default;

NetworkMetrics NetworkCollector::Collect() {
    return impl_->Collect();
}

} // namespace BareStats

#endif // __APPLE__
