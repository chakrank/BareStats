#include "udp_streamer.h"
#include "../llm/llm_resolver.h"
#include "util/crypto/crypto_box.h"
#include "util/http_client.h"
#include "util/logger.h"
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
typedef SOCKET SockType;
typedef int SockLen;
constexpr SockType InvalidSock = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
typedef int SockType;
typedef socklen_t SockLen;
constexpr SockType InvalidSock = -1;
#endif

namespace BareStats {

static bool EnsureSocketsInit() {
#if defined(_WIN32)
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        initialized = true;
    }
#endif
    return true;
}

static void CloseSocket(SockType s) {
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

// --- UdpTransmitter ---

UdpTransmitter::UdpTransmitter() {
    EnsureSocketsInit();
    // Unique session ID per executable launch for reboot resilience
    uint8_t rand_bytes[4] = {0};
    Crypto::GenerateRandomBytes(rand_bytes, sizeof(rand_bytes));
    std::memcpy(&session_id_, rand_bytes, sizeof(session_id_));
    if (session_id_ == 0) session_id_ = 1;
}

UdpTransmitter::~UdpTransmitter() {
    Close();
}

bool UdpTransmitter::Open(
    const std::string& target_host,
    uint16_t port,
    const uint8_t key[32],
    const std::string& rig_id
) {
    SatelliteTargetConfig tc;
    tc.name = "default";
    tc.target_host = target_host;
    tc.target_port = port;
    if (key) {
        std::memcpy(tc.public_key, key, 32);
    }
    std::vector<SatelliteTargetConfig> targets = { tc };
    return Open(rig_id, targets);
}

bool UdpTransmitter::Open(
    const std::string& rig_id,
    const std::vector<SatelliteTargetConfig>& targets
) {
    Close();

    std::memset(rig_id_, 0, sizeof(rig_id_));
    std::strncpy(rig_id_, rig_id.c_str(), sizeof(rig_id_) - 1);

    for (const auto& t : targets) {
        SatelliteTargetState state{};
        state.name = t.name;
        state.target_host = t.target_host;
        state.target_port = t.target_port;
        std::memcpy(state.public_key, t.public_key, 32);
        state.is_auto_discovery = (t.target_host == "auto" || t.target_host == "255.255.255.255");
        state.discovered_host.clear();
        state.consecutive_unacked = 0;
        state.pending_transaction_id = 0;
        targets_.push_back(state);
    }

    SockType s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == InvalidSock) return false;

#if !defined(_WIN32)
    if (s < 0 || s >= FD_SETSIZE) {
        CloseSocket(s);
        return false;
    }
#endif

    // Enable broadcast for subnet auto-discovery if needed
    int broadcast = 1;
#if defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
#else
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
#endif

    socket_ = static_cast<uintptr_t>(s);

    if (IsAutoDiscovery()) {
        SendDiscover();
    }

    return true;
}

void UdpTransmitter::Close() {
    if (socket_ != ~0ULL) {
        CloseSocket(static_cast<SockType>(socket_));
        socket_ = ~0ULL;
    }
    for (auto& t : targets_) {
        Crypto::SecureWipe(t.public_key, sizeof(t.public_key));
        Crypto::SecureWipe(t.pending_eph_sk, sizeof(t.pending_eph_sk));
    }
    targets_.clear();
    cached_bcast_addrs_.clear();
    last_bcast_refresh_ = std::chrono::steady_clock::time_point{};
}

std::vector<uint32_t> UdpTransmitter::GetSubnetBroadcastTargets() {
    auto now = std::chrono::steady_clock::now();
    if (!cached_bcast_addrs_.empty() && (now - last_bcast_refresh_) < std::chrono::seconds(10)) {
        return cached_bcast_addrs_;
    }

    std::vector<uint32_t> bcasts;

#if defined(_WIN32)
    ULONG outBufLen = 15000;
    std::vector<uint8_t> buffer(outBufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(outBufLen);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    }

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
                if (!pUnicast->Address.lpSockaddr || pUnicast->Address.lpSockaddr->sa_family != AF_INET) continue;

                auto* sa = reinterpret_cast<sockaddr_in*>(pUnicast->Address.lpSockaddr);
                uint32_t ip = sa->sin_addr.s_addr; // network byte order
                uint8_t prefixLen = pUnicast->OnLinkPrefixLength;
                if (prefixLen > 0 && prefixLen <= 32) {
                    uint32_t maskHost = (prefixLen == 32) ? 0xFFFFFFFFU : ~((1U << (32 - prefixLen)) - 1);
                    uint32_t maskNet = htonl(maskHost);
                    uint32_t bcast = ip | (~maskNet);
                    if (bcast != INADDR_BROADCAST && bcast != 0) {
                        bcasts.push_back(bcast);
                    }
                }
            }
        }
    }
