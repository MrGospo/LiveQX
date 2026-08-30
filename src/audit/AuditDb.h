#pragma once

// Owner of state/audit.db. Distinct from AuthDb: audit is append-only,
// tamper-evident and category-partitioned for retention. Kept isolated
// so a busy auth path (login storms, LDAP refresh) never contends with
// audit writes on the same SQLite mutex.
//
// Schema highlights:
//   - single audit_events table with an HMAC-SHA256 chain (prev_mac,
//     mac, key_fingerprint) so any offline row edit / delete / insert
//     is detectable by re-scanning the chain.
//   - audit_meta (key, value) — schema_version + last_mac cursor.
//   - indices on ts, actor, target, category — expected list-query
//     shapes: recent-first, by-user, by-target, category filter + range.
//
// Threading: single std::mutex, all methods take it. AuditLogger
// batches writes so the mutex is rarely contended.
//
// Corrupt-file handling mirrors AuthDb: any open/migration failure
// renames the file `<path>.corrupt-<unix_ns>` so the daemon can boot
// with a fresh audit.db and ops recovers the previous DB offline.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audit/AuditTypes.h"

struct sqlite3;

namespace liveqx::auth { class MasterKey; }

namespace liveqx::audit {

// Result of AuditDb::verifyChain — used by ops to prove the trail is
// intact. first_bad_id is 0 iff every row's mac reproduced under
// HMAC(master_key, prev_mac || canonical(row)).
struct ChainVerification {
    std::int64_t scanned{0};
    std::int64_t first_bad_id{0};    // 0 = clean
    std::string  first_bad_reason;   // "mac_mismatch"|"prev_mac_mismatch"|"key_epoch"
};

class AuditDb {
public:
    // key_ref is captured by pointer — MasterKey lifetime is process-
    // wide (owned by main.cpp) and outlives AuditDb.
    AuditDb(std::filesystem::path db_path,
            const liveqx::auth::MasterKey* key);
    ~AuditDb();

    AuditDb(const AuditDb&)            = delete;
    AuditDb& operator=(const AuditDb&) = delete;

    // Open (create if missing), PRAGMAs + migrations. Returns true iff
    // the database is queryable. On failure the file is renamed
    // .corrupt-<ns> and ok() stays false.
    bool open();
    bool ok() const noexcept { return db_ != nullptr; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // Append one row. Fills id, prev_mac, mac, key_fingerprint from the
    // current chain tail + master key, then INSERTs. Returns the row id
    // on success, std::nullopt on any DB error (caller falls back to
    // the emergency JSONL file).
    std::optional<std::int64_t> insert(AuditEvent& e);

    // Batch variant. Wraps the batch in a single BEGIN IMMEDIATE / COMMIT
    // and chains all rows against the same live tail — much cheaper than
    // N single inserts. On any row failure the whole batch rolls back and
    // returns 0.
    std::size_t insertBatch(std::vector<AuditEvent>& batch);

    // Query with filter. Returns rows ordered by ts_unix_ms DESC, id DESC.
    // limit is capped at 1000 regardless of filter.limit.
    std::vector<AuditEvent> list(const AuditFilter& f);

    // Total rows matching filter (ignores limit/offset). Used by the
    // UI to render "N events" and paginate.
    std::int64_t count(const AuditFilter& f);

    // Retention purge — DELETE rows in `cat` older than cutoff_ms.
    // Returns rows removed. Does NOT rewrite the chain — the tail
    // survives, older prev_mac links to a hole. verifyChain treats
    // pre-cutoff gaps as expected (see ChainVerification comments).
    int purgeOlderThan(Category cat, std::int64_t cutoff_ms);

    // Full-chain rescan. Reads every row in id order, recomputes
    // HMAC(prev_mac || canonical(row)) and compares with stored mac.
    // Handles key-rotation: rows tagged with a different key_fingerprint
    // are trusted (we can't verify old MACs after the key is gone).
    ChainVerification verifyChain();

private:
    std::filesystem::path              path_;
    const liveqx::auth::MasterKey*     key_{nullptr};
    std::mutex                         mutex_;
    sqlite3*                           db_{nullptr};

    // Cached tail of the chain — last inserted row's mac. Kept in
    // memory so hot inserts don't re-query audit_meta every time.
    // Refreshed from audit_meta at open() and bumped on every insert.
    std::vector<std::uint8_t>          last_mac_;

    bool exec(const char* sql);
    bool runMigrations();
    bool loadChainTail();
    bool saveChainTail();
    // Deterministic serialization of an AuditEvent — feeds the HMAC.
    // Excludes id/prev_mac/mac/key_fingerprint (those are the chain
    // itself). Excludes `legacy`. Stable across binary rebuilds so
    // verifyChain can reproduce years-old MACs.
    static std::string canonicalize(const AuditEvent& e);
};

}  // namespace liveqx::audit
