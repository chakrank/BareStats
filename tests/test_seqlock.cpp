#include "test_common.h"
#include "metrics/llm/llm_seqlock.h"
#include "metrics/telemetry_engine.h"
#include "util/http_client.h"
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <chrono>

using namespace BareStats;

bool Test_Seqlock_TriviallyCopyableGuarantees() {
    static_assert(std::is_trivially_copyable_v<LlmSnapshot>, "LlmSnapshot must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<RigClusterSnapshot>, "RigClusterSnapshot must be trivially copyable");

    LlmSeqlock seqlock;
    LlmSnapshot in{};
    in.is_online = true;
    in.llm_count = 1;
    std::strncpy(in.llms[0].model_name, "test-model", sizeof(in.llms[0].model_name) - 1);
    in.llms[0].tps = 42.0f;
    in.llms[0].state = Wire::LlmState::Generating;

    seqlock.WriteSnapshot(in);
    LlmSnapshot out = seqlock.ReadSnapshot();

    TEST_ASSERT(out.is_online, "Online state must match");
    TEST_ASSERT(out.llm_count == 1, "Count must match");
    TEST_ASSERT(std::string(out.llms[0].model_name) == "test-model", "Model name must match");
    TEST_ASSERT(out.llms[0].tps == 42.0f, "TPS must match");

    return true;
}

bool Test_Seqlock_HighContentionStress() {
    LlmSeqlock seqlock;
    std::atomic<bool> writer_done{false};
    std::atomic<size_t> successful_reads{0};
    std::atomic<size_t> torn_reads{0};

    constexpr size_t ITERATIONS = 50000;

    // Background writer thread
    std::thread writer([&]() {
        for (size_t i = 1; i <= ITERATIONS; ++i) {
            LlmSnapshot s{};
            s.is_online = true;
            s.llm_count = 1;
            // Write consistent data where vram and tps are correlated
            s.llms[0].vram_used_mb = static_cast<uint32_t>(i);
            s.llms[0].tps = static_cast<float>(i * 2);
            seqlock.WriteSnapshot(s);
            if ((i & 0x3FF) == 0) {
                std::this_thread::yield();
            }
        }
        writer_done.store(true);
    });

    // Background reader thread
    std::thread reader([&]() {
        while (!writer_done.load()) {
            LlmSnapshot s = seqlock.ReadSnapshot();
            if (s.is_online && s.llm_count == 1) {
                // Verify consistency: tps must be exactly 2 * vram_used_mb
                if (s.llms[0].tps != static_cast<float>(s.llms[0].vram_used_mb * 2)) {
                    torn_reads++;
                }
                successful_reads++;
            }
        }
        // Ensure at least one read executes even if writer finished before reader was scheduled
        LlmSnapshot s = seqlock.ReadSnapshot();
        if (s.is_online && s.llm_count == 1) {
            if (s.llms[0].tps != static_cast<float>(s.llms[0].vram_used_mb * 2)) {
                torn_reads++;
            }
            successful_reads++;
        }
    });

    writer.join();
    reader.join();

    TEST_ASSERT(torn_reads.load() == 0, "Seqlock must have exactly 0 torn reads under contention");
    TEST_ASSERT(successful_reads.load() > 0, "Reader must have executed successful lock-free reads");

    return true;
}

bool Test_Seqlock_HashSnapshot0PercentIdle() {
    LlmSnapshot snap1{};
    snap1.is_online = true;
    snap1.llm_count = 1;
    std::strncpy(snap1.llms[0].model_name, "qwen2.5:7b", sizeof(snap1.llms[0].model_name) - 1);
    snap1.llms[0].tps = 35.0f;
    snap1.llms[0].state = Wire::LlmState::Generating;

    LlmSnapshot snap2 = snap1;

    uint64_t hash1 = HashSnapshot(snap1);
    uint64_t hash2 = HashSnapshot(snap2);

    TEST_ASSERT(hash1 == hash2, "Identical snapshots must yield identical hashes for dirty-frame bypass");

    // Mutation test: changing TPS alters hash
    snap2.llms[0].tps = 35.1f;
    uint64_t hash3 = HashSnapshot(snap2);
    TEST_ASSERT(hash1 != hash3, "Changed TPS must alter hash");

    // Mutation test: changing state alters hash
    snap2.llms[0].tps = 35.0f;
    snap2.llms[0].state = Wire::LlmState::BurstHold;
    uint64_t hash4 = HashSnapshot(snap2);
    TEST_ASSERT(hash1 != hash4, "Changed state must alter hash");

    return true;
}

bool Test_HttpClient_DnsCachingAndBackoff() {
    HttpClient::ClearDnsCache();
    TEST_ASSERT(HttpClient::GetDnsCacheSize() == 0, "Cache must initially be empty");

    // 1. IPv4 literal fast-path bypass (no DNS cache entry created)
    sockaddr_in addr_lit{};
    bool ok_lit = HttpClient::ResolveHost("127.0.0.1", 11434, addr_lit);
    TEST_ASSERT(ok_lit, "IPv4 literal 127.0.0.1 must resolve");
    TEST_ASSERT(HttpClient::GetDnsCacheSize() == 0, "IPv4 literal must not create cache entry");
    TEST_ASSERT(addr_lit.sin_port == htons(11434), "Port must match requested port");

    // 2. Resolve hostname "localhost" (creates active cache entry)
    sockaddr_in addr_lh{};
    bool ok_lh = HttpClient::ResolveHost("localhost", 8080, addr_lh);
    TEST_ASSERT(ok_lh, "localhost must resolve");
    TEST_ASSERT(HttpClient::IsHostCached("localhost"), "localhost must be cached");
    TEST_ASSERT(HttpClient::GetDnsCacheSize() == 1, "Cache size must be 1");

    // 3. Repeated lookup hits cache with new port
    sockaddr_in addr_lh2{};
    bool ok_lh2 = HttpClient::ResolveHost("localhost", 9090, addr_lh2);
    TEST_ASSERT(ok_lh2, "Cached localhost lookup must succeed");
    TEST_ASSERT(addr_lh2.sin_port == htons(9090), "Port must update on cached lookup");

    // 4. Failed lookup records backoff entry
    sockaddr_in addr_fail{};
    bool ok_fail = HttpClient::ResolveHost("nonexistent.invalid.barestats.node", 80, addr_fail, 60, 5);
    TEST_ASSERT(!ok_fail, "Nonexistent hostname must fail resolution");

    // 5. Subsequent lookup within backoff window fails fast without blocking
    auto t0 = std::chrono::steady_clock::now();
    bool ok_fail_fast = HttpClient::ResolveHost("nonexistent.invalid.barestats.node", 80, addr_fail, 60, 5);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT(!ok_fail_fast, "Failed hostname must remain in backoff");
    TEST_ASSERT(elapsed_us < 5000, "Cached failure must return fast in < 5ms");

    HttpClient::ClearDnsCache();
    TEST_ASSERT(HttpClient::GetDnsCacheSize() == 0, "Cache must be empty after clear");

    return true;
}

// BS-VULN-06: Concurrently update cluster snapshot from multiple threads
bool Test_TelemetryEngine_ConcurrentClusterWriters() {
    TelemetryEngine engine;
    engine.SetAppMode(AppMode::CommandCenter);

    std::atomic<bool> start_flag{false};
    std::atomic<bool> stop_flag{false};

    // Thread 1: updates rigA
    std::thread t1([&]() {
        while (!start_flag.load()) std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
            LlmSnapshot snap{};
            snap.is_online = true;
            snap.llm_count = 1;
            snap.llms[0].tps = static_cast<float>(i);
            std::strncpy(snap.llms[0].model_name, "modelA", sizeof(snap.llms[0].model_name) - 1);
            engine.UpdateRigInCluster("rigA", snap);
        }
    });

    // Thread 2: updates rigB
    std::thread t2([&]() {
        while (!start_flag.load()) std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
            LlmSnapshot snap{};
            snap.is_online = true;
            snap.llm_count = 1;
            snap.llms[0].tps = static_cast<float>(i);
            std::strncpy(snap.llms[0].model_name, "modelB", sizeof(snap.llms[0].model_name) - 1);
            engine.UpdateRigInCluster("rigB", snap);
        }
    });

    // Thread 3: concurrent lock-free reader
    std::atomic<size_t> read_count{0};
    std::thread t3([&]() {
        while (!start_flag.load()) std::this_thread::yield();
        while (!stop_flag.load()) {
            RigClusterSnapshot snap = engine.GetClusterSnapshot();
            for (uint8_t r = 0; r < snap.rig_count; ++r) {
                size_t len = strnlen(snap.rigs[r].rig_id, sizeof(snap.rigs[r].rig_id));
                (void)len;
            }
            read_count.fetch_add(1);
        }
    });

    start_flag.store(true);
    t1.join();
    t2.join();
    stop_flag.store(true);
    t3.join();

    RigClusterSnapshot final_snap = engine.GetClusterSnapshot();
    TEST_ASSERT(final_snap.rig_count == 2, "Both rigs must be present in final cluster snapshot");
    bool has_a = false, has_b = false;
    for (uint8_t r = 0; r < final_snap.rig_count; ++r) {
        if (std::string(final_snap.rigs[r].rig_id) == "rigA") has_a = true;
        if (std::string(final_snap.rigs[r].rig_id) == "rigB") has_b = true;
    }
    TEST_ASSERT(has_a && has_b, "Both rigA and rigB must survive concurrent updates without clobbering");
    TEST_ASSERT(read_count.load() > 0, "Concurrent readers must have succeeded");

    return true;
}