#elif !defined(_WIN32)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            if (ifa->ifa_flags & IFF_BROADCAST) {
                if (ifa->ifa_broadaddr && ifa->ifa_broadaddr->sa_family == AF_INET) {
                    auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
                    if (sa->sin_addr.s_addr != INADDR_BROADCAST && sa->sin_addr.s_addr != 0) {
                        bcasts.push_back(sa->sin_addr.s_addr);
                    }
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif

    std::sort(bcasts.begin(), bcasts.end());
    bcasts.erase(std::unique(bcasts.begin(), bcasts.end()), bcasts.end());

    // Always include loopback and universal broadcast as fallbacks
    bcasts.push_back(htonl(INADDR_LOOPBACK));
    bcasts.push_back(INADDR_BROADCAST);

    cached_bcast_addrs_ = bcasts;
    last_bcast_refresh_ = now;
    return cached_bcast_addrs_;
}

std::vector<uint32_t> UdpTransmitter::GetSubnetProbeTargets() {
    std::vector<uint32_t> probeAddrs;

#if defined(_WIN32)
    ULONG outBufLen = 15000;
    std::vector<uint8_t> buffer(outBufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(outBufLen);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    }

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
                if (!pUnicast->Address.lpSockaddr || pUnicast->Address.lpSockaddr->sa_family != AF_INET) continue;

                auto* sa = reinterpret_cast<sockaddr_in*>(pUnicast->Address.lpSockaddr);
                uint32_t ipNet = sa->sin_addr.s_addr;
                uint32_t ipHost = ntohl(ipNet);
                uint8_t prefixLen = pUnicast->OnLinkPrefixLength;

                // For standard /24 networks (or /24 slice of /16), probe the 254 unicast hosts
                if (prefixLen >= 16 && prefixLen <= 30) {
                    uint32_t baseHost = ipHost & 0xFFFFFF00;
                    for (uint32_t h = 1; h <= 254; ++h) {
                        uint32_t candidate = baseHost | h;
                        if (candidate != ipHost) {
                            probeAddrs.push_back(htonl(candidate));
                        }
                    }
                }
            }
        }
    }
