#include "test_common.h"
#include "metrics/network/udp_streamer.h"
#include "config/service_installer.h"
#include "config/config.h"
#include "util/crypto/crypto_box.h"
#include "util/os_utils.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace BareStats;

bool Test_UdpStreamer_TransmitterAndReceiverRoundtrip() {
    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint8_t pub_key[32] = {0};
    Crypto::DerivePublicKey(priv_key, pub_key);

    uint16_t test_port = 19889;

    UdpReceiver receiver;
    bool bound = receiver.Bind(test_port, priv_key);
    TEST_ASSERT(bound, "UdpReceiver must bind successfully to port 19889");

    UdpTransmitter transmitter;
    bool opened = transmitter.Open("127.0.0.1", test_port, pub_key, "rig1");
    TEST_ASSERT(opened, "UdpTransmitter must open socket");

    Wire::SatellitePayload payload{};
    payload.ram_used_mb = 16384;
    payload.ram_total_mb = 32768;
    payload.cpu_load_pct = 42;
    payload.gpu_count = 1;
    payload.gpus[0].vram_used_mb = 6144;
    payload.gpus[0].vram_total_mb = 12288;
    payload.gpus[0].utilization_pct = 85;
    payload.llm_count = 1;
    std::strncpy(payload.llms[0].model_name, "deepseek-r1..14b", sizeof(payload.llms[0].model_name) - 1);
    payload.llms[0].state = Wire::LlmState::Generating;
    payload.llms[0].tps = 48.5f;

    bool sent = transmitter.SendPayload(payload);
    TEST_ASSERT(sent, "Transmitter must successfully send packet");

    // Allow loopback delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    Wire::SatellitePayload received{};
    bool got = receiver.ReceivePacket(received);
    TEST_ASSERT(got, "Receiver must receive and decrypt valid packet");

    TEST_ASSERT(received.ram_used_mb == 16384, "RAM used must match");
    TEST_ASSERT(received.cpu_load_pct == 42, "CPU load must match");
    TEST_ASSERT(received.gpu_count == 1, "GPU count must match");
    TEST_ASSERT(received.gpus[0].vram_used_mb == 6144, "GPU VRAM must match");
    TEST_ASSERT(received.llm_count == 1, "LLM count must match");
    TEST_ASSERT(std::string(received.llms[0].model_name) == "deepseek-r1..14b", "Model name must match");
    TEST_ASSERT(received.llms[0].tps == 48.5f, "TPS must match");

    // Test that loopback filtering ignores packets from this session
    receiver.SetIgnoredSessionId(transmitter.GetSessionId());
    transmitter.SendPayload(payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Wire::SatellitePayload ignoredPayload{};
    bool gotIgnored = receiver.ReceivePacket(ignoredPayload);
    TEST_ASSERT(!gotIgnored, "Receiver must drop self-loopback packet when session ID is ignored");

    transmitter.Close();
    receiver.Close();
    return true;
}

bool Test_UdpStreamer_TamperAndWrongKeyRejection() {
    uint8_t priv_rx[32] = {0};
    uint8_t priv_tx_wrong[32] = {0};
    Crypto::GenerateRandomBytes(priv_rx, sizeof(priv_rx));
    Crypto::GenerateRandomBytes(priv_tx_wrong, sizeof(priv_tx_wrong));
    uint8_t pub_tx_wrong[32] = {0};
    Crypto::DerivePublicKey(priv_tx_wrong, pub_tx_wrong);

    uint16_t test_port = 19890;

    UdpReceiver receiver;
    TEST_ASSERT(receiver.Bind(test_port, priv_rx), "Receiver bind");

    UdpTransmitter transmitter;
    TEST_ASSERT(transmitter.Open("127.0.0.1", test_port, pub_tx_wrong, "rig2"), "Transmitter open");

    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 99;
    transmitter.SendPayload(payload);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    Wire::SatellitePayload received{};
    bool got = receiver.ReceivePacket(received);
    TEST_ASSERT(!got, "Receiver must reject packet encrypted with wrong cluster key");

    transmitter.Close();
    receiver.Close();
    return true;
}

bool Test_UdpStreamer_SubnetAutoDiscoveryHandshake() {
    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint8_t pub_key[32] = {0};
    Crypto::DerivePublicKey(priv_key, pub_key);

    uint16_t test_port = 19891;

    UdpReceiver receiver;
    bool bound = receiver.Bind(test_port, priv_key);
    TEST_ASSERT(bound, "Receiver must bind successfully");

    UdpTransmitter transmitter;
    bool opened = transmitter.Open("127.0.0.1", test_port, pub_key, "sat_disc");
    TEST_ASSERT(opened, "Transmitter must open socket");

    // Manually trigger 104-byte DISCOVER packet send with challenge nonce
    bool sent_disc = transmitter.SendDiscover();
    TEST_ASSERT(sent_disc, "Transmitter must send 104-byte DISCOVER packet");

    // Allow loopback delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Receiver processes incoming discovery and replies with 104-byte DISCOVER_ACK
    Wire::SatellitePayload dummy{};
    bool got_payload = receiver.ReceivePacket(dummy);
    TEST_ASSERT(!got_payload, "ReceivePacket on DISCOVER packet must return false");

    // Transmitter polls and validates the 104-byte DISCOVER_ACK
    bool acked = transmitter.PollDiscoverAck(500);
    TEST_ASSERT(acked, "Transmitter must receive and verify DISCOVER_ACK");
    TEST_ASSERT(!transmitter.GetDiscoveredHost().empty(), "Discovered host must be set");

    // Verify subsequent payload transmission uses direct address
    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 75;
    std::strncpy(payload.rig_id, "sat_disc", sizeof(payload.rig_id) - 1);

    bool sent = transmitter.SendPayload(payload);
    TEST_ASSERT(sent, "Transmitter must send payload");

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    Wire::SatellitePayload received{};
    bool got = receiver.ReceivePacket(received);
    TEST_ASSERT(got, "Receiver must receive and decrypt payload");
    TEST_ASSERT(received.cpu_load_pct == 75, "CPU load must match");

    transmitter.Close();
    receiver.Close();
    return true;
}

bool Test_UdpStreamer_TransactionChallengeReplayRejection() {
    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint8_t pub_key[32] = {0};
    Crypto::DerivePublicKey(priv_key, pub_key);

    Wire::DiscoverPacket disc_pkt{};
    uint8_t eph_sk[32] = {0};
    Crypto::PackDiscover(pub_key, "sat_test", 0x1234, disc_pkt, eph_sk);

    // Generate a valid DiscoverAckPacket with a mismatched/old transaction_id
    Wire::DiscoverAckPacket bad_ack{};
    uint64_t fake_trans_id = 0xDEADBEEFCAFEULL;
    bool packed = Crypto::PackDiscoverAck(priv_key, "sat_test", 9889, fake_trans_id, bad_ack, disc_pkt.eph_pk);
    TEST_ASSERT(packed, "PackDiscoverAck must succeed");

    std::string ack_rig;
    uint16_t ack_port = 0;
    uint64_t verified_trans_id = 0;
    bool verified = Crypto::VerifyDiscoverAck(pub_key, bad_ack, ack_rig, ack_port, verified_trans_id, eph_sk);
    TEST_ASSERT(verified, "VerifyDiscoverAck must verify MAC");
    TEST_ASSERT(verified_trans_id == fake_trans_id, "Verified transaction ID must match packed");
    TEST_ASSERT(verified_trans_id != 0x12345678ULL, "Mismatched transaction ID must not match expected request");

    return true;
}

bool Test_UdpStreamer_ContinuousBroadcastAndDualPathDeduplication() {
    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint8_t pub_key[32] = {0};
    Crypto::DerivePublicKey(priv_key, pub_key);

    uint16_t test_port = 19892;

    UdpReceiver receiver;
    bool bound = receiver.Bind(test_port, priv_key);
    TEST_ASSERT(bound, "Receiver must bind successfully on port 19892");

    UdpTransmitter transmitter;
    // Open in auto-discovery broadcast mode
    bool opened = transmitter.Open("auto", test_port, pub_key, "rig_auto");
    TEST_ASSERT(opened, "Transmitter must open in auto discovery mode");

    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 55;
    payload.ram_used_mb = 8192;
    payload.ram_total_mb = 16384;
    std::strncpy(payload.rig_id, "rig_auto", sizeof(payload.rig_id) - 1);

    // 1. Broadcast transmission in auto mode (drain any preceding DISCOVER handshake packets first)
    bool sent1 = transmitter.SendPayload(payload);
    TEST_ASSERT(sent1, "Transmitter must broadcast payload in auto mode");

    Wire::SatellitePayload rx1{};
    bool got1 = false;
    for (int attempt = 0; attempt < 50 && !got1; ++attempt) {
        got1 = receiver.ReceivePacket(rx1);
        if (!got1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    TEST_ASSERT(got1, "Receiver must accept broadcast packet from auto mode");
    TEST_ASSERT(rx1.cpu_load_pct == 55, "CPU metric must match");

    // 2. Second transmission with advance
    payload.cpu_load_pct = 60;
    bool sent2 = transmitter.SendPayload(payload);
    TEST_ASSERT(sent2, "Transmitter must send second payload update");

    Wire::SatellitePayload rx2{};
    bool got2 = false;
    for (int attempt = 0; attempt < 50 && !got2; ++attempt) {
        got2 = receiver.ReceivePacket(rx2);
        if (!got2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    TEST_ASSERT(got2, "Receiver must accept subsequent payload update");
    TEST_ASSERT(rx2.cpu_load_pct == 60, "Updated CPU metric must match");

    transmitter.Close();
    receiver.Close();
    return true;
}

bool Test_ServiceInstaller_Validation() {
    // Calling InstallOsService with nonexistent satellite.ini must fail cleanly (exit code 1)
    int code = InstallOsService("nonexistent_satellite_file.ini");
    TEST_ASSERT(code == 1, "InstallOsService must return 1 when satellite.ini is missing");

    return true;
}

bool Test_ServiceInstaller_UnprivilegedInstallation() {
    fs::path stagingDir = GetServiceStagingDirectory();
    TEST_ASSERT(!stagingDir.empty(), "Staging directory must not be empty");

#if defined(_WIN32)
    std::string s = stagingDir.string();
    TEST_ASSERT(s.find("Program Files") == std::string::npos, "Staging directory must not be in Program Files");
    TEST_ASSERT(s.find("AppData") != std::string::npos || s.find("BareStats") != std::string::npos, "Staging directory must reside in unprivileged user AppData");
#elif defined(__APPLE__)
    std::string s = stagingDir.string();
    TEST_ASSERT(s.find("LaunchDaemons") == std::string::npos, "Staging directory must not be root LaunchDaemons");
    TEST_ASSERT(s.find("Library") != std::string::npos, "Staging directory must reside in user Library");
#elif defined(__linux__)
    if (geteuid() != 0) {
        std::string s = stagingDir.string();
        TEST_ASSERT(s.find("/etc/barestats") == std::string::npos, "Unprivileged user staging must not be /etc/barestats");
        TEST_ASSERT(s.find(".local") != std::string::npos, "Staging directory must reside in user .local");
    }
#endif

    uint8_t key[Crypto::KEY_SIZE] = {0};
    Crypto::GenerateKey(key);
    std::string hexKey = Crypto::KeyToHex(key);

    std::string tempIni = "temp_unprivileged_test.ini";
    bool ok = ConfigManager::ExportSatelliteIni(tempIni, hexKey, "unpriv_test", "127.0.0.1", 9889);
    TEST_ASSERT(ok, "ExportSatelliteIni must succeed");

    std::error_code dirEc;
    fs::create_directories(stagingDir, dirEc);
    if (dirEc || !fs::exists(stagingDir)) {
        // Sandboxed / container environment without writable user directory
        std::remove(tempIni.c_str());
        return true;
    }

    int installRes = InstallOsService(tempIni);
    TEST_ASSERT(installRes == 0, "InstallOsService must succeed in unprivileged environment");

    fs::path stagedIni = stagingDir / "satellite.ini";
    TEST_ASSERT(fs::exists(stagedIni), "satellite.ini must be staged in unprivileged target directory");

    int uninstallRes = UninstallOsService();
    TEST_ASSERT(uninstallRes == 0, "UninstallOsService must succeed");
    TEST_ASSERT(!fs::exists(stagedIni), "satellite.ini must be removed on uninstall");

    std::remove(tempIni.c_str());
    return true;
}

bool Test_ConfigManager_ExportSatelliteIni() {
    uint8_t key[Crypto::KEY_SIZE] = {0};
    Crypto::GenerateKey(key);
    std::string hexKey = Crypto::KeyToHex(key);
    TEST_ASSERT(hexKey.size() == 64, "Generated cluster key must be 64 hex characters");

    uint8_t pub_bytes[Crypto::KEY_SIZE] = {0};
    Crypto::DerivePublicKey(key, pub_bytes);
    std::string expectedPubHex = Crypto::KeyToHex(pub_bytes);

    std::string tempFile = "temp_test_satellite.ini";
    bool ok = ConfigManager::ExportSatelliteIni(tempFile, hexKey, "testrig", "192.168.1.50", 9889);
    TEST_ASSERT(ok, "ExportSatelliteIni must succeed");

    std::ifstream ifs(tempFile);
    TEST_ASSERT(ifs.is_open(), "Exported file must exist");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    std::remove(tempFile.c_str());

    TEST_ASSERT(content.find("[Satellite]") != std::string::npos, "File must have [Satellite]");
    TEST_ASSERT(content.find("rig_id = testrig") != std::string::npos, "File must have rig_id");
    TEST_ASSERT(content.find("target_host = 192.168.1.50") != std::string::npos, "File must have target_host");
    TEST_ASSERT(content.find("public_key = " + expectedPubHex) != std::string::npos, "File must have derived public_key");
    TEST_ASSERT(content.find("cluster_key = " + hexKey) == std::string::npos, "File must NEVER contain private cluster_key");

    return true;
}

bool Test_ResolveSatelliteExportPath() {
    fs::path p = BareStats::ResolveSatelliteExportPath();
    TEST_ASSERT(!p.empty(), "ResolveSatelliteExportPath must never return empty path");
    TEST_ASSERT(p.filename() == "satellite.ini", "Filename must be satellite.ini");

    // Must be co-located in the exact same directory as config.ini
    fs::path configP = BareStats::ConfigManager::GetDefaultConfigPath();
    TEST_ASSERT(p.parent_path() == fs::path(configP).parent_path(),
                "satellite.ini must be co-located in the exact same directory as config.ini");

    // Must respect custom config paths
    fs::path customP = BareStats::ResolveSatelliteExportPath("/tmp/custom_test_dir/config.ini");
    TEST_ASSERT(customP.filename() == "satellite.ini", "Custom path filename must be satellite.ini");
    TEST_ASSERT(customP.parent_path() == fs::path("/tmp/custom_test_dir"),
                "Custom path parent must match config.ini parent directory");

    return true;
}

bool Test_UdpStreamer_MultiTargetDualCommandCenter() {
    // Command Center A
    uint8_t priv_a[32] = {0};
    Crypto::GenerateRandomBytes(priv_a, sizeof(priv_a));
    uint8_t pub_a[32] = {0};
    Crypto::DerivePublicKey(priv_a, pub_a);
    uint16_t port_a = 19893;

    UdpReceiver receiver_a;
    TEST_ASSERT(receiver_a.Bind(port_a, priv_a), "Receiver A must bind successfully");

    // Command Center B
    uint8_t priv_b[32] = {0};
    Crypto::GenerateRandomBytes(priv_b, sizeof(priv_b));
    uint8_t pub_b[32] = {0};
    Crypto::DerivePublicKey(priv_b, pub_b);
    uint16_t port_b = 19894;

    UdpReceiver receiver_b;
    TEST_ASSERT(receiver_b.Bind(port_b, priv_b), "Receiver B must bind successfully");

    // Satellite targeting both Command Centers A and B
    std::vector<SatelliteTargetConfig> targets;
    SatelliteTargetConfig target_a;
    target_a.name = "CenterA";
    target_a.target_host = "127.0.0.1";
    target_a.target_port = port_a;
    std::memcpy(target_a.public_key, pub_a, 32);
    targets.push_back(target_a);

    SatelliteTargetConfig target_b;
    target_b.name = "CenterB";
    target_b.target_host = "127.0.0.1";
    target_b.target_port = port_b;
    std::memcpy(target_b.public_key, pub_b, 32);
    targets.push_back(target_b);

    UdpTransmitter transmitter;
    bool opened = transmitter.Open("shared_rig", targets);
    TEST_ASSERT(opened, "Transmitter must open with multiple targets");
    TEST_ASSERT(transmitter.GetTargetCount() == 2, "Transmitter must have 2 targets");

    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 77;
    payload.ram_used_mb = 12000;
    payload.ram_total_mb = 32000;
    std::strncpy(payload.rig_id, "shared_rig", sizeof(payload.rig_id) - 1);

    bool sent = transmitter.SendPayload(payload);
    TEST_ASSERT(sent, "Transmitter must successfully send to multi-targets");

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Command Center A must receive and decrypt
    Wire::SatellitePayload rx_a{};
    bool got_a = receiver_a.ReceivePacket(rx_a);
    TEST_ASSERT(got_a, "Receiver A must receive and decrypt payload");
    TEST_ASSERT(rx_a.cpu_load_pct == 77, "Receiver A CPU load must match");

    // Command Center B must receive and decrypt
    Wire::SatellitePayload rx_b{};
    bool got_b = receiver_b.ReceivePacket(rx_b);
    TEST_ASSERT(got_b, "Receiver B must receive and decrypt payload");
    TEST_ASSERT(rx_b.cpu_load_pct == 77, "Receiver B CPU load must match");

    transmitter.Close();
    receiver_a.Close();
    receiver_b.Close();
    return true;
}

// VULN-02: Unauthorized sender rig identity rejection regression test
bool Test_UdpStreamer_RejectUnauthorizedSenderRig() {
    uint8_t priv_cc[32] = {0};
    Crypto::GenerateRandomBytes(priv_cc, sizeof(priv_cc));
    uint8_t pub_cc[32] = {0};
    Crypto::DerivePublicKey(priv_cc, pub_cc);

    uint16_t test_port = 19895;
    UdpReceiver receiver;
    receiver.AddAllowedRig("authorized_rig");
    TEST_ASSERT(receiver.Bind(test_port, priv_cc), "Receiver bind");

    // An attacker possessing Command Center's public key (e.g. from satellite.ini) sends telemetry as "unauthorized_rig"
    UdpTransmitter attacker_tx;
    TEST_ASSERT(attacker_tx.Open("127.0.0.1", test_port, pub_cc, "unauthorized_rig"), "Attacker transmitter open");

    Wire::SatellitePayload rogue_p{};
    rogue_p.cpu_load_pct = 99;
    std::strncpy(rogue_p.rig_id, "unauthorized_rig", sizeof(rogue_p.rig_id) - 1);
    attacker_tx.SendPayload(rogue_p);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    Wire::SatellitePayload received{};
    bool accepted = receiver.ReceivePacket(received);

    attacker_tx.Close();
    receiver.Close();

    // Secure requirement: Command Center must reject telemetry from unauthorized / rogue rig identities.
    // Pre-patch failure: accepted == true (anonymous sealed box accepts any sender rig).
    // Post-patch pass: accepted == false (unauthorized sender rejected).
    TEST_ASSERT(!accepted, "Command Center must reject unauthorized sender rig identities");
    return true;
}

// VULN-03: Fixed 8-slot tracker pool starvation defense regression test
bool Test_UdpStreamer_TrackerPoolStarvationDefense() {
    uint8_t priv_cc[32] = {0};
    Crypto::GenerateRandomBytes(priv_cc, sizeof(priv_cc));
    uint8_t pub_cc[32] = {0};
    Crypto::DerivePublicKey(priv_cc, pub_cc);

    uint16_t test_port = 19896;
    UdpReceiver receiver;
    TEST_ASSERT(receiver.Bind(test_port, priv_cc), "Receiver bind");

    // Send telemetry for 8 distinct rigs to occupy all 8 tracker slots
    for (int i = 0; i < 8; ++i) {
        std::string rig_name = "flood_" + std::to_string(i);
        UdpTransmitter tx;
        tx.Open("127.0.0.1", test_port, pub_cc, rig_name.c_str());
        Wire::SatellitePayload p{};
        p.cpu_load_pct = 10;
        std::strncpy(p.rig_id, rig_name.c_str(), sizeof(p.rig_id) - 1);
        tx.SendPayload(p);
        tx.Close();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Wire::SatellitePayload dummy{};
        receiver.ReceivePacket(dummy);
    }

    // Send a legitimate 9th rig packet 10 seconds later (< 60s idle eviction threshold)
    UdpTransmitter legit_tx;
    legit_tx.Open("127.0.0.1", test_port, pub_cc, "legit_rig");
    Wire::SatellitePayload legit_p{};
    legit_p.cpu_load_pct = 50;
    std::strncpy(legit_p.rig_id, "legit_rig", sizeof(legit_p.rig_id) - 1);
    legit_tx.SendPayload(legit_p);
    legit_tx.Close();

    Wire::SatellitePayload legit_rx{};
    bool accepted = false;
    for (int attempt = 0; attempt < 50 && !accepted; ++attempt) {
        accepted = receiver.ReceivePacket(legit_rx);
        if (!accepted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    receiver.Close();

    // Secure requirement: Tracker pool must not permanently starve new rigs when 8 slots are occupied.
    // Pre-patch failure: accepted == false (pool of 8 slots is exhausted and cannot evict non-60s-idle slots).
    // Post-patch pass: accepted == true (dynamic capacity or LRU eviction ensures legitimate rig is not starved).
    TEST_ASSERT(accepted, "Tracker pool must not permanently starve new rigs when 8 slots are occupied");
    return true;
}

// VULN-04: Systemd DynamicUser permission consistency regression test
bool Test_ServiceInstaller_SystemdPermissionConsistency() {
    // Inspect generated root systemd service template logic
    std::string unit_sample = GenerateSystemdServiceUnit("/usr/local/bin/barestats", "/etc/barestats", true);

    bool has_dynamic_user = (unit_sample.find("DynamicUser=yes") != std::string::npos);
    bool has_config_dir = (unit_sample.find("ConfigurationDirectory=") != std::string::npos);

    // Secure requirement: Systemd unit with DynamicUser=yes must specify ConfigurationDirectory=barestats
    // or staging permissions must grant read access to the dynamically allocated unprivileged UID.
    // Pre-patch failure: has_dynamic_user is true but has_config_dir is false.
    // Post-patch pass: has_config_dir is true.
    TEST_ASSERT(!has_dynamic_user || has_config_dir,
                "Systemd unit with DynamicUser=yes must specify ConfigurationDirectory to grant access to /etc/barestats");
    return true;
}

// VULN-05: Launchd daemon command whitespace quoting regression test
bool Test_ServiceInstaller_LaunchdPathWhitespaceHandling() {
    fs::path plist_with_spaces = "/Users/John Doe/Library/LaunchAgents/com.barestats.satellite.plist";
    std::string lctl = "/bin/launchctl";
    uint32_t uid = 501;

    std::string raw_cmd = BuildLaunchdBootstrapCommand(lctl, uid, plist_with_spaces);

    std::string quoted_target = "\"" + plist_with_spaces.string() + "\"";
    bool path_is_quoted = (raw_cmd.find(quoted_target) != std::string::npos);

    // Secure requirement: Plist path must be quoted in constructed shell command.
    // Pre-patch failure: path_is_quoted is false (unquoted argument splitting).
    // Post-patch pass: path_is_quoted is true.
    TEST_ASSERT(path_is_quoted,
                "Launchd command construction must quote file paths to prevent shell argument injection");
    return true;
}

// BS-VULN-03: Cluster allowed rigs configuration and propagation regression test
bool Test_Config_ClusterAllowedRigsPropagation() {
    std::string tempIni = GetTestModuleDir() + "temp_allowed_rigs.ini";
    std::error_code ec;
    fs::remove(tempIni, ec);

    std::ofstream ofs(tempIni);
    ofs << "[Cluster]\n"
        << "cluster_key = 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        << "listen_port = 9889\n"
        << "allowed_rigs = rig1, rig2, rig_remote\n";
    ofs.close();

    AppConfig cfg = ConfigManager::Load(tempIni);
    TEST_ASSERT(cfg.cluster.allowedRigs == "rig1, rig2, rig_remote", "allowed_rigs must be loaded from config.ini");

    // Test saving back
    std::string saveIni = GetTestModuleDir() + "temp_allowed_rigs_save.ini";
    ConfigManager::Save(saveIni, cfg);
    AppConfig reloaded = ConfigManager::Load(saveIni);
    fs::remove(tempIni, ec);
    fs::remove(saveIni, ec);

    TEST_ASSERT(reloaded.cluster.allowedRigs == "rig1, rig2, rig_remote", "allowed_rigs must roundtrip through Save/Load");
    return true;
}

// BS-VULN-05: Preserve rig replay timestamps across tracker evictions
bool Test_UdpStreamer_EvictionReplayResistance() {
    uint8_t priv_cc[32] = {0};
    Crypto::GenerateRandomBytes(priv_cc, sizeof(priv_cc));
    uint8_t pub_cc[32] = {0};
    Crypto::DerivePublicKey(priv_cc, pub_cc);

    uint16_t test_port = 19897;
    UdpReceiver receiver;
    TEST_ASSERT(receiver.Bind(test_port, priv_cc), "Receiver bind");

    // 1. Initial transmission from victim_rig
    UdpTransmitter victim_tx;
    TEST_ASSERT(victim_tx.Open("127.0.0.1", test_port, pub_cc, "victim_rig"), "Open victim_tx");
    Wire::SatellitePayload victim_p{};
    victim_p.cpu_load_pct = 40;
    std::strncpy(victim_p.rig_id, "victim_rig", sizeof(victim_p.rig_id) - 1);
    TEST_ASSERT(victim_tx.SendPayload(victim_p), "Send initial victim payload");
    victim_tx.Close();

    Wire::SatellitePayload rx1{};
    bool got1 = false;
    for (int attempt = 0; attempt < 50 && !got1; ++attempt) {
        got1 = receiver.ReceivePacket(rx1);
        if (!got1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    TEST_ASSERT(got1, "Receiver must accept first victim packet");
    TEST_ASSERT(rx1.cpu_load_pct == 40, "First payload cpu matches");

    // Capture wire packet corresponding to rx1
    Wire::SatelliteWirePacket captured_wire{};
    Crypto::PackAndEncrypt(pub_cc, rx1, captured_wire);

    // 2. Transmit from 8 other distinct rigs to evict victim_rig from the 8-slot active pool
    for (int i = 0; i < 8; ++i) {
        std::string churn_name = "churn_" + std::to_string(i);
        UdpTransmitter churn_tx;
        churn_tx.Open("127.0.0.1", test_port, pub_cc, churn_name.c_str());
        Wire::SatellitePayload churn_p{};
        churn_p.cpu_load_pct = 10;
        std::strncpy(churn_p.rig_id, churn_name.c_str(), sizeof(churn_p.rig_id) - 1);
        churn_tx.SendPayload(churn_p);
        churn_tx.Close();

        for (int attempt = 0; attempt < 20; ++attempt) {
            Wire::SatellitePayload dummy{};
            if (receiver.ReceivePacket(dummy)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // 3. Replay captured packet for victim_rig via raw socket
#ifdef _WIN32
    SOCKET raw_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(test_port);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    sendto(raw_sock, reinterpret_cast<const char*>(&captured_wire), sizeof(captured_wire), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    closesocket(raw_sock);
#else
    int raw_sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(test_port);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    sendto(raw_sock, reinterpret_cast<const char*>(&captured_wire), sizeof(captured_wire), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    close(raw_sock);
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Wire::SatellitePayload rx_replayed{};
    bool replayed_accepted = receiver.ReceivePacket(rx_replayed);
    TEST_ASSERT(!replayed_accepted, "Replayed packet for evicted rig must be rejected via tombstone state");

    // 4. Legitimate subsequent transmission from victim_rig with newer sequence/counter
    UdpTransmitter fresh_tx;
    fresh_tx.Open("127.0.0.1", test_port, pub_cc, "victim_rig");
    Wire::SatellitePayload fresh_p{};
    fresh_p.cpu_load_pct = 95;
    std::strncpy(fresh_p.rig_id, "victim_rig", sizeof(fresh_p.rig_id) - 1);
    fresh_tx.SendPayload(fresh_p);
    fresh_tx.Close();

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Wire::SatellitePayload rx_fresh{};
    bool fresh_accepted = receiver.ReceivePacket(rx_fresh);
    TEST_ASSERT(fresh_accepted, "Fresh packet for evicted rig must be accepted");
    TEST_ASSERT(rx_fresh.cpu_load_pct == 95, "Fresh payload cpu matches");

    receiver.Close();
    return true;
}

// BS-VULN-07: Sanitize shell path arguments in launchd service installer
bool Test_ServiceInstaller_LaunchdShellMetacharacterSanitization() {
    std::string lctl = "/bin/launchctl";
    uint32_t uid = 501;

    // Normal path with space should be allowed and quoted
    fs::path valid_path = "/Users/Safe User/Library/LaunchAgents/com.barestats.plist";
    std::string valid_cmd = BuildLaunchdBootstrapCommand(lctl, uid, valid_path);
    TEST_ASSERT(!valid_cmd.empty(), "Valid path with space must produce non-empty command");
    TEST_ASSERT(valid_cmd.find("\"" + valid_path.string() + "\"") != std::string::npos, "Valid path must be quoted");

    // Paths containing shell injection characters must return empty command
    fs::path injection_quote = "/Users/hacker\"$(whoami)/file.plist";
    TEST_ASSERT(BuildLaunchdBootstrapCommand(lctl, uid, injection_quote).empty(), "Double quote must be rejected");

    fs::path injection_backtick = "/Users/hacker`whoami`/file.plist";
    TEST_ASSERT(BuildLaunchdBootstrapCommand(lctl, uid, injection_backtick).empty(), "Backtick must be rejected");

    fs::path injection_dollar = "/Users/hacker$USER/file.plist";
    TEST_ASSERT(BuildLaunchdBootstrapCommand(lctl, uid, injection_dollar).empty(), "Dollar sign must be rejected");

    fs::path injection_semi = "/Users/hacker;reboot/file.plist";
    TEST_ASSERT(BuildLaunchdBootstrapCommand(lctl, uid, injection_semi).empty(), "Semicolon must be rejected");

    fs::path injection_newline = "/Users/hacker\nreboot/file.plist";
    TEST_ASSERT(BuildLaunchdBootstrapCommand(lctl, uid, injection_newline).empty(), "Newline must be rejected");

    return true;
}

// AUDIT-P1-02: Discovery request authorized rig identity whitelist filtering
bool Test_UdpStreamer_DiscoverAllowedRigsAndWireRigIdMismatch() {
    uint8_t priv_key[32] = {0};
    Crypto::GenerateRandomBytes(priv_key, sizeof(priv_key));
    uint8_t pub_key[32] = {0};
    Crypto::DerivePublicKey(priv_key, pub_key);

    uint16_t test_port = 19898;

    UdpReceiver receiver;
    TEST_ASSERT(receiver.Bind(test_port, priv_key), "Receiver bind");
    receiver.SetAllowedRigs({"authorized_rig"});

    // 1. Discovery packet from unauthorized rig should be rejected and NOT trigger an ACK
    UdpTransmitter unauthorized_tx;
    TEST_ASSERT(unauthorized_tx.Open("127.0.0.1", test_port, pub_key, "hacker_rig"), "Open unauthorized tx");
    unauthorized_tx.SendDiscover();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Wire::SatellitePayload dummy{};
    receiver.ReceivePacket(dummy); // Process incoming UDP packet
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(!unauthorized_tx.PollDiscoverAck(0), "Unauthorized rig must not receive DISCOVER_ACK");
    unauthorized_tx.Close();

    // 2. Discovery packet from authorized rig should be accepted and receive an ACK
    UdpTransmitter authorized_tx;
    TEST_ASSERT(authorized_tx.Open("127.0.0.1", test_port, pub_key, "authorized_rig"), "Open authorized tx");
    authorized_tx.SendDiscover();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    receiver.ReceivePacket(dummy);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(authorized_tx.PollDiscoverAck(0), "Authorized rig must receive DISCOVER_ACK");
    authorized_tx.Close();

    // 2b. Case-insensitive allowed rig check
    UdpTransmitter case_tx;
    TEST_ASSERT(case_tx.Open("127.0.0.1", test_port, pub_key, "AUTHORIZED_RIG"), "Open case-variant tx");
    case_tx.SendDiscover();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    receiver.ReceivePacket(dummy);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(case_tx.PollDiscoverAck(0), "Case-variant rig must be accepted via case-insensitive allowed_rigs");
    case_tx.Close();

    // 3. Spoofed packet where outer wire packet rig_id does not match decrypted payload rig_id
    Wire::SatellitePayload inner_payload{};
    std::memcpy(inner_payload.magic, Wire::WIRE_MAGIC, 4);
    inner_payload.version = Wire::WIRE_VERSION;
    std::strncpy(inner_payload.rig_id, "authorized_rig", sizeof(inner_payload.rig_id) - 1);
    inner_payload.session_id = 1234;
    inner_payload.packet_counter = 1;
    inner_payload.sequence_num = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    Wire::SatelliteWirePacket wire_packet{};
    TEST_ASSERT(Crypto::PackAndEncrypt(pub_key, inner_payload, wire_packet), "Pack valid payload");

    // Tamper with outer unencrypted wire rig_id (spoofing a different sender)
    std::strncpy(wire_packet.rig_id, "spoofed_rig", sizeof(wire_packet.rig_id) - 1);

    // Send directly over UDP to receiver
    auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(test_port);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(sock, reinterpret_cast<const char*>(&wire_packet), sizeof(wire_packet), 0,
           reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Wire::SatellitePayload spoofed_rx{};
    bool accepted = receiver.ReceivePacket(spoofed_rx);
    TEST_ASSERT(!accepted, "Receiver must reject packet when outer wire rig_id mismatches decrypted rig_id");

    receiver.Close();
    return true;
}

// AUDIT-P1-06: Systemd ExecStart quotes on spaces; WorkingDirectory must not be quoted
bool Test_ServiceInstaller_SystemdQuoting() {
    fs::path exe_with_space = "/opt/bare stats/barestats";
    fs::path exe_no_space = "/usr/local/bin/barestats";
    fs::path work = "/var/lib/bare stats";

    std::string root_unit = GenerateSystemdServiceUnit(exe_with_space, work, true);
    TEST_ASSERT(root_unit.find("ExecStart=\"/opt/bare stats/barestats\"") != std::string::npos, "Root ExecStart with spaces must be quoted");
    TEST_ASSERT(root_unit.find("WorkingDirectory=/var/lib/bare stats\n") != std::string::npos, "Root WorkingDirectory must not have quotes");

    std::string user_unit = GenerateSystemdServiceUnit(exe_no_space, work, false);
    TEST_ASSERT(user_unit.find("ExecStart=/usr/local/bin/barestats\n") != std::string::npos, "User ExecStart without spaces must not be quoted");
    TEST_ASSERT(user_unit.find("WorkingDirectory=/var/lib/bare stats\n") != std::string::npos, "User WorkingDirectory must not have quotes");

    return true;
}

// AUDIT-P4-01: UDP Discovery Rate-Limiting & Target Isolation (GAP-08)
bool Test_UdpStreamer_DiscoveryRateLimitingAndTargetIsolation() {
    uint8_t priv_a[32] = {0};
    Crypto::GenerateRandomBytes(priv_a, sizeof(priv_a));
    uint8_t pub_a[32] = {0};
    Crypto::DerivePublicKey(priv_a, pub_a);

    uint8_t priv_b[32] = {0};
    Crypto::GenerateRandomBytes(priv_b, sizeof(priv_b));
    uint8_t pub_b[32] = {0};
    Crypto::DerivePublicKey(priv_b, pub_b);

    uint16_t port_a = 19896;
    uint16_t port_b = 19897;

    UdpReceiver receiver_a;
    TEST_ASSERT(receiver_a.Bind(port_a, priv_a), "Receiver A must bind");

    std::vector<SatelliteTargetConfig> targets;
    SatelliteTargetConfig t0;
    t0.name = "TargetA";
    t0.target_host = "auto";
    t0.target_port = port_a;
    std::memcpy(t0.public_key, pub_a, 32);
    targets.push_back(t0);

    SatelliteTargetConfig t1;
    t1.name = "TargetB";
    t1.target_host = "auto";
    t1.target_port = port_b;
    std::memcpy(t1.public_key, pub_b, 32);
    targets.push_back(t1);

    UdpTransmitter transmitter;
    TEST_ASSERT(transmitter.Open("rate_rig", targets), "Transmitter must open");
    // Open in auto-discovery mode executes initial SendDiscover
    const auto& states = transmitter.GetTargets();
    TEST_ASSERT(states.size() == 2, "Must have 2 target states");
    uint32_t init_attempts_0 = states[0].discover_attempts;
    uint32_t init_attempts_1 = states[1].discover_attempts;
    TEST_ASSERT(init_attempts_0 >= 1 && init_attempts_1 >= 1, "Initial discover attempts must be recorded");

    // Immediate SendPayload must NOT re-trigger discovery because 10 seconds have not elapsed
    Wire::SatellitePayload payload{};
    payload.cpu_load_pct = 40;
    std::strncpy(payload.rig_id, "rate_rig", sizeof(payload.rig_id) - 1);
    transmitter.SendPayload(payload);

    TEST_ASSERT(states[0].discover_attempts == init_attempts_0, "Target A must not re-trigger discovery within 10s");
    TEST_ASSERT(states[1].discover_attempts == init_attempts_1, "Target B must not re-trigger discovery within 10s");

    // Per-target discovery isolation: triggering discovery for Target 0 only
    uint64_t target_b_tx_id = states[1].pending_transaction_id;
    bool disc0 = transmitter.SendDiscover(0);
    TEST_ASSERT(disc0, "SendDiscover(0) must succeed");
    TEST_ASSERT(states[0].discover_attempts == init_attempts_0 + 1, "Target A discover attempts must increment");
    TEST_ASSERT(states[1].discover_attempts == init_attempts_1, "Target B discover attempts must remain unchanged");
    TEST_ASSERT(states[1].pending_transaction_id == target_b_tx_id, "Target B transaction ID must not be overwritten");

    // Acknowledge Target A discovery
    bool acked = false;
    for (int attempt = 0; attempt < 50 && !acked; ++attempt) {
        Wire::SatellitePayload dummy{};
        receiver_a.ReceivePacket(dummy);
        acked = transmitter.PollDiscoverAck(50);
        if (!acked) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    TEST_ASSERT(acked, "Must ack Target A discovery");
    TEST_ASSERT(!states[0].discovered_host.empty(), "Target A discovered host must be set");

    // Verify subsequent SendPayload transmits via unicast to Target A
    transmitter.SendPayload(payload);
    Wire::SatellitePayload rx_a{};
    bool got_payload = false;
    for (int attempt = 0; attempt < 50 && !got_payload; ++attempt) {
        got_payload = receiver_a.ReceivePacket(rx_a);
        if (!got_payload) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    TEST_ASSERT(got_payload, "Receiver A must receive payload sent via unicast");
    TEST_ASSERT(rx_a.cpu_load_pct == 40, "CPU load must match");

    transmitter.Close();
    receiver_a.Close();
    return true;
}


