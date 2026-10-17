#pragma once
//
// state/players.db — the store behind the LiveQX-Player integration (phase 2).
//
// Kept apart from auth.db on purpose: different growth pattern (heartbeats,
// state-change events), different retention, and a different risk — this file
// holds pairing-token hashes and the list of issued certificates.
//
//   players        one row per player: UUID (the certificate CN), name, state
//                  pending -> active -> revoked
//   enroll_tokens  one-time pairing tokens, ONLY as SHA-256 hashes
//   player_certs   every certificate ever issued (CA generation + serial);
//                  the source for CRLs. Rows OUTLIVE the player: a revoked
//                  certificate has to stay in the CRL until it would have
//                  expired anyway.
//   player_status  the last known state per player (not every heartbeat)
//   player_events  state changes (enrolled, online, offline, revoked, ...)
//   crl_state      the monotonic CRL number per CA generation
//   players_settings  the module's key/value settings (schema version 2)
//   player_objects / player_groups  the hierarchy all -> object -> group
//                  (schema version 3); a player belongs to exactly one group
//
// Pairing tokens. The plaintext exists exactly once, in the return value of
// createPlayer()/reissueToken(). claimToken() compares hashes in constant time
// over all of a player's tokens and burns the matching one atomically, so two
// concurrent requests with one token have exactly one winner. Why a claim
// failed is returned for the AUDIT only; the HTTP answer is the same for all
// reasons (A4). A CSR that turns out unusable releases the claim, so the
// player can retry; a certificate that is issued completes it.
//
// Times are unix seconds; a `now` of 0 means "read the clock". Thread-safe
// (one mutex; writes are BEGIN IMMEDIATE transactions).

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace liveqx::players {

enum class PlayerState { Pending, Active, Revoked };

const char* toString(PlayerState s) noexcept;
std::optional<PlayerState> parsePlayerState(const std::string& s) noexcept;

struct PlayerRow {
    std::string                 id;                 // UUID v4, lowercase = certificate CN
    std::string                 name;
    PlayerState                 state{PlayerState::Pending};
    std::string                 clock_policy{"strict"};   // as in the pairing package
    std::int64_t                created_at{0};
    std::optional<std::int64_t> created_by;         // user id
    std::optional<std::int64_t> activated_at;
    std::optional<std::int64_t> revoked_at;
    std::string                 revoke_reason;
    // The one group the player sits in. Null only for rows made below the
    // service layer (tests); the service never creates a player without one,
    // and a player without a group is visible to administrators only.
    std::optional<std::int64_t> group_id;
};

struct ObjectRow {
    std::int64_t id{0};
    std::string  name;
    std::int64_t created_at{0};
};

struct GroupRow {
    std::int64_t id{0};
    std::int64_t object_id{0};
    std::string  name;
    std::int64_t created_at{0};
};

// Outcome of changes to the hierarchy. NotEmpty: an object that still has
// groups, or a group that still has players, cannot be removed.
enum class GroupResult { Ok, NotFound, NameTaken, InvalidName, NotEmpty, Storage };

struct CreatedPlayer {
    PlayerRow    player;
    std::string  token;              // plaintext, shown ONCE
    std::int64_t token_expires_at{0};
};

struct TokenIssue {
    std::string  token;              // plaintext, shown ONCE
    std::int64_t token_expires_at{0};
};

struct ListFilter {
    std::optional<PlayerState> state;
    std::optional<std::int64_t> group_id;      // exactly this group
    std::optional<std::int64_t> object_id;     // any group of this object
    std::int64_t               limit{0};    // 0 = no limit
    std::int64_t               offset{0};
};

// Why a claim failed. For the audit trail; never shown to the caller.
enum class ClaimFailure { None, NoMatch, Expired, AlreadyUsed, UnknownPlayer, WrongState };

struct ClaimResult {
    bool         ok{false};
    std::int64_t token_id{0};        // valid when ok
    ClaimFailure reason{ClaimFailure::None};
};

struct CertRow {
    std::string                 serial_hex;         // uppercase, no separators
    int                         generation{0};      // issuing CA generation
    std::string                 player_id;
    std::int64_t                issued_at{0};
    std::int64_t                not_before{0};
    std::int64_t                not_after{0};
    std::optional<std::int64_t> revoked_at;
    std::string                 revoke_reason;
};

struct StatusRow {
    std::string  player_id;
    std::int64_t updated_at{0};
    bool         online{false};
    std::string  version;
    std::string  payload_json;       // last heartbeat body, at most 64 KiB
};

struct EventRow {
    std::int64_t id{0};
    std::string  player_id;
    std::int64_t ts{0};
    std::string  kind;
    std::string  details;
};

class PlayersDb {
public:
    static constexpr int kSchemaVersion = 3;
    static constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
    static constexpr std::size_t kMaxNameBytes = 120;
    static constexpr std::int64_t kMaxTokenTtlSeconds = 365LL * 86400;
    static constexpr std::size_t kMaxSettingBytes = 32 * 1024;

    explicit PlayersDb(std::filesystem::path path);
    ~PlayersDb();
    PlayersDb(const PlayersDb&)            = delete;
    PlayersDb& operator=(const PlayersDb&) = delete;

    // Opens (and creates, mode 0600) the database. Refuses — without touching
    // the file — one written by a newer schema version.
    bool open();
    bool ok() const noexcept { return db_ != nullptr; }
    int  schemaVersion();

    // ── players ──────────────────────────────────────────────────────────
    // nullopt if the name (trimmed, 1..120 bytes, no control characters), the
    // clock policy ("strict" | "relaxed") or the token lifetime (1 s .. 1 year)
    // is invalid.
    std::optional<CreatedPlayer> createPlayer(const std::string& name,
                                              std::optional<std::int64_t> created_by,
                                              const std::string& clock_policy,
                                              std::int64_t token_ttl_seconds,
                                              std::int64_t now = 0,
                                              std::optional<std::int64_t> group_id = std::nullopt);
    std::optional<PlayerRow>     findPlayer(const std::string& id);
    std::vector<PlayerRow>       listPlayers(const ListFilter& filter);

    // Revokes every live certificate, removes the player's tokens and status,
    // deletes the row. Certificate records and events stay. False if unknown.
    bool deletePlayer(const std::string& id, const std::string& reason, std::int64_t now = 0);

    // Marks the player revoked and revokes its live certificates. Returns the
    // certificates revoked by THIS call (empty if there were none or the player
    // was already revoked); nullopt if the player is unknown.
    std::optional<std::vector<CertRow>> revokePlayer(const std::string& id,
                                                     const std::string& reason,
                                                     std::int64_t now = 0);

    // ── pairing ──────────────────────────────────────────────────────────
    // A new token for a still-pending player; the old ones stop working.
    std::optional<TokenIssue> reissueToken(const std::string& player_id,
                                           std::int64_t token_ttl_seconds,
                                           std::int64_t now = 0);
    ClaimResult claimToken(const std::string& player_id, const std::string& token,
                           std::int64_t now = 0);
    bool        releaseClaim(std::int64_t token_id);
    // In ONE transaction: checks the claim, records the certificate, makes the
    // player active, writes the "enrolled" event. All or nothing.
    bool        finishEnrollment(std::int64_t token_id, const CertRow& cert, std::int64_t now = 0);
    // A renewed certificate for an active player.
    bool        recordRenewal(const CertRow& cert, std::int64_t now = 0);

    // ── certificates and CRLs ────────────────────────────────────────────
    std::vector<CertRow> certsOf(const std::string& player_id);
    bool                 revokeCert(int generation, const std::string& serial_hex,
                                    const std::string& reason, std::int64_t now = 0);
    // Revoked certificates of one CA generation that have not expired yet.
    std::vector<CertRow> revokedSerials(int generation, std::int64_t now = 0);
    // The next CRL number for a generation: 1, 2, 3 ... never repeats, never
    // goes down, survives restarts. 0 on failure.
    std::int64_t         nextCrlNumber(int generation);
    // Every revoked certificate that has not expired yet, all CA generations
    // together: what a revocation list is rebuilt from at startup.
    std::vector<CertRow> revokedCerts(std::int64_t now = 0);
    // Deletes certificate records whose notAfter is before `now`. Returns how many.
    std::size_t          purgeExpiredCerts(std::int64_t now = 0);

    // ── objects and groups ───────────────────────────────────────────────
    // Names are trimmed, 1..120 bytes, no control characters. An object name is
    // unique everywhere, a group name within its object. Ids are never reused
    // (access grants refer to them). Everything here is one transaction.
    std::optional<ObjectRow> createObject(const std::string& name, GroupResult* result = nullptr,
                                          std::int64_t now = 0);
    GroupResult              renameObject(std::int64_t id, const std::string& name);
    GroupResult              deleteObject(std::int64_t id);             // NotEmpty while it has groups
    std::optional<ObjectRow> findObject(std::int64_t id);
    std::vector<ObjectRow>   listObjects();

    std::optional<GroupRow>  createGroup(std::int64_t object_id, const std::string& name,
                                         GroupResult* result = nullptr, std::int64_t now = 0);
    GroupResult              renameGroup(std::int64_t id, const std::string& name);
    GroupResult              deleteGroup(std::int64_t id);              // NotEmpty while it has players
    std::optional<GroupRow>  findGroup(std::int64_t id);
    std::vector<GroupRow>    listGroups(std::optional<std::int64_t> object_id);

    // Moves a player to another group (any object) and leaves a "moved" event
    // with the old and new group. Who may do it is decided above this layer.
    GroupResult              movePlayer(const std::string& player_id, std::int64_t group_id,
                                        std::int64_t now = 0);
    // Players per group id (groups without players are absent).
    std::map<std::int64_t, std::int64_t> playerCountByGroup();

    // ── settings ─────────────────────────────────────────────────────────
    // Plain key/value storage for the module's settings (server URLs, token
    // lifetime, ...). What the keys mean and which values are valid is the
    // business of PlayersService; this layer only stores them.
    std::map<std::string, std::string> settings();
    // Upserts the given keys in one transaction; an empty value removes the
    // key. A value over kMaxSettingBytes fails the whole call.
    bool setSettings(const std::map<std::string, std::string>& values);

    // ── last known state and events ──────────────────────────────────────
    // One transaction. Rows for unknown players are skipped; an oversized
    // payload fails the whole batch (nothing is written).
    bool                     applyStatuses(const std::vector<StatusRow>& rows);
    std::optional<StatusRow> status(const std::string& player_id);
    std::vector<StatusRow>   allStatuses();

    bool                  addEvent(const std::string& player_id, const std::string& kind,
                                   const std::string& details, std::int64_t now = 0);
    std::vector<EventRow> events(const std::string& player_id, std::size_t limit);
    std::size_t           purgeEventsOlderThan(std::int64_t ts);

private:
    std::filesystem::path path_;
    sqlite3*              db_{nullptr};
    std::mutex            mu_;
};

}  // namespace liveqx::players
