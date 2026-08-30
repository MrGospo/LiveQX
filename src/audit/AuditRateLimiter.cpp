#include "audit/AuditRateLimiter.h"

#include <algorithm>

namespace liveqx::audit {

AuditRateLimiter::AuditRateLimiter(RateLimitConfig cfg) : cfg_(cfg) {}

void AuditRateLimiter::setClockOverride(std::function<TimePoint()> f) {
    clock_override_ = std::move(f);
}

AuditRateLimiter::TimePoint AuditRateLimiter::now() const {
    return clock_override_ ? clock_override_() : Clock::now();
}

void AuditRateLimiter::refill(Bucket& b, TimePoint t) const {
    if (t <= b.last_refill) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        t - b.last_refill).count();
    // tokens per ms = per_minute / 60000
    const double add = static_cast<double>(cfg_.tokens_per_minute) *
                       static_cast<double>(elapsed) / 60000.0;
    b.tokens = std::min(static_cast<double>(cfg_.burst_capacity),
                        b.tokens + add);
    b.last_refill = t;
}

void AuditRateLimiter::evictIdle(TimePoint t) {
    if (t < next_gc_at_) return;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (t - it->second.last_used > cfg_.eviction_after &&
            it->second.tokens >= static_cast<double>(cfg_.burst_capacity)) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
    next_gc_at_ = t + std::chrono::seconds(60);
}

bool AuditRateLimiter::tryAcquire(const std::string& key) {
    const auto t = now();
    std::lock_guard<std::mutex> lk(mu_);
    evictIdle(t);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        Bucket b{static_cast<double>(cfg_.burst_capacity), t, t};
        it = buckets_.emplace(key, b).first;
    }
    refill(it->second, t);
    it->second.last_used = t;

    if (it->second.tokens < 1.0) {
        stat_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    it->second.tokens -= 1.0;
    stat_allowed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool AuditRateLimiter::tryAcquireForUser(std::int64_t user_id) {
    return tryAcquire("u:" + std::to_string(user_id));
}

bool AuditRateLimiter::tryAcquireForIp(std::string_view ip) {
    std::string key;
    key.reserve(ip.size() + 2);
    key.append("i:").append(ip);
    return tryAcquire(key);
}

RateLimitStats AuditRateLimiter::stats() const {
    RateLimitStats s;
    s.allowed  = stat_allowed_.load(std::memory_order_relaxed);
    s.rejected = stat_rejected_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.actors = buckets_.size();
    }
    return s;
}

}  // namespace liveqx::audit
