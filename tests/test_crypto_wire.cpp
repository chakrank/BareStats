#include "test_common.h"
#include "util/crypto/crypto_box.h"
#include "metrics/wire_protocol.h"
#include <cstring>

using namespace BareStats;
using namespace BareStats::Crypto;
using namespace BareStats::Wire;

bool Test_WireProtocol_StructSizesAndAlignment() {
    TEST_ASSERT(sizeof(WireGpuMetrics) == 14, "WireGpuMetrics must be exactly 14 bytes");
    TEST_ASSERT(sizeof(LlmMetrics) == 54, "LlmMetrics must be exactly 54 bytes");
    TEST_ASSERT(sizeof(SatellitePayload) == 380, "SatellitePayload must be exactly 380 bytes");
    TEST_ASSERT(sizeof(SatelliteWirePacket) == 468, "SatelliteWirePacket must be exactly 468 bytes");
    TEST_ASSERT(sizeof(DiscoverPacket) == 104, "DiscoverPacket must be exactly 104 bytes");
    TEST_ASSERT(sizeof(DiscoverAckPacket) == 104, "DiscoverAckPacket must be exactly 104 bytes");

    TEST_ASSERT(std::is_trivially_copyable_v<WireGpuMetrics>, "WireGpuMetrics must be trivially copyable");
    TEST_ASSERT(std::is_trivially_copyable_v<LlmMetrics>, "LlmMetrics must be trivially copyable");
    TEST_ASSERT(std::is_trivially_copyable_v<SatellitePayload>, "SatellitePayload must be trivially copyable");
    TEST_ASSERT(std::is_trivially_copyable_v<SatelliteWirePacket>, "SatelliteWirePacket must be trivially copyable");
    TEST_ASSERT(std::is_trivially_copyable_v<DiscoverPacket>, "DiscoverPacket must be trivially copyable");
    TEST_ASSERT(std::is_trivially_copyable_v<DiscoverAckPacket>, "DiscoverAckPacket must be trivially copyable");

    return true;
}

bool Test_Crypto_RandomAndHexConversion() {
    uint8_t key1[KEY_SIZE];
    uint8_t key2[KEY_SIZE];
    TEST_ASSERT(GenerateKey(key1), "GenerateKey should succeed");
    TEST_ASSERT(GenerateKey(key2), "GenerateKey should succeed");

    // Keys should not be identical (randomness check)
    TEST_ASSERT(std::memcmp(key1, key2, KEY_SIZE) != 0, "Two random keys must not be identical");

    // Hex conversion round-trip
    std::string hex = KeyToHex(key1);
    TEST_ASSERT(hex.length() == 64, "Key hex string must be 64 characters");

    uint8_t parsed_key[KEY_SIZE];
    TEST_ASSERT(HexToKey(hex, parsed_key), "HexToKey should succeed for valid hex string");
    TEST_ASSERT(std::memcmp(key1, parsed_key, KEY_SIZE) == 0, "Parsed key must match original key");

    // Invalid hex tests
    uint8_t dummy[KEY_SIZE];
    TEST_ASSERT(!HexToKey("invalid_short", dummy), "HexToKey should reject short string");
    TEST_ASSERT(!HexToKey(hex + "00", dummy), "HexToKey should reject long string");

    std::string bad_chars = hex;
    bad_chars[0] = 'z';
    TEST_ASSERT(!HexToKey(bad_chars, dummy), "HexToKey should reject non-hex characters");

    // Curve25519 Public Key Derivation test
    uint8_t priv_key[KEY_SIZE];
    uint8_t pub_key1[KEY_SIZE];
    uint8_t pub_key2[KEY_SIZE];
    TEST_ASSERT(GenerateKey(priv_key), "GenerateKey should succeed");
    TEST_ASSERT(DerivePublicKey(priv_key, pub_key1), "DerivePublicKey should succeed");
    TEST_ASSERT(DerivePublicKey(priv_key, pub_key2), "DerivePublicKey should succeed second time");
    TEST_ASSERT(std::memcmp(pub_key1, pub_key2, KEY_SIZE) == 0, "DerivePublicKey must be deterministic");
    TEST_ASSERT(std::memcmp(priv_key, pub_key1, KEY_SIZE) != 0, "Public key must differ from private key");

    return true;
}