#elif !defined(_WIN32)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            uint32_t ipHost = ntohl(sa->sin_addr.s_addr);
            uint32_t baseHost = ipHost & 0xFFFFFF00;
            for (uint32_t h = 1; h <= 254; ++h) {
                uint32_t candidate = baseHost | h;
                if (candidate != ipHost) {
                    probeAddrs.push_back(htonl(candidate));
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif

    std::sort(probeAddrs.begin(), probeAddrs.end());
    probeAddrs.erase(std::unique(probeAddrs.begin(), probeAddrs.end()), probeAddrs.end());
    return probeAddrs;
}

bool UdpTransmitter::SendDiscoverForTarget(SatelliteTargetState& target) {
    if (socket_ == ~0ULL) return false;
    SockType s = static_cast<SockType>(socket_);

    target.last_discover_time = std::chrono::steady_clock::now();
    target.discover_attempts++;

    // Generate random 64-bit transaction nonce to bind request and response
    Crypto::GenerateRandomBytes(reinterpret_cast<uint8_t*>(&target.pending_transaction_id), sizeof(target.pending_transaction_id));
    if (target.pending_transaction_id == 0) target.pending_transaction_id = 1;

    Wire::DiscoverPacket pkt{};
    if (!Crypto::PackDiscover(target.public_key, rig_id_, target.pending_transaction_id, pkt, target.pending_eph_sk)) {
        return false;
    }

    if (!target.is_auto_discovery && !target.target_host.empty() && target.target_host != "auto" && target.target_host != "255.255.255.255") {
        sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(target.target_port);
        if (HttpClient::ResolveHost(target.target_host, target.target_port, dest_addr)) {
            int sent = sendto(
                s,
                reinterpret_cast<const char*>(&pkt),
                sizeof(pkt),
                0,
                reinterpret_cast<struct sockaddr*>(&dest_addr),
                sizeof(dest_addr)
            );
            if (sent == static_cast<int>(sizeof(pkt))) {
                return true;
            }
        }
        return false;
    }

    bool any_sent = false;
    auto bcastTargets = GetSubnetBroadcastTargets();
    for (uint32_t bcast : bcastTargets) {
        sockaddr_in bcast_addr{};
        bcast_addr.sin_family = AF_INET;
        bcast_addr.sin_port = htons(target.target_port);
        bcast_addr.sin_addr.s_addr = bcast;

        int sent = sendto(
            s,
            reinterpret_cast<const char*>(&pkt),
            sizeof(pkt),
            0,
            reinterpret_cast<struct sockaddr*>(&bcast_addr),
            sizeof(bcast_addr)
        );
        if (sent == static_cast<int>(sizeof(pkt))) {
            any_sent = true;
        }
    }

    // Direct Unicast Subnet Probe Sweep (bypasses Wi-Fi router AP isolation & broadcast drops)
    auto probeTargets = GetSubnetProbeTargets();
    for (uint32_t hostAddr : probeTargets) {
        sockaddr_in probe_addr{};
        probe_addr.sin_family = AF_INET;
        probe_addr.sin_port = htons(target.target_port);
        probe_addr.sin_addr.s_addr = hostAddr;

        int sent = sendto(
            s,
            reinterpret_cast<const char*>(&pkt),
            sizeof(pkt),
            0,
            reinterpret_cast<struct sockaddr*>(&probe_addr),
            sizeof(probe_addr)
        );
        if (sent == static_cast<int>(sizeof(pkt))) {
            any_sent = true;
        }
    }

    target.last_discover_time = std::chrono::steady_clock::now();
    return any_sent;
}

bool UdpTransmitter::SendDiscover(int target_index) {
    if (socket_ == ~0ULL || targets_.empty()) return false;

    if (target_index >= 0) {
        if (static_cast<size_t>(target_index) < targets_.size()) {
            return SendDiscoverForTarget(targets_[static_cast<size_t>(target_index)]);
        }
        return false;
    }

    bool any_sent = false;
    for (auto& target : targets_) {
        // Only targets requiring discovery (unresolved host)
        if (!target.discovered_host.empty()) continue;
        if (SendDiscoverForTarget(target)) {
            any_sent = true;
        }
    }
    return any_sent;
}

bool UdpTransmitter::PollDiscoverAck(uint32_t timeout_ms) {
    if (socket_ == ~0ULL || targets_.empty()) return false;
    SockType s = static_cast<SockType>(socket_);

#if !defined(_WIN32)
    if (s < 0 || s >= FD_SETSIZE) {
        return false;
    }
#endif

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto now = std::chrono::steady_clock::now();
        long rem_ms = (timeout_ms > 0 && now < deadline)
            ? static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count())
            : 0;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s, &read_fds);

        timeval tv{};
        tv.tv_sec = rem_ms / 1000;
        tv.tv_usec = (rem_ms % 1000) * 1000;

        int sel = select(static_cast<int>(s + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel <= 0 || !FD_ISSET(s, &read_fds)) {
            return false;
        }

        Wire::DiscoverAckPacket ack{};
        sockaddr_in sender_addr{};
        SockLen len = sizeof(sender_addr);
        int received = recvfrom(
            s,
            reinterpret_cast<char*>(&ack),
            sizeof(ack),
            0,
            reinterpret_cast<struct sockaddr*>(&sender_addr),
            &len
        );

        if (received != sizeof(Wire::DiscoverAckPacket)) {
            if (timeout_ms == 0) return false;
            continue;
        }

        for (auto& target : targets_) {
            if (target.pending_transaction_id == 0) continue;

            std::string ack_rig;
            uint16_t ack_port = 0;
            uint64_t ack_trans_id = 0;
            if (Crypto::VerifyDiscoverAck(target.public_key, ack, ack_rig, ack_port, ack_trans_id, target.pending_eph_sk)) {
                // Verify both rig_id and matching transaction challenge nonce
                if (ack_rig == rig_id_ && ack_trans_id == target.pending_transaction_id) {
                    char ip_str[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));
                    target.discovered_host = ip_str;
                    if (ack_port != 0) {
                        target.target_port = ack_port;
                    }
                    target.consecutive_unacked = 0;
                    target.pending_transaction_id = 0;
                    Crypto::SecureWipe(target.pending_eph_sk, sizeof(target.pending_eph_sk));
                    LOG_INFO("CLUSTER", "Auto-discovered Command Center for '" + target.name + "' at " + target.discovered_host + ":" + std::to_string(target.target_port));
                    return true;
                }
            }
        }

        if (timeout_ms == 0) break;
    }

    return false;
}

