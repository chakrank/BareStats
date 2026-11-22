#include "crypto_box.h"
#include "monocypher.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace BareStats {
namespace Crypto {

bool GenerateRandomBytes(uint8_t* out, size_t len) {
    if (!out || len == 0) return false;

#if defined(_WIN32)
    NTSTATUS status = BCryptGenRandom(NULL, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);
#elif defined(__APPLE__)
    arc4random_buf(out, len);
    return true;
#elif defined(__linux__)
    ssize_t res = getrandom(out, len, 0);
    if (res == static_cast<ssize_t>(len)) {
        return true;
    }
    // Fallback to /dev/urandom if getrandom() is interrupted or unavailable
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t total_read = 0;
        while (total_read < len) {
            ssize_t n = read(fd, out + total_read, len - total_read);
            if (n <= 0) break;
            total_read += n;
        }
        close(fd);
        return total_read == len;
    }
    return false;
#else
    return false;
#endif
}

bool GenerateKey(uint8_t out_key[KEY_SIZE]) {
    return GenerateRandomBytes(out_key, KEY_SIZE);
}

bool GenerateNonce(uint8_t out_nonce[NONCE_SIZE]) {
    return GenerateRandomBytes(out_nonce, NONCE_SIZE);
}

std::string KeyToHex(const uint8_t key[KEY_SIZE]) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        ss << std::setw(2) << static_cast<int>(key[i]);
    }
    return ss.str();
}

static int HexCharToInt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool HexToKey(std::string_view hex, uint8_t out_key[KEY_SIZE]) {
    if (hex.length() != KEY_SIZE * 2) return false;

    for (size_t i = 0; i < KEY_SIZE; ++i) {
        int hi = HexCharToInt(hex[i * 2]);
        int lo = HexCharToInt(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out_key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void SecureWipe(void* buffer, size_t size) {
    if (buffer && size > 0) {
        crypto_wipe(buffer, size);
    }
}

void EncryptAead(
    const uint8_t key[KEY_SIZE],
    const uint8_t nonce[NONCE_SIZE],
    const uint8_t* ad, size_t ad_len,
    const uint8_t* plain, size_t plain_len,
    uint8_t* cipher,
    uint8_t mac[MAC_SIZE]
) {
    crypto_aead_lock(cipher, mac, key, nonce, ad, ad_len, plain, plain_len);
}

bool DecryptAead(
    const uint8_t key[KEY_SIZE],
    const uint8_t nonce[NONCE_SIZE],
    const uint8_t mac[MAC_SIZE],
    const uint8_t* ad, size_t ad_len,
    const uint8_t* cipher, size_t cipher_len,
    uint8_t* plain
) {
    int res = crypto_aead_unlock(plain, mac, key, nonce, ad, ad_len, cipher, cipher_len);
    return res == 0;
}

bool DerivePublicKey(const uint8_t secret_key[KEY_SIZE], uint8_t out_public_key[KEY_SIZE]) {
    if (!secret_key || !out_public_key) return false;
    crypto_x25519_public_key(out_public_key, secret_key);
    return true;
}

bool PackAndEncrypt(
    const uint8_t key[KEY_SIZE],
    const Wire::SatellitePayload& payload,
    Wire::SatelliteWirePacket& out_packet
) {
    // 1. Copy rig_id to unencrypted wire header
    std::memcpy(out_packet.rig_id, payload.rig_id, sizeof(out_packet.rig_id));

    // 2. Generate cryptographically secure random 24-byte nonce
    if (!GenerateNonce(out_packet.nonce)) {
        return false;
    }

    // 3. Ephemeral Curve25519 keypair generation
    uint8_t eph_sk[KEY_SIZE] = {0};
    if (!GenerateKey(eph_sk)) {
        return false;
    }
    crypto_x25519_public_key(out_packet.eph_pk, eph_sk);

    // 4. ECDH with recipient's public key
    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, eph_sk, key);
    SecureWipe(eph_sk, sizeof(eph_sk));
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }

    // 5. Derive symmetric AEAD key via keyed BLAKE2b (KDF)
    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), out_packet.eph_pk, sizeof(out_packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    // 6. Authenticated Associated Data: rig_id (16 bytes) || eph_pk (32 bytes) = 48 bytes
    uint8_t ad[sizeof(out_packet.rig_id) + sizeof(out_packet.eph_pk)];
    std::memcpy(ad, out_packet.rig_id, sizeof(out_packet.rig_id));
    std::memcpy(ad + sizeof(out_packet.rig_id), out_packet.eph_pk, sizeof(out_packet.eph_pk));

    // 7. Encrypt payload and compute Poly1305 MAC
    EncryptAead(
        aead_key,
        out_packet.nonce,
        ad, sizeof(ad),
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload),
        out_packet.ciphertext,
        out_packet.mac
    );
    SecureWipe(aead_key, sizeof(aead_key));
    return true;
}

