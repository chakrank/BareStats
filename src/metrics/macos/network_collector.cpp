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
    bool initialized = false;
    uint64_t prevInBytes = 0;
    uint64_t prevOutBytes = 0;
    std::chrono::steady_clock::time_point prevTime;
    std::string primaryInterfaceName;

    Impl() {
        prevTime = std::chrono::steady_clock::now();
    }

    static bool IsVirtualOrFilteredInterface(const char* name) {
        if (!name) return true;
        static const char* const prefixes[] = {
            "lo", "utun", "gif", "stf", "bridge", "llw", "awdl", "ipsec", "anpi", "pktap", "p2p"
        };
        for (const auto* pfx : prefixes) {
            size_t pfxLen = std::strlen(pfx);
            if (std::strncmp(name, pfx, pfxLen) == 0) {
                return true;
            }
        }
        return false;
    }

    static std::string FindDefaultGatewayInterface() {
        int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
        size_t needed = 0;
        if (sysctl(mib, 6, nullptr, &needed, nullptr, 0) < 0 || needed == 0) {
            return "";
        }

        std::vector<char> buf(needed);
        if (sysctl(mib, 6, buf.data(), &needed, nullptr, 0) < 0) {
            return "";
        }

        char* next = buf.data();
        char* end = buf.data() + needed;

        while (next < end) {
            struct rt_msghdr* rtm = reinterpret_cast<struct rt_msghdr*>(next);
            if (rtm->rtm_msglen == 0) break;
            next += rtm->rtm_msglen;

            if (rtm->rtm_flags & RTF_GATEWAY) {
                struct sockaddr* sa = reinterpret_cast<struct sockaddr*>(rtm + 1);
                if (rtm->rtm_addrs & RTA_DST) {
                    if (sa->sa_family == AF_INET) {
                        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(sa);
                        if (sin->sin_addr.s_addr == 0) { // Default route 0.0.0.0
                            char ifName[IF_NAMESIZE] = { 0 };
                            if (if_indextoname(rtm->rtm_index, ifName)) {
                                return std::string(ifName);
                            }
                        }
                    }
                }
            }
        }

        return "";
    }

    NetworkMetrics Collect() {
        NetworkMetrics metrics{};

        struct ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) != 0 || !ifap) {
            return metrics;
        }

        std::string defaultIf = FindDefaultGatewayInterface();

        uint64_t totalIn = 0;
        uint64_t totalOut = 0;
        bool foundTarget = false;
        std::string chosenIf;

        // First pass: match the default gateway interface if available
        if (!defaultIf.empty()) {
            for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
                if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

                if (defaultIf == ifa->ifa_name && ifa->ifa_data) {
                    struct if_data* ifd = reinterpret_cast<struct if_data*>(ifa->ifa_data);
                    totalIn = ifd->ifi_ibytes;
                    totalOut = ifd->ifi_obytes;
                    chosenIf = ifa->ifa_name;
                    foundTarget = true;
                    break;
                }
            }
        }

        // Second pass: fallback to primary physical adapter (e.g. en0, en1) with active traffic
        if (!foundTarget) {
            uint64_t bestTraffic = 0;
            for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
                if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
                if (IsVirtualOrFilteredInterface(ifa->ifa_name)) continue;

                if (ifa->ifa_data) {
                    struct if_data* ifd = reinterpret_cast<struct if_data*>(ifa->ifa_data);
                    uint64_t traffic = ifd->ifi_ibytes + ifd->ifi_obytes;
                    if (traffic > bestTraffic || chosenIf.empty()) {
                        bestTraffic = traffic;
                        totalIn = ifd->ifi_ibytes;
                        totalOut = ifd->ifi_obytes;
                        chosenIf = ifa->ifa_name;
                        foundTarget = true;
                    }
                }
            }
        }

        freeifaddrs(ifap);

        if (!foundTarget) {
            return metrics;
        }

        auto now = std::chrono::steady_clock::now();
        metrics.isAvailable = true;
        metrics.primaryAdapterName = chosenIf;
        metrics.primaryAdapter = Utf8ToWideString(chosenIf);
        metrics.totalRxBytes = totalIn;
        metrics.totalTxBytes = totalOut;

        if (!initialized) {
            prevInBytes = totalIn;
            prevOutBytes = totalOut;
            prevTime = now;
            initialized = true;
            return metrics;
        }

        double elapsedSec = std::chrono::duration<double>(now - prevTime).count();
        if (elapsedSec > 0.05) {
            uint64_t diffIn = (totalIn >= prevInBytes) ? (totalIn - prevInBytes) : 0;
            uint64_t diffOut = (totalOut >= prevOutBytes) ? (totalOut - prevOutBytes) : 0;

            metrics.rxBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(diffIn) / elapsedSec));
            metrics.txBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(diffOut) / elapsedSec));

            prevInBytes = totalIn;
            prevOutBytes = totalOut;
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