void UdpTransmitter::ForceRediscovery() {
    for (auto& t : targets_) {
        t.discovered_host.clear();
        t.consecutive_unacked = 0;
        t.last_discover_time = std::chrono::steady_clock::time_point{};
        t.discover_attempts = 0;
    }
    if (IsAutoDiscovery()) {
        SendDiscover();
    }
}

std::string UdpTransmitter::GetDiscoveredHost() const {
    for (const auto& t : targets_) {
        if (!t.discovered_host.empty()) return t.discovered_host;
    }
    return "";
}

bool UdpTransmitter::IsAutoDiscovery() const {
    for (const auto& t : targets_) {
        if (t.is_auto_discovery) return true;
    }
    return false;
}

bool UdpTransmitter::SendPayload(Wire::SatellitePayload& payload) {
    if (socket_ == ~0ULL || targets_.empty()) return false;

    SockType s = static_cast<SockType>(socket_);
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < targets_.size(); ++i) {
        auto& t = targets_[i];
        if (t.is_auto_discovery) {
            PollDiscoverAck(0);
            if (t.discovered_host.empty()) {
                if (t.last_discover_time == std::chrono::steady_clock::time_point{} ||
                    (now - t.last_discover_time) >= std::chrono::seconds(10)) {
                    SendDiscover(static_cast<int>(i));
                }
            }
        }
    }

    // Fill protocol headers
    std::memcpy(payload.magic, Wire::WIRE_MAGIC, 4);
    payload.version = Wire::WIRE_VERSION;
    std::memcpy(payload.rig_id, rig_id_, sizeof(payload.rig_id));
    payload.session_id = session_id_;
    payload.packet_counter = ++packet_counter_;

    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    payload.sequence_num = static_cast<uint64_t>(now_us);

    bool any_target_sent = false;
    auto bcastTargets = GetSubnetBroadcastTargets();

    for (size_t i = 0; i < targets_.size(); ++i) {
        auto& target = targets_[i];
        Wire::SatelliteWirePacket packet{};
        if (!Crypto::PackAndEncrypt(target.public_key, payload, packet)) {
            continue;
        }

        bool target_sent = false;

        // Skip redundant subnet broadcasts once discovered
        if (!target.discovered_host.empty()) {
            sockaddr_in dest_addr{};
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(target.target_port);
            if (HttpClient::ResolveHost(target.discovered_host, target.target_port, dest_addr)) {
                int sent_unicast = sendto(
                    s,
                    reinterpret_cast<const char*>(&packet),
                    sizeof(packet),
                    0,
                    reinterpret_cast<struct sockaddr*>(&dest_addr),
                    sizeof(dest_addr)
                );
                if (sent_unicast == static_cast<int>(sizeof(packet))) {
                    target_sent = true;
                }
            }
        } else {
            // Unresolved target: broadcast if auto-discovery or static broadcast
            bool is_bcast_mode = target.is_auto_discovery || target.target_host == "auto" || target.target_host == "255.255.255.255";
            if (is_bcast_mode) {
                for (uint32_t bcast : bcastTargets) {
                    sockaddr_in bcast_addr{};
                    bcast_addr.sin_family = AF_INET;
                    bcast_addr.sin_port = htons(target.target_port);
                    bcast_addr.sin_addr.s_addr = bcast;

                    int sent_bcast = sendto(
                        s,
                        reinterpret_cast<const char*>(&packet),
                        sizeof(packet),
                        0,
                        reinterpret_cast<struct sockaddr*>(&bcast_addr),
                        sizeof(bcast_addr)
                    );
                    if (sent_bcast == static_cast<int>(sizeof(packet))) {
                        target_sent = true;
                    }
                }
            } else if (!target.target_host.empty()) {
                sockaddr_in dest_addr{};
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(target.target_port);
                if (HttpClient::ResolveHost(target.target_host, target.target_port, dest_addr)) {
                    int sent_unicast = sendto(
                        s,
                        reinterpret_cast<const char*>(&packet),
                        sizeof(packet),
                        0,
                        reinterpret_cast<struct sockaddr*>(&dest_addr),
                        sizeof(dest_addr)
                    );
                    if (sent_unicast == static_cast<int>(sizeof(packet))) {
                        target_sent = true;
                    }
                }
            }
        }

        if (target_sent) {
            any_target_sent = true;
            if (target.is_auto_discovery && !target.discovered_host.empty()) {
                target.consecutive_unacked++;
                if (target.consecutive_unacked >= 60) {
                    // Periodically re-verify discovery every 60 packets
                    if (target.pending_transaction_id != 0) {
                        target.discovered_host.clear();
                    }
                    SendDiscover(static_cast<int>(i));
                    target.consecutive_unacked = 0;
                }
            }
        }
    }

    return any_target_sent;
}

