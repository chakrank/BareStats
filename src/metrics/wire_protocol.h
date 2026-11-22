#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace BareStats {
namespace Wire {

// Enforce strict 1-byte packing across MSVC, GCC, and Clang
#pragma pack(push, 1)

// --- Protocol Constants ---
constexpr char     WIRE_MAGIC[4] = {'B', 'S', 'A', 'T'};
constexpr uint8_t  WIRE_VERSION = 1;
constexpr uint16_t WIRE_DEFAULT_PORT = 9889;
constexpr size_t   WIRE_MAX_GPUS = 8;
constexpr size_t   WIRE_MAX_MODELS = 4;

// System Flags Bitmask
constexpr uint8_t SYS_FLAG_UMA = 1 << 0; // Bit 0: Unified Memory Architecture (Apple Silicon / Strix Halo)
constexpr uint8_t SYS_FLAG_LLM_ONLINE = 1 << 1; // Bit 1: Remote inference server active (Ollama, llama-server, vLLM)

// --- LLM Lifecycle State Enum ---
enum class LlmState : uint8_t {
    Offline = 0,
    Loading = 1,
    Thinking = 2,
    Generating = 3,
    BurstHold = 4,   // 3-second freeze holding peak generation TPS
    Compacting = 5,
    Idle = 6,
    Unloading = 7
};

// --- Silicon & Model Sub-Structs (Ordered for natural alignment) ---

struct WireGpuMetrics {
    uint32_t vram_used_mb;    // 4 bytes
    uint32_t vram_total_mb;   // 4 bytes
    uint16_t power_w;         // 2 bytes (0-65535 Watts)
    uint16_t fan_rpm;         // 2 bytes (0-65535 RPM)
    uint8_t  utilization_pct; // 1 byte (0-100)
    int8_t   temp_c;          // 1 byte (-128 to 127 Celsius)
}; // Exactly 14 bytes per GPU

struct LlmMetrics {
    uint32_t vram_used_mb;    // 4 bytes (VRAM consumed by this specific model)
    float    tps;             // 4 bytes (Live tokens per second, IEEE 754 32-bit float)
    uint16_t queue_running;   // 2 bytes (Active inference streams)
    uint16_t queue_waiting;   // 2 bytes (Pending queued requests)
    LlmState state;           // 1 byte (Lifecycle state enum)
    uint8_t  kv_cache_pct;    // 1 byte (0-100 context saturation)
    char     model_name[32];  // 32 bytes (Null-terminated condensed name, e.g. "DeepSeek-R1..14B")
    char     quant[8];        // 8 bytes (Null-terminated, e.g. "Q4_K_M")
}; // Exactly 54 bytes per Model

// --- Encrypted Satellite Payload (Ordered for natural alignment) ---
struct SatellitePayload {
    // Header & Reboot-Resilient Replay Protection
    uint64_t sequence_num;              // 8 bytes (Unix epoch microseconds timestamp)
    uint32_t session_id;                // 4 bytes (Random boot GUID/startup timestamp, reboot resilience)
    uint32_t packet_counter;            // 4 bytes (Monotonically increasing sequence counter within session)
    uint32_t ram_used_mb;               // 4 bytes
    uint32_t ram_total_mb;              // 4 bytes
    char     magic[4];                  // 4 bytes ("BSAT" magic header)
    char     rig_id[16];                // 16 bytes (Null-padded string identifier, e.g. "rig1")
    uint8_t  version;                   // 1 byte (Protocol version = 1)
    uint8_t  cpu_load_pct;              // 1 byte (0-100)
    uint8_t  sys_flags;                 // 1 byte (Bit 0 = UMA)
    uint8_t  gpu_count;                 // 1 byte (0 to 8)
    // Offset 48: Naturally aligned 4-byte boundary
    WireGpuMetrics gpus[WIRE_MAX_GPUS]; // 112 bytes (8 * 14 bytes)
    // Offset 160: Naturally aligned 4-byte boundary
    uint8_t    llm_count;               // 1 byte (0 to 4)
    uint8_t    _reserved[3];            // 3 bytes (Padding to preserve 4-byte natural alignment for llms)
    // Offset 164: Naturally aligned 4-byte boundary
    LlmMetrics llms[WIRE_MAX_MODELS];   // 216 bytes (4 * 54 bytes)
}; // Total: 48 + 112 + 4 + 216 = 380 bytes

// --- Wire Packet Transmitted via UDP (Asymmetric Sealed Box) ---
// Outer header contains rig_id (16 bytes) and ephemeral public key (32 bytes) as Authenticated Data (AAD)
struct SatelliteWirePacket {
    char    rig_id[16];                           // 16 bytes (Plaintext header, Authenticated Data)
    uint8_t mac[16];                              // 16 bytes (Poly1305 MAC Tag)
    uint8_t nonce[24];                            // 24 bytes (XChaCha20 Nonce)
    uint8_t eph_pk[32];                           // 32 bytes (Ephemeral Curve25519 Public Key)
    uint8_t ciphertext[sizeof(SatellitePayload)]; // 380 bytes encrypted payload
}; // Total: 16 + 16 + 24 + 32 + 380 = 468 bytes

// --- Subnet Auto-Discovery Protocol (Strict 104-byte natural alignment) ---
constexpr char     WIRE_DISCOVER_MAGIC[4] = {'B', 'D', 'I', 'S'};
constexpr char     WIRE_DISCOVER_ACK_MAGIC[4] = {'B', 'A', 'C', 'K'};

struct DiscoverPacket {
    char     magic[4];          // 4 bytes: "BDIS"
    uint8_t  version;           // 1 byte: 1
    char     rig_id[16];        // 16 bytes: sender rig_id (null-padded)
    uint64_t transaction_id;    // 8 bytes: random challenge request nonce
    uint8_t  eph_pk[32];         // 32 bytes: sender ephemeral Curve25519 public key
    uint8_t  mac[16];           // 16 bytes: Poly1305 MAC tag
    uint8_t  nonce[24];         // 24 bytes: random nonce
    uint8_t  _pad[3];           // 3 bytes padding
}; // Total: 4 + 1 + 16 + 8 + 32 + 16 + 24 + 3 = 104 bytes

struct DiscoverAckPacket {
    char     magic[4];          // 4 bytes: "BACK"
    uint8_t  version;           // 1 byte: 1
    char     rig_id[16];        // 16 bytes: target rig_id
    uint16_t listen_port;       // 2 bytes: Command Center listen port
    uint64_t transaction_id;    // 8 bytes: echoed challenge response nonce
    uint8_t  eph_pk[32];         // 32 bytes: echoed sender ephemeral public key
    uint8_t  mac[16];           // 16 bytes: Poly1305 MAC tag
    uint8_t  nonce[24];         // 24 bytes: random nonce
    uint8_t  _pad[1];           // 1 byte padding
}; // Total: 4 + 1 + 16 + 2 + 8 + 32 + 16 + 24 + 1 = 104 bytes

#pragma pack(pop)

// --- Compile-Time Layout Assertions ---
static_assert(sizeof(WireGpuMetrics) == 14, "WireGpuMetrics size mismatch: must be 14 bytes");
static_assert(sizeof(LlmMetrics) == 54, "LlmMetrics size mismatch: must be 54 bytes");
static_assert(sizeof(SatellitePayload) == 380, "SatellitePayload size mismatch: must be 380 bytes");
static_assert(sizeof(SatelliteWirePacket) == 468, "SatelliteWirePacket size mismatch: must be 468 bytes");
static_assert(sizeof(DiscoverPacket) == 104, "DiscoverPacket size mismatch: must be 104 bytes");
static_assert(sizeof(DiscoverAckPacket) == 104, "DiscoverAckPacket size mismatch: must be 104 bytes");

static_assert(std::is_trivially_copyable_v<WireGpuMetrics>, "WireGpuMetrics must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LlmMetrics>, "LlmMetrics must be trivially copyable");
static_assert(std::is_trivially_copyable_v<SatellitePayload>, "SatellitePayload must be trivially copyable");
static_assert(std::is_trivially_copyable_v<SatelliteWirePacket>, "SatelliteWirePacket must be trivially copyable");
static_assert(std::is_trivially_copyable_v<DiscoverPacket>, "DiscoverPacket must be trivially copyable");
static_assert(std::is_trivially_copyable_v<DiscoverAckPacket>, "DiscoverAckPacket must be trivially copyable");

// Replay state tracking (64-packet bitmap sliding window + wall clock freshness)
struct RigReplayState {
    uint32_t last_session_id = 0;
    uint32_t last_counter = 0;
    uint64_t window_bitmap = 0;
    uint64_t last_seq_us = 0;

    bool ValidateAndAccept(const SatellitePayload& p, uint64_t current_time_us = 0) {
        // Wall-clock freshness check (+/- 300 seconds = 300,000,000 us) to defeat captured stale replays
        if (current_time_us != 0) {
            int64_t delta = static_cast<int64_t>(current_time_us) - static_cast<int64_t>(p.sequence_num);
            if (delta < -300'000'000LL || delta > 300'000'000LL) {
                return false;
            }
        }

        // Reboot / session reset detection
        if (p.session_id != last_session_id) {
            if (last_seq_us == 0 || p.sequence_num > last_seq_us) {
                last_session_id = p.session_id;
                last_counter = p.packet_counter;
                window_bitmap = 1ULL;
                last_seq_us = p.sequence_num;
                return true;
            }
            return false;
        }

        // In-session packet counter evaluation with 64-packet sliding window
        if (p.packet_counter > last_counter) {
            uint32_t advance = p.packet_counter - last_counter;
            // Cap counter jump per packet to prevent sliding window exhaustion and counter burnout
            static constexpr uint32_t MAX_COUNTER_ADVANCE = 1000;
            if (advance > MAX_COUNTER_ADVANCE) {
                return false;
            }
            if (advance < 64) {
                window_bitmap = (window_bitmap << advance) | 1ULL;
            } else {
                window_bitmap = 1ULL;
            }
            last_counter = p.packet_counter;
            last_seq_us = p.sequence_num;
            return true;
        }

        // Out-of-order packet within recent 64-packet window
        uint32_t diff = last_counter - p.packet_counter;
        if (diff >= 64) return false; // Too old
        if (window_bitmap & (1ULL << diff)) return false; // Duplicate replay

        window_bitmap |= (1ULL << diff);
        return true;
    }
};

} // namespace Wire
} // namespace BareStats
