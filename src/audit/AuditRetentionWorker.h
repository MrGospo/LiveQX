#pragma once

// Background janitor for state/audit.db. Wakes on a fixed interval and
// deletes rows older than the per-category retention window (see
// AuditTypes::defaultRetentionDays):
//
//   auth     — 365d (regulator requirement)
//   channel/output/gateway/plugin/mount — 90d
//   system   — 90d
//   access   — 30d
//
// The purge does NOT rewrite the HMAC chain — old prev_mac links to
// deleted rows are treated as "expected gap" by AuditDb::verifyChain.
//
// Runs on its own thread so heavy purges (millions of rows on year-old
// installs) never block AuditLogger's writer thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace liveqx::audit {

class AuditDb;

class AuditRetentionWorker {
public:
    // db may be null / !ok — worker starts anyway and no-ops each tick
    // until the DB becomes available. run_interval controls how often
    // we scan (default 1h — retention windows are day-scale, so hourly
    // is fine and keeps the per-tick cost bounded).
    AuditRetentionWorker(AuditDb* db,
                         std::chrono::minutes run_interval = std::chrono::minutes{60});
    ~AuditRetentionWorker();

    AuditRetentionWorker(const AuditRetentionWorker&)            = delete;
    AuditRetentionWorker& operator=(const AuditRetentionWorker&) = delete;

    void start();
    void stop();

    // Public for tests — runs one purge sweep synchronously on the caller
    // thread. Returns total rows removed across all categories.
    int runOnce();

private:
    void loop();

    AuditDb*                  db_{nullptr};
    std::chrono::minutes      interval_;
    std::atomic<bool>         running_{false};
    std::atomic<bool>         stopping_{false};
    std::mutex                mu_;
    std::condition_variable   cv_;
    std::thread               worker_;
};

}  // namespace liveqx::audit