// --- UdpReceiver ---

UdpReceiver::UdpReceiver() {
    EnsureSocketsInit();
}

UdpReceiver::~UdpReceiver() {
    Close();
}

bool UdpReceiver::Bind(uint16_t port, const uint8_t key[32]) {
    Close();
    std::memcpy(key_, key, 32);
    bound_port_ = port;

    SockType s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == InvalidSock) return false;

#if !defined(_WIN32)
    if (s < 0 || s >= FD_SETSIZE) {
        CloseSocket(s);
        return false;
    }
#endif

    // Set non-blocking mode
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    // Allow broadcast reception across all network stacks
    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

    // On Windows, enforce exclusive address use to prevent local port hijacking (VULN-07)
#if defined(_WIN32)
    int exclusive = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        CloseSocket(s);
        return false;
    }

    socket_ = static_cast<uintptr_t>(s);
    is_bound_ = true;
    tracker_count_ = 0;
    std::memset(trackers_, 0, sizeof(trackers_));
    std::memset(tombstones_, 0, sizeof(tombstones_));
    return true;
}

void UdpReceiver::Close() {
    if (socket_ != ~0ULL) {
        CloseSocket(static_cast<SockType>(socket_));
        socket_ = ~0ULL;
    }
    is_bound_ = false;
    bound_port_ = 0;
    Crypto::SecureWipe(key_, sizeof(key_));
    tracker_count_ = 0;
    std::memset(trackers_, 0, sizeof(trackers_));
    std::memset(tombstones_, 0, sizeof(tombstones_));
}

void UdpReceiver::SaveTombstone(const char rig_id[16], const Wire::RigReplayState& state) {
    if (state.last_seq_us == 0) return;

    // Check if an existing tombstone exists for this rig
    for (size_t i = 0; i < MAX_TOMBSTONES; ++i) {
        if (tombstones_[i].valid && std::strncmp(tombstones_[i].rig_id, rig_id, 16) == 0) {
            tombstones_[i].last_session_id = state.last_session_id;
            tombstones_[i].last_counter = state.last_counter;
            tombstones_[i].last_seq_us = state.last_seq_us;
            return;
        }
    }

    // Find an empty slot
    for (size_t i = 0; i < MAX_TOMBSTONES; ++i) {
        if (!tombstones_[i].valid) {
            std::memcpy(tombstones_[i].rig_id, rig_id, 16);
            tombstones_[i].last_session_id = state.last_session_id;
            tombstones_[i].last_counter = state.last_counter;
            tombstones_[i].last_seq_us = state.last_seq_us;
            tombstones_[i].valid = true;
            return;
        }
    }

    // Buffer full: shift FIFO
    for (size_t i = 1; i < MAX_TOMBSTONES; ++i) {
        tombstones_[i - 1] = tombstones_[i];
    }
    std::memcpy(tombstones_[MAX_TOMBSTONES - 1].rig_id, rig_id, 16);
    tombstones_[MAX_TOMBSTONES - 1].last_session_id = state.last_session_id;
    tombstones_[MAX_TOMBSTONES - 1].last_counter = state.last_counter;
    tombstones_[MAX_TOMBSTONES - 1].last_seq_us = state.last_seq_us;
    tombstones_[MAX_TOMBSTONES - 1].valid = true;
}

