#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>

// Process-level metrics scraped from /proc/self/* and getrusage().
// Snapshot fields populated on Linux. On non-Linux builds (developer mac),
// integer fields remain at -1 and `cpu_seconds` at -1.0; callers should
// treat negative values as "unavailable" and omit them from exposition.
struct ProcessMetricsSnapshot {
    std::int64_t rss_bytes           = -1;   // VmRSS
    std::int64_t vsize_bytes         = -1;   // VmSize
    std::int64_t open_fds            = -1;   // count of /proc/self/fd entries
    int          threads             = -1;   // VmRSS Threads:
    double       cpu_seconds         = -1.0; // user + sys via getrusage(RUSAGE_SELF)
    std::int64_t start_time_unix_sec = 0;    // captured at construction (0 if not provided)
};

// Cached reader. snapshot() is cheap (1s TTL) so callers can hit it on
// every Prometheus scrape without measurable cost. Thread-safe.
class ProcessMetrics {
public:
    // process_start_unix_sec — seconds since epoch captured at process boot.
    // Pass 0 if not tracked (Prometheus exposition will then omit start time).
    explicit ProcessMetrics(std::int64_t process_start_unix_sec) noexcept;

    ProcessMetricsSnapshot snapshot() const;

    // Cache window. Public so tests can verify staleness handling without
    // sleeping for real time; bump via setCacheTtl in tests.
    static constexpr std::chrono::milliseconds DefaultCacheTtl{1000};

    // Test/perf hook: override TTL. Default = DefaultCacheTtl.
    void setCacheTtl(std::chrono::milliseconds ttl) noexcept;

private:
    void refreshLocked() const;

    std::int64_t                                 start_unix_sec_;
    mutable std::mutex                           mu_;
    mutable std::chrono::steady_clock::time_point last_read_{};
    mutable bool                                 has_cache_ = false;
    mutable ProcessMetricsSnapshot               cached_{};
    std::chrono::milliseconds                    cache_ttl_ = DefaultCacheTtl;
};