bool Test_Crypto_AeadRoundtripAndTamperRejection() {
    uint8_t key[KEY_SIZE];
    uint8_t wrong_key[KEY_SIZE];
    uint8_t nonce[NONCE_SIZE];

    TEST_ASSERT(GenerateKey(key), "Key gen failed");
    TEST_ASSERT(GenerateKey(wrong_key), "Wrong key gen failed");
    TEST_ASSERT(GenerateNonce(nonce), "Nonce gen failed");

    const char* ad = "rig1";
    const char* message = "BareStats Distributed AI Inference Telemetry Payload Test String";
    size_t ad_len = std::strlen(ad);
    size_t msg_len = std::strlen(message);

    std::vector<uint8_t> ciphertext(msg_len);
    uint8_t mac[MAC_SIZE];

    // Encrypt
    EncryptAead(key, nonce, reinterpret_cast<const uint8_t*>(ad), ad_len,
                reinterpret_cast<const uint8_t*>(message), msg_len,
                ciphertext.data(), mac);

    // Decrypt successfully
    std::vector<uint8_t> decrypted(msg_len);
    bool ok = DecryptAead(key, nonce, mac, reinterpret_cast<const uint8_t*>(ad), ad_len,
                          ciphertext.data(), msg_len, decrypted.data());
    TEST_ASSERT(ok, "Valid decryption must succeed");
    TEST_ASSERT(std::memcmp(message, decrypted.data(), msg_len) == 0, "Decrypted message must match original");

    // Tamper 1: Wrong key
    ok = DecryptAead(wrong_key, nonce, mac, reinterpret_cast<const uint8_t*>(ad), ad_len,
                     ciphertext.data(), msg_len, decrypted.data());
    TEST_ASSERT(!ok, "Decryption with wrong key must fail");

    // Tamper 2: Corrupted ciphertext bit
    std::vector<uint8_t> corrupt_cipher = ciphertext;
    corrupt_cipher[msg_len / 2] ^= 0x01;
    ok = DecryptAead(key, nonce, mac, reinterpret_cast<const uint8_t*>(ad), ad_len,
                     corrupt_cipher.data(), msg_len, decrypted.data());
    TEST_ASSERT(!ok, "Decryption with corrupted ciphertext must fail");

    // Tamper 3: Corrupted nonce bit
    uint8_t corrupt_nonce[NONCE_SIZE];
    std::memcpy(corrupt_nonce, nonce, NONCE_SIZE);
    corrupt_nonce[0] ^= 0x01;
    ok = DecryptAead(key, corrupt_nonce, mac, reinterpret_cast<const uint8_t*>(ad), ad_len,
                     ciphertext.data(), msg_len, decrypted.data());
    TEST_ASSERT(!ok, "Decryption with corrupted nonce must fail");

    // Tamper 4: Corrupted MAC tag bit
    uint8_t corrupt_mac[MAC_SIZE];
    std::memcpy(corrupt_mac, mac, MAC_SIZE);
    corrupt_mac[0] ^= 0x01;
    ok = DecryptAead(key, nonce, corrupt_mac, reinterpret_cast<const uint8_t*>(ad), ad_len,
                     ciphertext.data(), msg_len, decrypted.data());
    TEST_ASSERT(!ok, "Decryption with corrupted MAC must fail");

    // Tamper 5: Corrupted Associated Data (rig_id altered)
    const char* spoofed_ad = "rig2";
    ok = DecryptAead(key, nonce, mac, reinterpret_cast<const uint8_t*>(spoofed_ad), std::strlen(spoofed_ad),
                     ciphertext.data(), msg_len, decrypted.data());
    TEST_ASSERT(!ok, "Decryption with spoofed Associated Data must fail");

    return true;
}