bool DecryptAndUnpack(
    const uint8_t key[KEY_SIZE],
    const Wire::SatelliteWirePacket& packet,
    Wire::SatellitePayload& out_payload
) {
    // 1. ECDH: recipient private_key * packet.eph_pk
    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, key, packet.eph_pk);
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }

    // 2. Derive symmetric AEAD key via keyed BLAKE2b (KDF)
    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), packet.eph_pk, sizeof(packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    // 3. Authenticate Associated Data: rig_id (16 bytes) || eph_pk (32 bytes) = 48 bytes
    uint8_t ad[sizeof(packet.rig_id) + sizeof(packet.eph_pk)];
    std::memcpy(ad, packet.rig_id, sizeof(packet.rig_id));
    std::memcpy(ad + sizeof(packet.rig_id), packet.eph_pk, sizeof(packet.eph_pk));

    bool success = DecryptAead(
        aead_key,
        packet.nonce,
        packet.mac,
        ad, sizeof(ad),
        packet.ciphertext, sizeof(packet.ciphertext),
        reinterpret_cast<uint8_t*>(&out_payload)
    );
    SecureWipe(aead_key, sizeof(aead_key));

    if (!success) {
        return false;
    }

    // Validate Magic Header and Protocol Version
    if (std::memcmp(out_payload.magic, Wire::WIRE_MAGIC, sizeof(Wire::WIRE_MAGIC)) != 0) {
        return false;
    }

    if (out_payload.version != Wire::WIRE_VERSION) {
        return false;
    }

    if (std::memcmp(out_payload.rig_id, packet.rig_id, sizeof(packet.rig_id)) != 0) {
        return false;
    }

    return true;
}

bool PackDiscover(
    const uint8_t target_public_key[KEY_SIZE],
    const std::string& rig_id,
    uint64_t transaction_id,
    Wire::DiscoverPacket& out_packet,
    uint8_t* out_eph_sk
) {
    std::memcpy(out_packet.magic, Wire::WIRE_DISCOVER_MAGIC, 4);
    out_packet.version = Wire::WIRE_VERSION;
    std::memset(out_packet.rig_id, 0, sizeof(out_packet.rig_id));
    std::strncpy(out_packet.rig_id, rig_id.c_str(), sizeof(out_packet.rig_id) - 1);
    out_packet.transaction_id = transaction_id;
    std::memset(out_packet._pad, 0, sizeof(out_packet._pad));

    if (!GenerateNonce(out_packet.nonce)) {
        return false;
    }

    uint8_t eph_sk[KEY_SIZE] = {0};
    if (!GenerateKey(eph_sk)) {
        return false;
    }
    crypto_x25519_public_key(out_packet.eph_pk, eph_sk);

    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, eph_sk, target_public_key);
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(eph_sk, sizeof(eph_sk));
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }
    if (out_eph_sk) {
        std::memcpy(out_eph_sk, eph_sk, KEY_SIZE);
    }
    SecureWipe(eph_sk, sizeof(eph_sk));

    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), out_packet.eph_pk, sizeof(out_packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    // Authenticate header (magic + version + rig_id + transaction_id + eph_pk = 61 bytes)
    EncryptAead(aead_key, out_packet.nonce, reinterpret_cast<const uint8_t*>(&out_packet), 61, nullptr, 0, nullptr, out_packet.mac);
    SecureWipe(aead_key, sizeof(aead_key));
    return true;
}

