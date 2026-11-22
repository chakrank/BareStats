#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include "metrics/wire_protocol.h"

namespace BareStats {
namespace Crypto {

constexpr size_t KEY_SIZE = 32;
constexpr size_t NONCE_SIZE = 24;
constexpr size_t MAC_SIZE = 16;

// Cryptographically secure random byte generation using native OS facilities
bool GenerateRandomBytes(uint8_t* out, size_t len);
bool GenerateKey(uint8_t out_key[KEY_SIZE]);
bool GenerateNonce(uint8_t out_nonce[NONCE_SIZE]);

// Asymmetric Curve25519 / X25519 Public Key Derivation
bool DerivePublicKey(const uint8_t secret_key[KEY_SIZE], uint8_t out_public_key[KEY_SIZE]);

// Hex encoding/decoding for configuration files (config.ini / satellite.ini)
std::string KeyToHex(const uint8_t key[KEY_SIZE]);
bool HexToKey(std::string_view hex, uint8_t out_key[KEY_SIZE]);

// Constant-time memory erasure
void SecureWipe(void* buffer, size_t size);

// Low-level AEAD (XChaCha20-Poly1305) operations
void EncryptAead(
    const uint8_t key[KEY_SIZE],
    const uint8_t nonce[NONCE_SIZE],
    const uint8_t* ad, size_t ad_len,
    const uint8_t* plain, size_t plain_len,
    uint8_t* cipher,
    uint8_t mac[MAC_SIZE]
);

bool DecryptAead(
    const uint8_t key[KEY_SIZE],
    const uint8_t nonce[NONCE_SIZE],
    const uint8_t mac[MAC_SIZE],
    const uint8_t* ad, size_t ad_len,
    const uint8_t* cipher, size_t cipher_len,
    uint8_t* plain
);

// High-level Wire Protocol packet pack/unpack
bool PackAndEncrypt(
    const uint8_t key[KEY_SIZE],
    const Wire::SatellitePayload& payload,
    Wire::SatelliteWirePacket& out_packet
);

bool DecryptAndUnpack(
    const uint8_t key[KEY_SIZE],
    const Wire::SatelliteWirePacket& packet,
    Wire::SatellitePayload& out_payload
);

// Subnet Auto-Discovery Handshake (104-byte authenticated challenge-response packets)
bool PackDiscover(
    const uint8_t target_public_key[KEY_SIZE],
    const std::string& rig_id,
    uint64_t transaction_id,
    Wire::DiscoverPacket& out_packet,
    uint8_t* out_eph_sk = nullptr
);

bool VerifyDiscover(
    const uint8_t private_key[KEY_SIZE],
    const Wire::DiscoverPacket& packet,
    std::string& out_rig_id,
    uint64_t& out_transaction_id
);

bool PackDiscoverAck(
    const uint8_t private_key[KEY_SIZE],
    const std::string& rig_id,
    uint16_t listen_port,
    uint64_t transaction_id,
    Wire::DiscoverAckPacket& out_packet,
    const uint8_t* sender_eph_pk = nullptr
);

bool VerifyDiscoverAck(
    const uint8_t target_public_key[KEY_SIZE],
    const Wire::DiscoverAckPacket& packet,
    std::string& out_rig_id,
    uint16_t& out_listen_port,
    uint64_t& out_transaction_id,
    const uint8_t* sender_eph_sk = nullptr
);

} // namespace Crypto
} // namespace BareStats
