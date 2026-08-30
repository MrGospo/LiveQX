#pragma once

// Per-actor token bucket for mutation traffic.
//
// A single misbehaving admin script writing thousands of channels a second
// can bury the audit trail in noise and DoS the SQLite writer. This
// limiter caps mutations at N per minute per actor (default 300) —
// enough for legitimate bulk operations, refuses runaway loops. Reads
// (GET) are not rate-limited here; that belongs in an HTTP-level RL.
//
// Actor identity:
//   - authenticated request  → user_id (int64)
//   - anonymous request      → IP string (login flooders)
// Both key into the same map through a stable string form.
//
// Threading: one std::mutex around the bucket map. Contention is trivial
// (map operations dominated by ~O(active_actors) lookups). Buckets are
// GC'd on the fly: a bucket with a full token count that has not been
// touched for the eviction interval is dropped so the map stays small.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace liveqx::audit {

struct RateLimitConfig {
    int         tokens_per_minute{300};
    int         burst_capacity   {300};
    std::chrono::seconds eviction_after{600};  // idle bucket TTL
};

struct RateLimitStats {
    std::uint64_t allowed {0};
    std::uint64_t rejected{0};
    std::size_t   actors  {0};
};

class AuditRateLimiter {
public:
    explicit AuditRateLimiter(RateLimitConfig cfg = {});

    // True → caller proceeds. False → caller responds 429 and emits a
    // Category::Access audit event.
    bool tryAcquireForUser(std::int64_t user_id);
    bool tryAcquireForIp(std::string_view ip);

    RateLimitStats stats() const;

    // Test seam. Injects a monotonic clock so tests can advance time
    // deterministically.
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    void setClockOverride(std::function<TimePoint()> f);

private:
    struct Bucket {
        double     tokens;
        TimePoint  last_refill;
        TimePoint  last_used;
    };

    bool tryAcquire(const std::string& key);
    TimePoint now() const;
    void refill(Bucket& b, TimePoint t) const;
    void evictIdle(TimePoint t);

    RateLimitConfig cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
    TimePoint next_gc_at_{};

    std::function<TimePoint()> clock_override_;

    mutable std::atomic<std::uint64_t> stat_allowed_ {0};
    mutable std::atomic<std::uint64_t> stat_rejected_{0};
};

}  // namespace liveqx::audit