bool VerifyDiscover(
    const uint8_t private_key[KEY_SIZE],
    const Wire::DiscoverPacket& packet,
    std::string& out_rig_id,
    uint64_t& out_transaction_id
) {
    if (std::memcmp(packet.magic, Wire::WIRE_DISCOVER_MAGIC, 4) != 0) {
        return false;
    }
    if (packet.version != Wire::WIRE_VERSION) {
        return false;
    }

    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, private_key, packet.eph_pk);
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }

    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), packet.eph_pk, sizeof(packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    bool ok = DecryptAead(aead_key, packet.nonce, packet.mac, reinterpret_cast<const uint8_t*>(&packet), 61, nullptr, 0, nullptr);
    SecureWipe(aead_key, sizeof(aead_key));
    if (!ok) {
        return false;
    }

    char id_buf[17] = {0};
    std::memcpy(id_buf, packet.rig_id, 16);
    out_rig_id = id_buf;
    out_transaction_id = packet.transaction_id;
    return true;
}

bool PackDiscoverAck(
    const uint8_t private_key[KEY_SIZE],
    const std::string& rig_id,
    uint16_t listen_port,
    uint64_t transaction_id,
    Wire::DiscoverAckPacket& out_packet,
    const uint8_t* sender_eph_pk
) {
    std::memcpy(out_packet.magic, Wire::WIRE_DISCOVER_ACK_MAGIC, 4);
    out_packet.version = Wire::WIRE_VERSION;
    std::memset(out_packet.rig_id, 0, sizeof(out_packet.rig_id));
    std::strncpy(out_packet.rig_id, rig_id.c_str(), sizeof(out_packet.rig_id) - 1);
    out_packet.listen_port = listen_port;
    out_packet.transaction_id = transaction_id;
    std::memset(out_packet._pad, 0, sizeof(out_packet._pad));

    if (sender_eph_pk) {
        std::memcpy(out_packet.eph_pk, sender_eph_pk, KEY_SIZE);
    } else {
        std::memset(out_packet.eph_pk, 0, KEY_SIZE);
    }

    if (!GenerateNonce(out_packet.nonce)) {
        return false;
    }

    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, private_key, out_packet.eph_pk);
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }

    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), out_packet.eph_pk, sizeof(out_packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    // Authenticate header (magic + version + rig_id + listen_port + transaction_id + eph_pk = 63 bytes)
    EncryptAead(aead_key, out_packet.nonce, reinterpret_cast<const uint8_t*>(&out_packet), 63, nullptr, 0, nullptr, out_packet.mac);
    SecureWipe(aead_key, sizeof(aead_key));
    return true;
}

bool VerifyDiscoverAck(
    const uint8_t target_public_key[KEY_SIZE],
    const Wire::DiscoverAckPacket& packet,
    std::string& out_rig_id,
    uint16_t& out_listen_port,
    uint64_t& out_transaction_id,
    const uint8_t* sender_eph_sk
) {
    if (std::memcmp(packet.magic, Wire::WIRE_DISCOVER_ACK_MAGIC, 4) != 0) {
        return false;
    }
    if (packet.version != Wire::WIRE_VERSION) {
        return false;
    }

    if (!sender_eph_sk) {
        return false;
    }

    uint8_t shared_secret[KEY_SIZE] = {0};
    crypto_x25519(shared_secret, sender_eph_sk, target_public_key);
    const uint8_t zero[KEY_SIZE] = {0};
    if (crypto_verify32(shared_secret, zero) == 0) {
        SecureWipe(shared_secret, sizeof(shared_secret));
        return false;
    }

    uint8_t aead_key[KEY_SIZE] = {0};
    crypto_blake2b_keyed(aead_key, sizeof(aead_key), shared_secret, sizeof(shared_secret), packet.eph_pk, sizeof(packet.eph_pk));
    SecureWipe(shared_secret, sizeof(shared_secret));

    bool ok = DecryptAead(aead_key, packet.nonce, packet.mac, reinterpret_cast<const uint8_t*>(&packet), 63, nullptr, 0, nullptr);
    SecureWipe(aead_key, sizeof(aead_key));
    if (!ok) {
        return false;
    }

    char id_buf[17] = {0};
    std::memcpy(id_buf, packet.rig_id, 16);
    out_rig_id = id_buf;
    out_listen_port = packet.listen_port;
    out_transaction_id = packet.transaction_id;
    return true;
}

} // namespace Crypto
} // namespace BareStats