bool Test_WireProtocol_PackAndUnpack() {
    uint8_t private_key[KEY_SIZE];
    TEST_ASSERT(GenerateKey(private_key), "Private key gen failed");
    uint8_t public_key[KEY_SIZE];
    TEST_ASSERT(DerivePublicKey(private_key, public_key), "Public key derivation failed");

    uint8_t wrong_private_key[KEY_SIZE];
    TEST_ASSERT(GenerateKey(wrong_private_key), "Wrong private key gen failed");

    SatellitePayload payload{};
    std::memcpy(payload.magic, WIRE_MAGIC, sizeof(WIRE_MAGIC));
    payload.version = WIRE_VERSION;
    std::strncpy(payload.rig_id, "rig1", sizeof(payload.rig_id) - 1);
    payload.sequence_num = 1729384729182374ULL;
    payload.session_id = 42;
    payload.packet_counter = 1;
    payload.cpu_load_pct = 45;
    payload.sys_flags = SYS_FLAG_UMA;
    payload.ram_used_mb = 16384;
    payload.ram_total_mb = 65536;

    // GPUs
    payload.gpu_count = 2;
    payload.gpus[0].vram_used_mb = 12000;
    payload.gpus[0].vram_total_mb = 24576;
    payload.gpus[0].power_w = 285;
    payload.gpus[0].fan_rpm = 1800;
    payload.gpus[0].utilization_pct = 98;
    payload.gpus[0].temp_c = 68;

    payload.gpus[1].vram_used_mb = 8192;
    payload.gpus[1].vram_total_mb = 24576;
    payload.gpus[1].power_w = 195;
    payload.gpus[1].fan_rpm = 1500;
    payload.gpus[1].utilization_pct = 75;
    payload.gpus[1].temp_c = 62;

    // LLMs
    payload.llm_count = 1;
    std::strncpy(payload.llms[0].model_name, "DeepSeek-R1..14B", sizeof(payload.llms[0].model_name) - 1);
    std::strncpy(payload.llms[0].quant, "Q4_K_M", sizeof(payload.llms[0].quant) - 1);
    payload.llms[0].state = LlmState::Generating;
    payload.llms[0].tps = 34.25f;
    payload.llms[0].kv_cache_pct = 42;
    payload.llms[0].queue_running = 1;
    payload.llms[0].queue_waiting = 0;
    payload.llms[0].vram_used_mb = 11450;

    // Pack and encrypt with recipient public key
    SatelliteWirePacket packet{};
    TEST_ASSERT(PackAndEncrypt(public_key, payload, packet), "PackAndEncrypt must succeed");
    TEST_ASSERT(std::strcmp(packet.rig_id, "rig1") == 0, "Outer header rig_id must be populated");

    // Decrypt and unpack with recipient private key
    SatellitePayload unpacked{};
    TEST_ASSERT(DecryptAndUnpack(private_key, packet, unpacked), "DecryptAndUnpack must succeed");

    // Decrypting with wrong private key must fail
    SatellitePayload wrong_unpacked{};
    TEST_ASSERT(!DecryptAndUnpack(wrong_private_key, packet, wrong_unpacked), "DecryptAndUnpack with wrong private key must fail");

    // Verify fields
    TEST_ASSERT(std::memcmp(unpacked.magic, WIRE_MAGIC, sizeof(WIRE_MAGIC)) == 0, "Magic must match");
    TEST_ASSERT(unpacked.version == WIRE_VERSION, "Version must match");
    TEST_ASSERT(std::strcmp(unpacked.rig_id, "rig1") == 0, "Rig ID must match");
    TEST_ASSERT(unpacked.sequence_num == 1729384729182374ULL, "Sequence num must match");
    TEST_ASSERT(unpacked.session_id == 42, "Session ID must match");
    TEST_ASSERT(unpacked.packet_counter == 1, "Packet counter must match");
    TEST_ASSERT(unpacked.cpu_load_pct == 45, "CPU load must match");
    TEST_ASSERT(unpacked.sys_flags == SYS_FLAG_UMA, "Sys flags must match");
    TEST_ASSERT(unpacked.ram_used_mb == 16384, "RAM used must match");
    TEST_ASSERT(unpacked.gpu_count == 2, "GPU count must match");

    TEST_ASSERT(unpacked.gpus[0].vram_used_mb == 12000, "GPU 0 VRAM must match");
    TEST_ASSERT(unpacked.gpus[0].temp_c == 68, "GPU 0 Temp must match");
    TEST_ASSERT(unpacked.gpus[1].power_w == 195, "GPU 1 Power must match");

    TEST_ASSERT(unpacked.llm_count == 1, "LLM count must match");
    TEST_ASSERT(std::strcmp(unpacked.llms[0].model_name, "DeepSeek-R1..14B") == 0, "Model name must match");
    TEST_ASSERT(std::strcmp(unpacked.llms[0].quant, "Q4_K_M") == 0, "Quant must match");
    TEST_ASSERT(unpacked.llms[0].state == LlmState::Generating, "State must match");
    TEST_ASSERT(std::abs(unpacked.llms[0].tps - 34.25f) < 0.001f, "TPS must match");

    return true;
}