void UdpReceiver::RestoreTombstone(const char rig_id[16], Wire::RigReplayState& state) {
    for (size_t i = 0; i < MAX_TOMBSTONES; ++i) {
        if (tombstones_[i].valid && std::strncmp(tombstones_[i].rig_id, rig_id, 16) == 0) {
            state.last_session_id = tombstones_[i].last_session_id;
            state.last_counter = tombstones_[i].last_counter;
            state.last_seq_us = tombstones_[i].last_seq_us;
            state.window_bitmap = 1ULL;
            return;
        }
    }
}

UdpReceiver::RigTracker* UdpReceiver::GetOrCreateTracker(const char rig_id[16], uint64_t now_us) {
    for (uint8_t i = 0; i < tracker_count_; ++i) {
        if (std::strncmp(trackers_[i].rig_id, rig_id, 16) == 0) {
            trackers_[i].last_active_us = now_us;
            return &trackers_[i];
        }
    }
    if (tracker_count_ < 8) {
        RigTracker* t = &trackers_[tracker_count_++];
        std::memset(t, 0, sizeof(RigTracker));
        std::memcpy(t->rig_id, rig_id, 16);
        t->last_active_us = now_us;
        RestoreTombstone(rig_id, t->state);
        return t;
    }

    // Tracker pool exhaustion defense: LRU eviction to prevent pool starvation DoS (VULN-03)
    int oldest_idx = 0;
    uint64_t oldest_time = trackers_[0].last_active_us;
    for (uint8_t i = 1; i < tracker_count_; ++i) {
        if (trackers_[i].last_active_us < oldest_time) {
            oldest_time = trackers_[i].last_active_us;
            oldest_idx = i;
        }
    }
    RigTracker* victim = &trackers_[oldest_idx];
    SaveTombstone(victim->rig_id, victim->state);

    RigTracker* t = victim;
    std::memset(t, 0, sizeof(RigTracker));
    std::memcpy(t->rig_id, rig_id, 16);
    t->last_active_us = now_us;
    RestoreTombstone(rig_id, t->state);
    return t;
}

