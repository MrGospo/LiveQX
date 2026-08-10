#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

// Lock-free single-producer single-consumer ring buffer.
// N must be a power of 2.
//
// Hot path push/pop are lock-free. The CV is used only as a wakeup mechanism
// for blocking consumers — `notify()` must be called by the producer after a
// successful push (or after any state change consumers wait on, e.g. shutdown).
template<typename T, size_t N>
class SpscQueue {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be power of 2");

public:
    bool push(T item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        data_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;
        T item = std::move(data_[tail]);
        tail_.store((tail + 1) & (N - 1), std::memory_order_release);
        return item;
    }

    // Blocking pop with timeout. Returns std::nullopt if no element became
    // available within `timeout`. Spurious wakeups are tolerated: the predicate
    // is `!empty()`. Producers must call notify() after push to wake waiters.
    std::optional<T> pop_wait(std::chrono::milliseconds timeout) {
        if (auto v = pop()) return v;
        std::unique_lock<std::mutex> lk(cv_mtx_);
        cv_.wait_for(lk, timeout, [this] { return !empty(); });
        lk.unlock();
        return pop();
    }

    void notify() { cv_.notify_one(); }
    void notify_all() { cv_.notify_all(); }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    std::array<T, N> data_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::mutex              cv_mtx_;
    std::condition_variable cv_;
};