bool Test_WireProtocol_ReplaySlidingWindow() {
    RigReplayState replay{};

    SatellitePayload p{};
    p.session_id = 100;
    p.sequence_num = 1000;
    p.packet_counter = 1;

    // First packet in session: must accept
    TEST_ASSERT(replay.ValidateAndAccept(p), "First packet must be accepted");

    // Next sequential packet: must accept
    p.packet_counter = 2;
    p.sequence_num = 2000;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Packet 2 must be accepted");

    // Replay packet 2: must reject duplicate
    TEST_ASSERT(!replay.ValidateAndAccept(p), "Duplicate packet 2 must be rejected");

    // Replay packet 1: must reject duplicate
    p.packet_counter = 1;
    TEST_ASSERT(!replay.ValidateAndAccept(p), "Duplicate packet 1 must be rejected");

    // Out-of-order jump: packet 5 arrives
    p.packet_counter = 5;
    p.sequence_num = 5000;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Packet 5 must be accepted");

    // Delayed packet 3 arrives: within 64-packet window, must accept!
    p.packet_counter = 3;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Delayed packet 3 must be accepted");

    // Replay packet 3: must reject
    TEST_ASSERT(!replay.ValidateAndAccept(p), "Duplicate delayed packet 3 must be rejected");

    // Delayed packet 4 arrives: within window, must accept!
    p.packet_counter = 4;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Delayed packet 4 must be accepted");

    // Stale packet test: advance by 70 packets
    p.packet_counter = 75;
    p.sequence_num = 75000;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Packet 75 must be accepted");

    // Now packet 5 is diff = 70 (> 64), must be rejected as stale!
    p.packet_counter = 5;
    TEST_ASSERT(!replay.ValidateAndAccept(p), "Packet 5 must now be rejected as stale");

    // Reboot / Session Reset: new session_id with newer timestamp must be accepted
    p.session_id = 101;
    p.sequence_num = 80000;
    p.packet_counter = 1;
    TEST_ASSERT(replay.ValidateAndAccept(p), "Rebooted session must be accepted");

    return true;
}

