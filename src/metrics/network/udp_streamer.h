#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>
#include "metrics/wire_protocol.h"

namespace BareStats {

struct SatelliteTargetConfig {
    std::string name;
    std::string target_host;
    uint16_t target_port = 9889;
    uint8_t public_key[32] = {0};
};

struct SatelliteTargetState {
    std::string name;
    std::string target_host;
    uint16_t target_port = 9889;
    uint8_t public_key[32] = {0};
    bool is_auto_discovery = false;
    std::string discovered_host;
    uint32_t consecutive_unacked = 0;
    uint64_t pending_transaction_id = 0;
    uint8_t pending_eph_sk[32] = {0};
    std::chrono::steady_clock::time_point last_discover_time{};
    uint32_t discover_attempts = 0;
};

class UdpTransmitter {
public:
    UdpTransmitter();
    ~UdpTransmitter();

    bool Open(const std::string& rig_id, const std::vector<SatelliteTargetConfig>& targets);
    bool Open(const std::string& target_host, uint16_t port, const uint8_t key[32], const std::string& rig_id);
    void Close();

    bool SendPayload(Wire::SatellitePayload& payload);

    // Subnet Auto-Discovery Handshake
    bool SendDiscover(int target_index = -1);
    bool PollDiscoverAck(uint32_t timeout_ms = 0);
    std::string GetDiscoveredHost() const;
    bool IsAutoDiscovery() const;
    void ForceRediscovery();
    uint32_t GetSessionId() const { return session_id_; }
    size_t GetTargetCount() const { return targets_.size(); }
    const std::vector<SatelliteTargetState>& GetTargets() const { return targets_; }

private:
    uintptr_t socket_ = ~0ULL; // Cross-platform socket representation
    char rig_id_[16] = {0};
    uint32_t session_id_ = 0;
    uint32_t packet_counter_ = 0;

    std::vector<SatelliteTargetState> targets_;

    bool SendDiscoverForTarget(SatelliteTargetState& target);

    std::vector<uint32_t> cached_bcast_addrs_;
    std::chrono::steady_clock::time_point last_bcast_refresh_;
    std::vector<uint32_t> GetSubnetBroadcastTargets();
    std::vector<uint32_t> GetSubnetProbeTargets();
};

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    bool Bind(uint16_t port, const uint8_t key[32]);
    void Close();
    uint16_t GetBoundPort() const { return bound_port_; }
    void SetIgnoredSessionId(uint32_t session_id) { ignored_session_id_ = session_id; }
    void SetAllowedRigs(const std::vector<std::string>& allowed_rigs) { allowed_rigs_ = allowed_rigs; }
    void AddAllowedRig(const std::string& rig_id) { if (!rig_id.empty()) allowed_rigs_.push_back(rig_id); }
    const std::vector<std::string>& GetAllowedRigs() const { return allowed_rigs_; }

    // Non-blocking poll for an authenticated packet.
    // Handles DISCOVER handshake requests and populates out_payload on valid telemetry payloads.
    bool ReceivePacket(Wire::SatellitePayload& out_payload);

private:
    uintptr_t socket_ = ~0ULL;
    uint8_t key_[32] = {0};
    bool is_bound_ = false;
    uint16_t bound_port_ = 0;
    uint32_t ignored_session_id_ = 0;
    std::vector<std::string> allowed_rigs_;

    // Per-rig anti-replay sliding window trackers with idle eviction
    struct RigTracker {
        char rig_id[16] = {0};
        uint64_t last_active_us = 0;
        Wire::RigReplayState state{};
    };
    RigTracker trackers_[8];
    uint8_t tracker_count_ = 0;

    // BS-VULN-05: Preserve replay monotonicity across tracker evictions
    struct RigTombstone {
        char rig_id[16] = {0};
        uint32_t last_session_id = 0;
        uint32_t last_counter = 0;
        uint64_t last_seq_us = 0;
        bool valid = false;
    };
    static constexpr size_t MAX_TOMBSTONES = 64;
    RigTombstone tombstones_[MAX_TOMBSTONES] = {};

    void SaveTombstone(const char rig_id[16], const Wire::RigReplayState& state);
    void RestoreTombstone(const char rig_id[16], Wire::RigReplayState& state);

    RigTracker* GetOrCreateTracker(const char rig_id[16], uint64_t now_us = 0);
};

} // namespace BareStats
