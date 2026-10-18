#include "../network_collector.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace BareStats {

static std::wstring ToLower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return str;
}

struct NetworkCollector::Impl {
    bool initialized = false;
    uint64_t prevInOctets = 0;
    uint64_t prevOutOctets = 0;
    std::chrono::steady_clock::time_point prevTime;
    std::wstring primaryAdapterName;

    Impl() {
        prevTime = std::chrono::steady_clock::now();
    }

    bool IsValidPhysicalAdapter(const MIB_IF_ROW2& row) const {
        if (row.OperStatus != IfOperStatusUp) {
            return false;
        }

        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) {
            return false;
        }

        if (row.InterfaceAndOperStatusFlags.FilterInterface) {
            return false;
        }

        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.Type == IF_TYPE_TUNNEL ||
            row.Type == IF_TYPE_PPP ||
            row.Type == IF_TYPE_SLIP) {
            return false;
        }

        std::wstring desc = ToLower(row.Description);
        std::wstring alias = ToLower(row.Alias);

        static const wchar_t* const blacklist[] = {
            L"virtual", L"vpn", L"hyper-v", L"vethernet", L"vmware",
            L"virtualbox", L"loopback", L"teredo", L"isatap", L"bluetooth",
            L"tap-", L"wintun", L"tailscale", L"wireguard", L"npcap",
            L"pcap", L"host-only", L"pseudo", L"bridge", L"docker"
        };

        for (const auto* term : blacklist) {
            if (desc.find(term) != std::wstring::npos || alias.find(term) != std::wstring::npos) {
                return false;
            }
        }

        return true;
    }

    NET_IFINDEX GetBestGatewayInterfaceIndex() const {
        PMIB_IPFORWARD_TABLE2 pRouteTable = nullptr;
        NET_IFINDEX bestIndex = 0;
        ULONG lowestMetric = ULONG_MAX;

        if (GetIpForwardTable2(AF_UNSPEC, &pRouteTable) == NO_ERROR && pRouteTable != nullptr) {
            for (ULONG i = 0; i < pRouteTable->NumEntries; ++i) {
                const auto& row = pRouteTable->Table[i];

                bool isDefaultRoute = false;
                if (row.DestinationPrefix.Prefix.si_family == AF_INET) {
                    if (row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr == 0 && row.DestinationPrefix.PrefixLength == 0) {
                        isDefaultRoute = true;
                    }
                } else if (row.DestinationPrefix.Prefix.si_family == AF_INET6) {
                    bool allZero = true;
                    for (int b = 0; b < 16; ++b) {
                        if (row.DestinationPrefix.Prefix.Ipv6.sin6_addr.s6_addr[b] != 0) {
                            allZero = false;
                            break;
                        }
                    }
                    if (allZero && row.DestinationPrefix.PrefixLength == 0) {
                        isDefaultRoute = true;
                    }
                }

                if (isDefaultRoute) {
                    if (row.Metric < lowestMetric) {
                        lowestMetric = row.Metric;
                        bestIndex = row.InterfaceIndex;
                    }
                }
            }
            FreeMibTable(pRouteTable);
        }
        return bestIndex;
    }

    NetworkMetrics Collect() {
        NetworkMetrics metrics{};

        PMIB_IF_TABLE2 pTable = nullptr;
        if (GetIfTable2(&pTable) != NO_ERROR || pTable == nullptr) {
            return metrics;
        }

        NET_IFINDEX bestIndex = GetBestGatewayInterfaceIndex();

        uint64_t totalIn = 0;
        uint64_t totalOut = 0;
        bool foundPhysical = false;
        std::wstring primaryName;

        for (ULONG i = 0; i < pTable->NumEntries; ++i) {
            const auto& row = pTable->Table[i];
            if (IsValidPhysicalAdapter(row)) {
                if (bestIndex != 0) {
                    if (row.InterfaceIndex == bestIndex) {
                        totalIn = row.InOctets;
                        totalOut = row.OutOctets;
                        primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
                        foundPhysical = true;
                        break;
                    }
                } else {
                    if (row.InOctets > 0 || row.OutOctets > 0) {
                        totalIn = row.InOctets;
                        totalOut = row.OutOctets;
                        primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
                        foundPhysical = true;
                        break;
                    }
                }
            }
        }

        if (!foundPhysical) {
            for (ULONG i = 0; i < pTable->NumEntries; ++i) {
                const auto& row = pTable->Table[i];
                if (IsValidPhysicalAdapter(row)) {
                    totalIn = row.InOctets;
                    totalOut = row.OutOctets;
                    primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
                    foundPhysical = true;
                    break;
                }
            }
        }

        if (!foundPhysical && bestIndex != 0) {
            for (ULONG i = 0; i < pTable->NumEntries; ++i) {
                const auto& row = pTable->Table[i];
                if (row.InterfaceIndex == bestIndex && row.OperStatus == IfOperStatusUp) {
                    totalIn = row.InOctets;
                    totalOut = row.OutOctets;
                    primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
                    foundPhysical = true;
                    break;
                }
            }
        }

        if (!foundPhysical) {
            for (ULONG i = 0; i < pTable->NumEntries; ++i) {
                const auto& row = pTable->Table[i];
                if (row.OperStatus == IfOperStatusUp && row.Type != IF_TYPE_SOFTWARE_LOOPBACK) {
                    if (row.InOctets > 0 || row.OutOctets > 0) {
                        totalIn = row.InOctets;
                        totalOut = row.OutOctets;
                        foundPhysical = true;
                        primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
                        if (row.InterfaceIndex == bestIndex) {
                            break;
                        }
                    }
                }
            }
        }

        FreeMibTable(pTable);

        if (!foundPhysical) {
            return metrics;
        }

        auto now = std::chrono::steady_clock::now();
        metrics.isAvailable = true;
        metrics.primaryAdapter = primaryName;
        int u8len = WideCharToMultiByte(CP_UTF8, 0, primaryName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (u8len > 1) {
            std::string u8(u8len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, primaryName.c_str(), -1, &u8[0], u8len, nullptr, nullptr);
            metrics.primaryAdapterName = u8;
        }
        metrics.totalRxBytes = totalIn;
        metrics.totalTxBytes = totalOut;

        if (!initialized) {
            prevInOctets = totalIn;
            prevOutOctets = totalOut;
            prevTime = now;
            initialized = true;
            return metrics;
        }

        double elapsedSec = std::chrono::duration<double>(now - prevTime).count();
        if (elapsedSec > 0.05) {
            uint64_t diffIn = (totalIn >= prevInOctets) ? (totalIn - prevInOctets) : 0;
            uint64_t diffOut = (totalOut >= prevOutOctets) ? (totalOut - prevOutOctets) : 0;

            metrics.rxBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(diffIn) / elapsedSec));
            metrics.txBytesPerSec = static_cast<uint64_t>(std::round(static_cast<double>(diffOut) / elapsedSec));

            prevInOctets = totalIn;
            prevOutOctets = totalOut;
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

#endif // _WIN32