bool Test_WireProtocol_WallClockFreshness() {
    Wire::RigReplayState replay{};
    Wire::SatellitePayload p{};
    p.session_id = 200;
    p.packet_counter = 1;

    uint64_t current_time_us = 1'000'000'000'000ULL; // arbitrary current time

    // Case 1: Fresh packet (within 5 seconds) -> must accept
    p.sequence_num = current_time_us - 5'000'000ULL;
    TEST_ASSERT(replay.ValidateAndAccept(p, current_time_us), "Fresh packet within 5s must be accepted");

    // Case 2: Stale packet (301 seconds old, 301,000,000 us) -> must reject
    p.packet_counter = 2;
    p.sequence_num = current_time_us - 301'000'000ULL;
    TEST_ASSERT(!replay.ValidateAndAccept(p, current_time_us), "Stale packet > 300s old must be rejected");

    // Case 3: Far future packet (301 seconds in future) -> must reject
    p.packet_counter = 3;
    p.sequence_num = current_time_us + 301'000'000ULL;
    TEST_ASSERT(!replay.ValidateAndAccept(p, current_time_us), "Far future packet > 300s ahead must be rejected");

    return true;
}

// VULN-01: Session reset replay monotonicity regression test
bool Test_WireProtocol_ReplaySessionMonotonicity() {
    Wire::RigReplayState replay{};
    Wire::SatellitePayload p1{};

    // Session 100 at timestamp 50s (50,000,000 us)
    p1.session_id = 100;
    p1.packet_counter = 50;
    p1.sequence_num = 50'000'000ULL;
    uint64_t current_time_us = 50'000'000ULL;

    TEST_ASSERT(replay.ValidateAndAccept(p1, current_time_us), "Initial packet in session 100 must be accepted");

    // An attacker replays an older captured packet from session 99 with timestamp 10s (10,000,000 us).
    // Delta from current_time is 40s (within +/- 300s wall-clock freshness window).
    Wire::SatellitePayload stale_p{};
    stale_p.session_id = 99;
    stale_p.packet_counter = 10;
    stale_p.sequence_num = 10'000'000ULL;

    // Secure requirement: Session reset must enforce sequence monotonicity (sequence_num > last_seq_us).
    // Pre-patch failure: returns true (vulnerable: session reset unconditionally resets last_seq_us).
    // Post-patch pass: returns false (stale session replay rejected).
    bool accepted = replay.ValidateAndAccept(stale_p, current_time_us);
    TEST_ASSERT(!accepted, "Session reset must enforce sequence monotonicity (last_seq_us check)");

    return true;
}

// BS-VULN-04: Cap sliding window counter advance step within a session
bool Test_WireProtocol_CapCounterAdvanceStep() {
    Wire::RigReplayState replay{};
    Wire::SatellitePayload p{};
    p.session_id = 42;
    p.packet_counter = 10;
    p.sequence_num = 100'000'000ULL;
    uint64_t current_time_us = 100'000'000ULL;

    TEST_ASSERT(replay.ValidateAndAccept(p, current_time_us), "Initial packet must be accepted");

    // Normal advance of 50 packets: counter becomes 60
    p.packet_counter = 60;
    p.sequence_num = 105'000'000ULL;
    TEST_ASSERT(replay.ValidateAndAccept(p, current_time_us + 5'000'000ULL), "Advance of 50 packets must be accepted");

    // Massive jump to 0xFFFFFFFF: should be rejected to prevent sliding window exhaustion
    Wire::SatellitePayload attack_p = p;
    attack_p.packet_counter = 0xFFFFFFFF;
    attack_p.sequence_num = 106'000'000ULL;
    TEST_ASSERT(!replay.ValidateAndAccept(attack_p, current_time_us + 6'000'000ULL), "Massive counter jump must be rejected");

    // Legitimate next packet (counter 61) should still be accepted because attack_p did not advance last_counter
    Wire::SatellitePayload legit_next = p;
    legit_next.packet_counter = 61;
    legit_next.sequence_num = 106'000'000ULL;
    TEST_ASSERT(replay.ValidateAndAccept(legit_next, current_time_us + 6'000'000ULL), "Legitimate packet 61 must still be accepted");

    return true;
}