bool UdpReceiver::ReceivePacket(Wire::SatellitePayload& out_payload) {
    if (!is_bound_ || socket_ == ~0ULL) return false;

    SockType s = static_cast<SockType>(socket_);

    uint8_t buffer[512] = {0};
    sockaddr_in src_addr{};
    SockLen src_len = sizeof(src_addr);

    int n = recvfrom(
        s,
        reinterpret_cast<char*>(buffer),
        sizeof(buffer),
        0,
        reinterpret_cast<struct sockaddr*>(&src_addr),
        &src_len
    );

    if (n <= 0) {
        return false;
    }

    char sender_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &src_addr.sin_addr, sender_ip, sizeof(sender_ip));
    uint16_t sender_port = ntohs(src_addr.sin_port);
    std::string sender_endpoint = std::string(sender_ip) + ":" + std::to_string(sender_port);

    auto now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    // Check for 104-byte Subnet Discovery Request
    if (n == static_cast<int>(sizeof(Wire::DiscoverPacket))) {
        const auto* disc = reinterpret_cast<const Wire::DiscoverPacket*>(buffer);
        std::string sender_rig;
        uint64_t disc_trans_id = 0;
        if (Crypto::VerifyDiscover(key_, *disc, sender_rig, disc_trans_id)) {
            if (!allowed_rigs_.empty()) {
                bool authorized = false;
                for (const auto& allowed : allowed_rigs_) {
                    if (RigIdEquals(sender_rig, allowed)) {
                        authorized = true;
                        break;
                    }
                }
                if (!authorized) {
                    LOG_WARN("CLUSTER", "Rejected discovery request from unauthorized rig identity: " + sender_rig);
                    return false;
                }
            }
            LOG_INFO("CLUSTER", "Verified discover request from " + sender_rig + " at " + sender_endpoint);
            // Reply with authenticated 104-byte DISCOVER_ACK echoing the request's transaction_id and sender's eph_pk
            Wire::DiscoverAckPacket ack{};
            if (Crypto::PackDiscoverAck(key_, sender_rig, bound_port_, disc_trans_id, ack, disc->eph_pk)) {
                sendto(
                    s,
                    reinterpret_cast<const char*>(&ack),
                    sizeof(ack),
                    0,
                    reinterpret_cast<struct sockaddr*>(&src_addr),
                    src_len
                );
            }
        } else {
            LOG_WARN("CLUSTER", "Discovery request auth failed from " + sender_endpoint);
        }
        return false; // Handshake packet processed
    }

    if (n != static_cast<int>(sizeof(Wire::SatelliteWirePacket))) {
        LOG_WARN("CLUSTER", "Ignored UDP packet from " + sender_endpoint + ": unexpected size " + std::to_string(n) + " bytes");
        return false;
    }

    const auto* packet = reinterpret_cast<const Wire::SatelliteWirePacket*>(buffer);

    // Decrypt and authenticate AEAD packet
    Wire::SatellitePayload decrypted{};
    if (!Crypto::DecryptAndUnpack(key_, *packet, decrypted)) {
        std::string raw_rig(packet->rig_id, strnlen(packet->rig_id, sizeof(packet->rig_id)));
        std::string target_rig;
        for (char c : raw_rig) {
            if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127) {
                target_rig.push_back(c);
            } else {
                target_rig.push_back('_');
            }
        }
        LOG_WARN("CLUSTER", "Packet decryption/auth failed from " + sender_endpoint + " (rig_id=" + target_rig + ", key mismatch?)");
        return false; // Authentication failed or corrupted packet
    }

    // BS-VULN-01: Enforce null-termination on all decrypted wire strings
    decrypted.rig_id[sizeof(decrypted.rig_id) - 1] = '\0';
    for (size_t m_idx = 0; m_idx < Wire::WIRE_MAX_MODELS; ++m_idx) {
        decrypted.llms[m_idx].model_name[sizeof(decrypted.llms[m_idx].model_name) - 1] = '\0';
        decrypted.llms[m_idx].quant[sizeof(decrypted.llms[m_idx].quant) - 1] = '\0';
    }

    // Drop self-loopback packets BEFORE touching anti-replay tracker (prevents local transmitter clobbering remote state)
    if (ignored_session_id_ != 0 && decrypted.session_id == ignored_session_id_) {
        return false;
    }

    // Validate protocol magic and version
    if (std::memcmp(decrypted.magic, Wire::WIRE_MAGIC, 4) != 0 ||
        decrypted.version != Wire::WIRE_VERSION) {
        LOG_WARN("CLUSTER", "Packet protocol version/magic mismatch from " + sender_endpoint);
        return false;
    }

    // Whitelist check (VULN-02): enforce authorized sender rig identities
    std::string decrypted_rig(decrypted.rig_id, strnlen(decrypted.rig_id, sizeof(decrypted.rig_id)));
    if (!allowed_rigs_.empty()) {
        bool authorized = false;
        for (const auto& allowed : allowed_rigs_) {
            if (RigIdEquals(decrypted_rig, allowed)) {
                authorized = true;
                break;
            }
        }
        if (!authorized) {
            LOG_WARN("CLUSTER", "Rejected telemetry from unauthorized rig identity: " + decrypted_rig);
            return false;
        }
    }

    // Verify outer wire rig_id matches authenticated payload rig_id
    if (std::memcmp(packet->rig_id, decrypted.rig_id, sizeof(packet->rig_id)) != 0) {
        LOG_WARN("CLUSTER", "Packet wire rig_id does not match decrypted payload rig_id from " + sender_endpoint);
        return false;
    }

    // Anti-replay sliding window & wall-clock freshness verification per rig
    RigTracker* tracker = GetOrCreateTracker(decrypted.rig_id, now_us);
    if (!tracker) return false;

    if (!tracker->state.ValidateAndAccept(decrypted, now_us)) {
        return false; // Replayed, duplicate, or out-of-window packet dropped cleanly
    }
    tracker->last_active_us = now_us;
    SaveTombstone(tracker->rig_id, tracker->state);

    out_payload = decrypted;
    return true;
}

} // namespace BareStats
