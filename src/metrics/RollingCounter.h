#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Lock-free ringbuffer of per-second counter buckets. One writer per add(),
// one ticker thread calling tick() at 1Hz. Readers (rate()) are wait-free.
//
// Memory model: tick() zeroes the next bucket BEFORE advancing head_. A
// concurrent add() racing with tick() may land in either the bucket about to
// be zeroed or the freshly-zeroed one — for metrics this is acceptable
// (≤1 increment lost in the rare race window).
template <std::size_t N = 60>
class RollingCounter {
public:
    static_assert(N >= 2, "RollingCounter needs at least 2 buckets");

    RollingCounter() {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }

    void add(std::uint64_t delta = 1) noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        buckets_[h].fetch_add(delta, std::memory_order_relaxed);
    }

    // Zero all buckets and reset head. Caller must ensure no concurrent
    // add()/tick() — used during channel restart while the writer is gone.
    void reset() noexcept {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
        head_.store(0, std::memory_order_release);
    }

    // Advance head, zero the new current bucket. Call once per second.
    void tick() noexcept {
        const std::size_t cur  = head_.load(std::memory_order_relaxed);
        const std::size_t next = (cur + 1) % N;
        buckets_[next].store(0, std::memory_order_relaxed);
        head_.store(next, std::memory_order_release);
    }

    // Sum of the W most recent buckets including the current open one.
    // W is clamped to N.
    std::uint64_t rate(std::size_t window_buckets) const noexcept {
        if (window_buckets == 0) return 0;
        if (window_buckets > N) window_buckets = N;
        const std::size_t h = head_.load(std::memory_order_acquire);
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < window_buckets; ++i) {
            const std::size_t idx = (h + N - i) % N;
            sum += buckets_[idx].load(std::memory_order_relaxed);
        }
        return sum;
    }

private:
    std::array<std::atomic<std::uint64_t>, N> buckets_{};
    std::atomic<std::size_t>                  head_{0};
};
