#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>

#include "network_collector.h"
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
        // 1. Must be currently operational (link up)
        if (row.OperStatus != IfOperStatusUp) {
            return false;
        }

        // 2. Must be flagged as a physical hardware interface by Windows NDIS
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) {
            return false;
        }

        // 3. Must not be a packet filter interface
        if (row.InterfaceAndOperStatusFlags.FilterInterface) {
            return false;
        }

        // 4. Interface type check: Must be standard Ethernet, Wi-Fi, or mobile broadband
        // Disallow loopback, tunnels, software interfaces, slip, ppp
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.Type == IF_TYPE_TUNNEL ||
            row.Type == IF_TYPE_PPP ||
            row.Type == IF_TYPE_SLIP) {
            return false;
        }

        // 5. Deep inspection heuristic: Filter out virtual adapters mimicking hardware NICs
        std::wstring alias = ToLower(row.Alias);
        std::wstring desc = ToLower(row.Description);

        static const wchar_t* const kVirtualKeywords[] = {
            L"hyper-v",
            L"virtual",
            L"wsl",
            L"loopback",
            L"vmware",
            L"virtualbox",
            L"tap-",
            L"tun",
            L"tailscale",
            L"wireguard",
            L"docker",
            L"vethernet",
            L"pseudo",
            L"bluetooth",
            L"npcap",
            L"pcap",
            L"fortinet",
            L"anyconnect",
            L"globalprotect",
            L"zerotier"
        };

        for (const auto* kw : kVirtualKeywords) {
            if (alias.find(kw) != std::wstring::npos || desc.find(kw) != std::wstring::npos) {
                return false;
            }
        }

        return true;
    }

    NetworkMetrics Collect() {
        NetworkMetrics metrics;

        PMIB_IF_TABLE2 pTable = nullptr;
        if (GetIfTable2(&pTable) != NO_ERROR || !pTable) {
            return metrics;
        }

        uint64_t totalIn = 0;
        uint64_t totalOut = 0;
        std::wstring primaryName;
        bool foundPhysical = false;

        // Discover the best default route interface (if internet gateway is active)
        NET_IFINDEX bestIndex = 0;
        SOCKADDR_INET destAddr{};
        destAddr.Ipv4.sin_family = AF_INET;
        destAddr.Ipv4.sin_addr.s_addr = 0x08080808; // 8.8.8.8 in network byte order

        MIB_IPFORWARD_ROW2 bestRoute{};
        SOCKADDR_INET bestSource{};
        if (GetBestRoute2(nullptr, 0, nullptr, &destAddr, 0, &bestRoute, &bestSource) == NO_ERROR) {
            bestIndex = bestRoute.InterfaceIndex;
        }

        for (ULONG i = 0; i < pTable->NumEntries; ++i) {
            const MIB_IF_ROW2& row = pTable->Table[i];
            if (IsValidPhysicalAdapter(row)) {
                foundPhysical = true;
                totalIn += row.InOctets;
                totalOut += row.OutOctets;

                // Prioritize best gateway adapter name for display
                if (row.InterfaceIndex == bestIndex || primaryName.empty()) {
                    primaryName = (row.Alias[0] != L'\0') ? row.Alias : row.Description;
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
