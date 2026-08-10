#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "gateway/ts/TsPacket.h"

namespace liveqx::gateway::ts {

// Lock-free SPSC ring of fixed-size 188-byte TS packet slots.
//
// Sized for the NIC-IO → parser pipeline:
//   * Producer is one IO thread (recvmmsg / SRT / RTP source) that never blocks
//     on the queue — the network arrives whether or not we keep up.
//   * Consumer is the parser thread (PsiSectionAssembler + PCR sampler).
//
// Backpressure: when the ring fills, the producer overwrites the OLDEST slot
// (advances the tail), counting the drop in `dropped_oldest_`. This matches the
// broadcast convention — fresh packets are more useful than stale ones for
// live PSI/SI carriage; the parser will naturally re-acquire from the next
// PUSI'd section. Per-output backpressure is handled separately (each output
// has its own SPSC and may drop independently).
//
// Slots are stored in-place (`std::array<uint8_t, 188>`) so steady-state hot
// path has zero allocations and a single 188-byte memcpy per packet.
//
// Capacity must be a power of two so that masking replaces modulo. Choose a
// capacity that comfortably covers one IO burst: at 100 Mbps an 8K slot ring
// = 1.5 MB and ≈120 ms of headroom.
template <std::size_t Capacity>
class TsRingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr std::size_t kCapacity = Capacity;

    TsRingBuffer() noexcept = default;
    TsRingBuffer(const TsRingBuffer&) = delete;
    TsRingBuffer& operator=(const TsRingBuffer&) = delete;

    // Producer. Never returns false — when full, the oldest slot is dropped
    // and `dropped_oldest_` is incremented. Returns true if no drop occurred,
    // false if a drop happened (so the caller can update metrics).
    bool push(std::span<const std::uint8_t, kTsPacketSize> packet) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & (Capacity - 1);
        bool ok = true;
        if (next == tail_.load(std::memory_order_acquire)) {
            // Full. Advance tail to overwrite the oldest. Producer is allowed
            // to move tail in this design because there is exactly one
            // producer; the consumer's tail.load above sees the snapshot
            // before our store, and the subsequent fetch_add publishes the
            // drop bookkeeping.
            tail_.store((tail_.load(std::memory_order_relaxed) + 1) & (Capacity - 1),
                        std::memory_order_release);
            dropped_oldest_.fetch_add(1, std::memory_order_relaxed);
            ok = false;
        }
        std::memcpy(slots_[head].data(), packet.data(), kTsPacketSize);
        head_.store(next, std::memory_order_release);
        return ok;
    }

    // Strict push: if the ring is full, do not drop — return false. Used for
    // tests and for paths where the caller wants to apply its own backpressure.
    bool tryPush(std::span<const std::uint8_t, kTsPacketSize> packet) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        std::memcpy(slots_[head].data(), packet.data(), kTsPacketSize);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer. Copies one packet into `out`. Returns false when empty.
    bool pop(std::span<std::uint8_t, kTsPacketSize> out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        std::memcpy(out.data(), slots_[tail].data(), kTsPacketSize);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    // Zero-copy peek. Returns a const view of the next packet without
    // advancing the consumer cursor. The pointer remains valid until the
    // next pop()/peekAdvance() call from the same consumer thread.
    bool peek(TsPacketView& out) const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = TsPacketView(std::span<const std::uint8_t, kTsPacketSize>(slots_[tail].data(),
                                                                         kTsPacketSize));
        return true;
    }

    void peekAdvance() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
    }

    // Approximate occupancy. Lock-free, may race with concurrent push/pop;
    // good enough for metrics and "is it draining?" diagnostics.
    std::size_t sizeApprox() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (Capacity - 1);
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    std::uint64_t droppedOldest() const noexcept {
        return dropped_oldest_.load(std::memory_order_relaxed);
    }

    void resetDroppedCounter() noexcept {
        dropped_oldest_.store(0, std::memory_order_relaxed);
    }

private:
    using Slot = std::array<std::uint8_t, kTsPacketSize>;
    std::array<Slot, Capacity> slots_{};

    // Cache-line-isolated to avoid false sharing between producer and consumer.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_oldest_{0};
};

}  // namespace liveqx::gateway::ts
