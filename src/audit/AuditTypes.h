#pragma once

// Enterprise audit trail. Distinct from the legacy auth-only auth_audit
// table (see AuthTypes.h::AuditEvent) — this record captures every
// mutating server action: user/channel/output/gateway/plugin/system.
//
// Storage lives in state/audit.db, owned by AuditDb. Rows are append-only;
// each carries an HMAC-SHA256 tag chained to the previous row, keyed on
// the master key, so any offline mutation is detectable by rescanning
// the chain (see AuditDb::verifyChain, added later in этап 1).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::audit {

// Category buckets are stable strings — operators write SIEM filters on
// them and category-based retention (365/90/30d) keys off this column.
// Adding a new category is a config change, not a schema migration.
enum class Category {
    Auth,       // login / logout / password / session lifecycle
    Channel,    // channel CRUD, playlist, schedule mutations
    Output,     // output CRUD + start/stop
    Gateway,    // gateway CRUD + start/stop
    Plugin,     // plugin install / enable / disable
    Mount,      // storage mounts CRUD
    System,     // system config, master-key rotation, service restart
    Access,     // rejected access — RBAC deny, rate-limit trip
};

const char* categoryName(Category c) noexcept;
std::optional<Category> categoryFromString(std::string_view s) noexcept;

// Retention policy per category, in days. Consumed by the janitor.
// Auth events kept longest (regulator requirement). System/access shorter.
int defaultRetentionDays(Category c) noexcept;

// One row in audit_events. Producer builds this; AuditDb signs it into
// the chain on insert. `id`, `prev_mac`, `mac`, `key_fingerprint` are
// filled by the DB layer — producers leave them at defaults.
struct AuditEvent {
    // ── identity / clock ────────────────────────────────────────────
    std::int64_t                 id{0};              // filled by DB
    std::int64_t                 ts_unix_ms{0};      // wall-clock ms

    // ── classification ──────────────────────────────────────────────
    Category                     category{Category::System};
    std::string                  action;             // "channel.update", "login.ok"

    // ── actor (who did it) ──────────────────────────────────────────
    // user_id/username are empty for anonymous / pre-auth actions
    // (e.g. failed login before user lookup). role is snapshot-at-time —
    // if we later demote the user, the audit row still reflects the
    // role that was in effect when the mutation happened.
    std::optional<std::int64_t>  actor_user_id;
    std::string                  actor_username;
    std::string                  actor_role;         // "admin"|"operator"|"viewer"|""
    std::string                  actor_ip;

    // ── target (what was acted on) ──────────────────────────────────
    // target_type: "channel"|"output"|"gateway"|"plugin"|"user"|"mount"|"system"|""
    // target_id:   opaque string (channel_id, username, mount unit path…).
    std::string                  target_type;
    std::string                  target_id;

    // ── HTTP context (empty for background jobs / CLI actions) ──────
    std::string                  http_method;        // "PATCH"
    std::string                  http_path;          // "/api/channels/42"
    int                          http_status{0};     // 0 = not applicable
    std::int64_t                 elapsed_ms{0};

    // ── human summary + structured details ──────────────────────────
    // summary is what shows up in the UI list column. details_json is
    // the full payload (diff, request body sans secrets). Never store
    // raw passwords, tokens or master-key material here — sanitisation
    // is the producer's responsibility.
    std::string                  summary;
    std::string                  details_json{"{}"};

    // ── correlation ─────────────────────────────────────────────────
    // request_id ties multiple audit rows to a single HTTP request
    // (useful for actions that fan out: channel.stop → output.stop
    // for each attached output). Empty = no correlation.
    std::string                  request_id;

    // ── tamper-evidence (filled by AuditDb) ─────────────────────────
    std::vector<std::uint8_t>    prev_mac;           // 0 or 32 bytes
    std::vector<std::uint8_t>    mac;                // always 32 bytes on read
    std::string                  key_fingerprint;    // 16 hex chars
    bool                         legacy{false};      // 1 iff pre-chain row (backfilled)
};

// Filter for AuditDb::list. All conditions AND together. limit is
// capped to 1000 by the DB layer to bound response size.
struct AuditFilter {
    std::optional<std::int64_t>  from_ts_ms;         // ts_unix_ms >= from
    std::optional<std::int64_t>  to_ts_ms;           // ts_unix_ms <  to
    std::optional<Category>      category;
    std::string                  action;             // exact match if non-empty
    std::optional<std::int64_t>  actor_user_id;
    std::string                  actor_username;     // exact match if non-empty
    std::string                  target_type;
    std::string                  target_id;
    std::string                  request_id;
    int                          limit{100};
    int                          offset{0};
};

}  // namespace liveqx::audit