// AUDIT-P1-01: Low-order / all-zero shared secret rejection across all crypto functions
bool Test_Crypto_RejectLowOrderSharedSecrets() {
    uint8_t zero_key[KEY_SIZE] = {0};
    uint8_t valid_priv[KEY_SIZE] = {0};
    uint8_t valid_pub[KEY_SIZE] = {0};
    TEST_ASSERT(GenerateKey(valid_priv), "GenerateKey failed");
    TEST_ASSERT(DerivePublicKey(valid_priv, valid_pub), "DerivePublicKey failed");

    SatellitePayload payload{};
    std::memcpy(payload.magic, WIRE_MAGIC, sizeof(WIRE_MAGIC));
    payload.version = WIRE_VERSION;
    std::strncpy(payload.rig_id, "rig_test", sizeof(payload.rig_id) - 1);
    payload.session_id = 1;
    payload.packet_counter = 1;

    // 1. PackAndEncrypt with all-zero public key
    SatelliteWirePacket wire_pkt{};
    bool pack_zero = PackAndEncrypt(zero_key, payload, wire_pkt);
    TEST_ASSERT(!pack_zero, "PackAndEncrypt must reject low-order / all-zero public key");

    // 2. DecryptAndUnpack with all-zero ephemeral public key
    TEST_ASSERT(PackAndEncrypt(valid_pub, payload, wire_pkt), "Pack with valid pubkey failed");
    std::memset(wire_pkt.eph_pk, 0, KEY_SIZE); // corrupt eph_pk to all-zero
    SatellitePayload out_payload{};
    bool unpack_zero = DecryptAndUnpack(valid_priv, wire_pkt, out_payload);
    TEST_ASSERT(!unpack_zero, "DecryptAndUnpack must reject all-zero eph_pk");

    // 3. PackDiscover with all-zero target public key
    DiscoverPacket disc_pkt{};
    uint8_t eph_sk[KEY_SIZE] = {0};
    bool pack_disc_zero = PackDiscover(zero_key, "rig_test", 12345, disc_pkt, eph_sk);
    TEST_ASSERT(!pack_disc_zero, "PackDiscover must reject all-zero target public key");

    // 4. VerifyDiscover with all-zero eph_pk
    std::string out_rig;
    uint64_t out_trans = 0;
    TEST_ASSERT(PackDiscover(valid_pub, "rig_test", 12345, disc_pkt, eph_sk), "PackDiscover valid failed");
    std::memset(disc_pkt.eph_pk, 0, KEY_SIZE);
    bool verify_disc_zero = VerifyDiscover(valid_priv, disc_pkt, out_rig, out_trans);
    TEST_ASSERT(!verify_disc_zero, "VerifyDiscover must reject all-zero eph_pk");

    // 5. PackDiscoverAck with all-zero sender eph_pk
    DiscoverAckPacket ack_pkt{};
    bool pack_ack_zero = PackDiscoverAck(valid_priv, "rig_test", 9889, 12345, ack_pkt, zero_key);
    TEST_ASSERT(!pack_ack_zero, "PackDiscoverAck must reject all-zero sender eph_pk");

    // 6. VerifyDiscoverAck with all-zero target public key
    TEST_ASSERT(PackDiscover(valid_pub, "rig_test", 12345, disc_pkt, eph_sk), "Repack discover");
    TEST_ASSERT(PackDiscoverAck(valid_priv, "rig_test", 9889, 12345, ack_pkt, disc_pkt.eph_pk), "PackDiscoverAck valid");
    uint16_t out_port = 0;
    bool verify_ack_zero = VerifyDiscoverAck(zero_key, ack_pkt, out_rig, out_port, out_trans, eph_sk);
    TEST_ASSERT(!verify_ack_zero, "VerifyDiscoverAck must reject all-zero target public key");

    // 7. VerifyDiscoverAck without sender_eph_sk (nullptr) must fail (invalid fallback removed)
    bool verify_ack_null_sk = VerifyDiscoverAck(valid_pub, ack_pkt, out_rig, out_port, out_trans, nullptr);
    TEST_ASSERT(!verify_ack_null_sk, "VerifyDiscoverAck must reject nullptr sender_eph_sk");

    return true;
}


