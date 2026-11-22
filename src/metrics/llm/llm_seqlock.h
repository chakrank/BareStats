#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <atomic>
#include <mutex>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h> // For _mm_pause()
#endif

#include "metrics/wire_protocol.h"
#include "metrics/llm/llm_collector.h"
#include "metrics/llm/llm_resolver.h"

namespace BareStats {

template <typename T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>, "Seqlock payload must be trivially copyable!");

public:
    Seqlock() : seq_(0), payload_{} {}

    // WRITER: Called by background network I/O worker or UDP listener
    void WriteSnapshot(const T& new_data) {
        std::lock_guard<std::mutex> lock(write_mutex_);

        // 1. Increment sequence to an ODD number (Write in progress)
        size_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed);

        // 2. Memory fence: ensure sequence increment publishes before memory copy begins
        std::atomic_thread_fence(std::memory_order_release);

        // 3. Perform raw memory copy
        payload_ = new_data;

        // 4. Memory fence: ensure memory copy completes before sequence store publishes
        std::atomic_thread_fence(std::memory_order_release);

        // 5. Increment sequence to an EVEN number (Write complete)
        seq_.store(seq + 2, std::memory_order_release);
    }

    // READER: Called by UI thread at 60 FPS (or on compositor dirty ticks)
    T ReadSnapshot() const {
        T copy;
        while (true) {
            size_t seq0 = seq_.load(std::memory_order_acquire);
            if (seq0 & 1) {
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause(); // Yield CPU pipeline slightly
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
                __yield();
#else
                __asm__ volatile("yield");
#endif
#endif
                continue;
            }

            // Ensure we do not start reading payload before seq0 is validated
            std::atomic_thread_fence(std::memory_order_acquire);

            // Perform optimistic read
            copy = payload_;

            // Ensure payload copy completes before reading seq1
            std::atomic_thread_fence(std::memory_order_acquire);

            // Validate sequence hasn't changed during copy
            size_t seq1 = seq_.load(std::memory_order_acquire);
            if (seq0 == seq1) {
                return copy;
            }
        }
    }

private:
    std::atomic<size_t> seq_{0};
    mutable std::mutex  write_mutex_;
    T                   payload_{};
};

using LlmSeqlock = Seqlock<LlmSnapshot>;
using ClusterSeqlock = Seqlock<RigClusterSnapshot>;

// 64-bit FNV-1a hash over active snapshot bytes for 0.00% idle duty cycle
inline uint64_t HashSnapshot(const LlmSnapshot& snap) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&snap);

    // Hash only the active bytes to ignore trailing unpopulated slots
    size_t active_size = offsetof(LlmSnapshot, llms) + (snap.llm_count * sizeof(Wire::LlmMetrics));
    if (active_size > sizeof(LlmSnapshot)) {
        active_size = sizeof(LlmSnapshot);
    }

    for (size_t i = 0; i < active_size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL;
    }

    hash ^= snap.is_online ? 1 : 0;
    return hash;
}

// 64-bit FNV-1a hash over active cluster snapshot bytes
inline uint64_t HashClusterSnapshot(const RigClusterSnapshot& cluster) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&cluster);

    size_t active_size = offsetof(RigClusterSnapshot, rigs) + (cluster.rig_count * sizeof(RigNodeSnapshot));
    if (active_size > sizeof(RigClusterSnapshot)) {
        active_size = sizeof(RigClusterSnapshot);
    }

    for (size_t i = 0; i < active_size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL;
    }

    return hash;
}

} // namespace BareStats
