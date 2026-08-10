#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include "utils/Log.h"

// Per call-site rate limiter. Each LOG_*_RL call instantiates one static
// Limiter; concurrent threads hitting the same site share it.
//
// Window-based: at most `max_per_window` admissions per `window_ns`. Past
// the budget messages are silently counted; on the next admission the
// suppressed total is logged once before the new message.
namespace ratelog {

class Limiter {
public:
    constexpr Limiter(std::int64_t window_ns, std::uint32_t max_per_window) noexcept
        : window_ns_(window_ns), max_(max_per_window) {}

    struct Decision {
        bool          allow;
        std::uint64_t suppressed;  // non-zero only on first admission of new window
    };

    Decision admit() noexcept {
        using namespace std::chrono;
        const std::int64_t now = duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();

        std::int64_t start = window_start_ns_.load(std::memory_order_relaxed);
        if (start == 0 || (now - start) >= window_ns_) {
            // Try to roll the window. Only one thread wins the CAS.
            if (window_start_ns_.compare_exchange_strong(
                    start, now, std::memory_order_acq_rel)) {
                count_.store(1, std::memory_order_relaxed);
                const std::uint64_t sup =
                    suppressed_.exchange(0, std::memory_order_relaxed);
                return {true, sup};
            }
            // Lost the CAS — fall through to the loaded `start` from another
            // thread and treat as in-window.
        }

        const std::uint32_t c =
            count_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= max_) return {true, 0};
        suppressed_.fetch_add(1, std::memory_order_relaxed);
        return {false, 0};
    }

private:
    const std::int64_t          window_ns_;
    const std::uint32_t         max_;
    std::atomic<std::int64_t>   window_start_ns_{0};
    std::atomic<std::uint32_t>  count_{0};
    std::atomic<std::uint64_t>  suppressed_{0};
};

}  // namespace ratelog

// Default budget: 5 messages / 1 second per call-site.
#define LOG_RL_IMPL(level_macro, max_per_sec, ...)                              \
    do {                                                                        \
        static ::ratelog::Limiter _rl_lim(1'000'000'000LL,                      \
                                          static_cast<std::uint32_t>(max_per_sec)); \
        const auto _rl_d = _rl_lim.admit();                                     \
        if (_rl_d.allow) {                                                      \
            if (_rl_d.suppressed)                                               \
                spdlog::warn("[rate-limit] suppressed {} messages in last 1s",  \
                             _rl_d.suppressed);                                 \
            level_macro(__VA_ARGS__);                                           \
        }                                                                       \
    } while (0)

#define LOG_WARN_RL(max_per_sec, ...)  LOG_RL_IMPL(LOG_WARN,  max_per_sec, __VA_ARGS__)
#define LOG_DEBUG_RL(max_per_sec, ...) LOG_RL_IMPL(LOG_DEBUG, max_per_sec, __VA_ARGS__)
#define LOG_ERROR_RL(max_per_sec, ...) LOG_RL_IMPL(LOG_ERROR, max_per_sec, __VA_ARGS__)

// ── Logger-aware variants ───────────────────────────────────────────────────
// Same window/budget semantics, but route through an explicit logger
// reference (e.g. a per-channel spdlog::logger). `lg_expr` may be any
// expression yielding `spdlog::logger&` — typically the host class's lg().
#define LOG_RL_LG_IMPL(lg_expr, level_method, max_per_sec, ...)                \
    do {                                                                       \
        static ::ratelog::Limiter _rl_lim(1'000'000'000LL,                     \
                                          static_cast<std::uint32_t>(max_per_sec)); \
        const auto _rl_d = _rl_lim.admit();                                    \
        if (_rl_d.allow) {                                                     \
            auto& _rl_lg = (lg_expr);                                          \
            if (_rl_d.suppressed)                                              \
                _rl_lg.warn("[rate-limit] suppressed {} messages in last 1s",  \
                            _rl_d.suppressed);                                 \
            _rl_lg.level_method(__VA_ARGS__);                                  \
        }                                                                      \
    } while (0)

#define LOG_WARN_RL_LG(lg_expr, max_per_sec, ...)  LOG_RL_LG_IMPL(lg_expr, warn,  max_per_sec, __VA_ARGS__)
#define LOG_DEBUG_RL_LG(lg_expr, max_per_sec, ...) LOG_RL_LG_IMPL(lg_expr, debug, max_per_sec, __VA_ARGS__)
#define LOG_ERROR_RL_LG(lg_expr, max_per_sec, ...) LOG_RL_LG_IMPL(lg_expr, error, max_per_sec, __VA_ARGS__)
