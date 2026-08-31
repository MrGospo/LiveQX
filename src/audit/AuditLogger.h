#pragma once

// Async, batched writer in front of AuditDb.
//
// Producers (HTTP handlers, background jobs) call log() from any thread —
// the event is enqueued and the caller returns immediately. A dedicated
// writer thread drains the queue in batches, wraps each batch in a single
// BEGIN IMMEDIATE / COMMIT and chains rows against the live tail.
//
// Failure paths:
//   1. audit.db is healthy   → batch INSERT lands, all consumers happy.
//   2. audit.db write fails  → batch is drained to state/audit-emergency.jsonl
//                              (append + fsync). Ops recovers the trail into
//                              audit.db offline once the DB is back.
//   3. audit.db unopened     → same as (2). start() does not require the DB
//                              to have opened successfully — a broken audit
//                              DB must never wedge the daemon; instead every
//                              event queues onto the emergency file.
//
// Backpressure & fail-closed for mutations:
//   - Soft cap (kBacklogSoftCap): queue > threshold → warning logged.
//   - Hard cap (kBacklogHardCap): queue > threshold → shouldRejectMutation()
//     returns true and mutation handlers respond 503 Service Unavailable.
//     Auth handlers use the broken-glass path (below) so login still works.
//
// Broken-glass path:
//   - logSyncBrokenGlass(): synchronous write; on DB failure writes to the
//     emergency file directly and returns. Never blocks the caller on the
//     writer queue. Auth uses this so an audit-degraded box does not lock
//     admins out of the UI.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audit/AuditTypes.h"

namespace liveqx::audit {

class AuditDb;

struct AuditLoggerStats {
    std::uint64_t enqueued        {0};
    std::uint64_t written_db      {0};
    std::uint64_t written_emergency{0};
    std::uint64_t db_failures     {0};
    std::uint64_t dropped_overflow{0}; // never > 0 unless emergency file also failed
    std::uint64_t queue_depth     {0};
    std::int64_t  last_write_ns   {0};
    bool          fail_closed     {false}; // hard cap tripped
};

class AuditLogger {
public:
    // db may be nullptr or !db->ok() — the logger will boot in emergency-only
    // mode and every event lands in the JSONL fallback.
    AuditLogger(AuditDb* db, std::filesystem::path emergency_file);
    ~AuditLogger();

    AuditLogger(const AuditLogger&)            = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // Start / stop the writer thread. start() is idempotent.
    void start();
    void stop();

    // Enqueue. Non-blocking. If the queue is over the hard cap and the
    // emergency file is also unwritable, the event is dropped and
    // dropped_overflow is incremented. Under normal operation this is 0.
    void log(AuditEvent ev);

    // Broken-glass path — synchronous, never queues. Tries DB first; on
    // failure writes to the emergency file. Used by the auth login path
    // so an audit-degraded system still lets admins in.
    void logSyncBrokenGlass(AuditEvent ev);

    // True iff enqueued backlog exceeds the hard cap. Mutation handlers
    // consult this and respond 503 to preserve the audit invariant that
    // every mutation is recorded.
    bool shouldRejectMutation() const noexcept;

    // Read-side access for the REST layer. Nullable — /api/audit/*
    // handlers respond 503 when the underlying DB is missing so the UI
    // renders a proper empty state instead of a query error.
    AuditDb* db() const noexcept { return db_; }

    AuditLoggerStats stats() const;

private:
    static constexpr std::size_t kBacklogSoftCap = 5000;
    static constexpr std::size_t kBacklogHardCap = 20000;
    static constexpr std::size_t kBatchMax       = 256;
    static constexpr auto        kFlushInterval  = std::chrono::milliseconds(200);

    void writerLoop();
    // Attempts DB batch insert; on failure spills to emergency file.
    // Returns count actually persisted (either destination counts).
    std::size_t drainBatch(std::vector<AuditEvent>& batch);
    // Serializes one event as a single JSONL line (self-describing so ops
    // can replay it into audit.db later). Called under emergency_mu_.
    bool writeEmergency(const AuditEvent& ev);
    static std::string toJsonLine(const AuditEvent& ev);

    AuditDb*                  db_{nullptr};
    std::filesystem::path     emergency_path_;

    mutable std::mutex        queue_mu_;
    std::condition_variable   queue_cv_;
    std::deque<AuditEvent>    queue_;
    std::atomic<bool>         stopping_{false};
    std::atomic<bool>         running_{false};
    std::thread               writer_;

    // Emergency file. Held open in append mode after the first write;
    // reopened on error.
    std::mutex                emergency_mu_;
    std::ofstream             emergency_stream_;

    mutable std::atomic<std::uint64_t> stat_enqueued_        {0};
    mutable std::atomic<std::uint64_t> stat_written_db_      {0};
    mutable std::atomic<std::uint64_t> stat_written_emerg_   {0};
    mutable std::atomic<std::uint64_t> stat_db_failures_     {0};
    mutable std::atomic<std::uint64_t> stat_dropped_         {0};
    mutable std::atomic<std::int64_t>  stat_last_write_ns_   {0};
};

}  // namespace liveqx::audit
