#pragma once

// Enterprise audit trail. Captures every mutating server action:
// user/channel/output/gateway/plugin/system.
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
    Player,     // player (LiveQX-Player) pairing, revocation, settings
};

// Every category, once. Anything that needs "all of them" (retention worker,
// the categories endpoint) iterates this, so a new category cannot be added to
// the enum and forgotten in a hand-written copy of the list.
inline constexpr Category kAllCategories[] = {
    Category::Auth,   Category::Channel, Category::Output, Category::Gateway,
    Category::Plugin, Category::Mount,   Category::System, Category::Access,
    Category::Player,
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
    // target_type: "channel"|"output"|"gateway"|"plugin"|"user"|"mount"|"player"|"system"|""
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
    // ── where it happened (players, B13) ────────────────────────────
    // The object and group of the screen (or group, or object) the event is about,
    // as they were WHEN it was written; unset for events that are not about a place.
    // An event can touch two places: a screen moved from one group to another.
    // `scope_*` is where it ended up, `scope_from_*` where it came from.
    //
    // `scoped` says which format the row was written in, and so what its
    // signature covers — rows are verified by the rule they were written under:
    //   0  before places existed: no place in the signature
    //   1  the place (scope_*) is signed
    //   2  the place and the place it came from (scope_from_*) are signed
    int                          scoped{0};
    std::optional<std::int64_t>  scope_object_id;
    std::optional<std::int64_t>  scope_group_id;
    std::optional<std::int64_t>  scope_from_object_id;
    std::optional<std::int64_t>  scope_from_group_id;

    std::vector<std::uint8_t>    prev_mac;           // 0 or 32 bytes
    std::vector<std::uint8_t>    mac;                // always 32 bytes on read
    std::string                  key_fingerprint;    // 16 hex chars
    bool                         legacy{false};      // 1 iff pre-chain row (backfilled)
};

// A place an event is about (see AuditEvent::scope_*).
struct AuditScope {
    std::optional<std::int64_t> object_id;
    std::optional<std::int64_t> group_id;
    // Where it came from, for an event that moved something between places.
    std::optional<std::int64_t> from_object_id;
    std::optional<std::int64_t> from_group_id;
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
    // Place: an event touching this object / group at either end (the place it
    // happened in, or the one it came from).
    std::optional<std::int64_t>  scope_object_id;
    std::optional<std::int64_t>  scope_group_id;
    // For people who may read the audit of some places only: player events whose
    // place is one of these objects or groups, and nothing else. Applied in the
    // query itself, so the count and the paging respect it. Both lists empty
    // while this is set = nothing matches.
    bool                         restrict_to_places{false};
    std::vector<std::int64_t>    allowed_object_ids;
    std::vector<std::int64_t>    allowed_group_ids;
    int                          limit{100};
    int                          offset{0};
};

}  // namespace liveqx::audit
