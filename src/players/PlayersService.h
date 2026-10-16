#pragma once
//
// Domain layer of the player integration: sits between the REST handlers and
// state/players.db.
//
//   * settings of the module (server / device-API / broker URLs, token
//     lifetime, default clock policy, optional CA override) with validation;
//   * creating a player and assembling the pairing package (A3) — the one
//     place where the plaintext token leaves the process;
//   * revoking / deleting a player and telling the CRL machinery which
//     certificates were revoked by that call.
//
// Nothing here knows about HTTP or the audit log; handlers translate the
// returned error codes and write the audit trail.

#include "events/EventBus.h"
#include "players/PlayersDb.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::players {

struct PlayersSettings {
    std::string server_url;          // https://host[:port]   enroll + admin
    std::string device_api_url;      // https://host[:port]   heartbeat, renew (mTLS)
    std::string mqtt_broker_url;     // mqtts://host[:port]
    std::string ca_cert_pem;         // optional override of the server TLS root
    int         token_ttl_days{7};
    std::string clock_policy{"strict"};   // default for new players
};

struct PlayersServiceOptions {
    // The server's TLS root; used when no override is set.
    std::filesystem::path ca_cert_path;
    events::EventBus*     events{nullptr};
    // NTP servers ("host" or "host:port") the server itself uses; empty =
    // NTP is not configured and the package carries no ntp_hint.
    std::function<std::vector<std::string>()> ntp_hint;
    // Called with the certificates revoked by a revoke/remove call, so the
    // CRL can be rebuilt and the broker told. Not called for an empty set.
    std::function<void(const std::vector<CertRow>&)> on_certs_revoked;
};

struct PairingResult {
    bool        ok{false};
    std::string error;               // snake_case code when !ok
    PlayerRow   player;
    nlohmann::json package;          // the A3 package (contains the token!)
};

struct RemovalResult {
    bool        found{false};
    std::size_t revoked_certs{0};
};

class PlayersService {
public:
    static constexpr int kPackageSchema = 1;

    PlayersService(PlayersDb& db, PlayersServiceOptions opts);

    // The store, for read-only views (lists, details, events).
    PlayersDb& db() noexcept { return db_; }

    PlayersSettings settings();

    // The root players must trust for the server's TLS: the explicit override
    // if set, else the server's own CA file. Empty if neither is usable.
    std::string caCertPem();
    // JSON view of the settings (what GET /api/players/settings returns).
    static nlohmann::json toJson(const PlayersSettings& s);

    // Applies a patch: key -> new value, empty value = reset to default.
    // All-or-nothing. Returns "" or an error code: unknown_setting,
    // invalid_server_url, invalid_device_api_url, invalid_mqtt_broker_url,
    // invalid_ca_cert_pem, invalid_token_ttl_days, invalid_clock_policy,
    // storage_error.
    std::string updateSettings(const std::map<std::string, std::string>& patch);

    // Errors: invalid_clock_policy, invalid_name, not_configured,
    // ca_unavailable, storage_error. `now` = 0 reads the clock.
    PairingResult create(const std::string& name, std::optional<std::int64_t> created_by,
                         std::optional<std::string> clock_policy, std::int64_t now = 0);

    // A new token (and package) for a pending player. Errors: not_found,
    // not_pending, not_configured, ca_unavailable, storage_error.
    PairingResult reissueToken(const std::string& player_id, std::int64_t now = 0);

    RemovalResult revoke(const std::string& player_id, const std::string& reason,
                         std::int64_t now = 0);
    RemovalResult remove(const std::string& player_id, const std::string& reason,
                         std::int64_t now = 0);

private:
    // Fills `out` (the package) or returns an error code.
    std::string buildPackage(const PlayerRow& p, const std::string& token,
                             std::int64_t expires_at, nlohmann::json& out);
    std::string checkReady(const PlayersSettings& s, std::string& ca_pem);

    PlayersDb&            db_;
    PlayersServiceOptions opts_;
};

}  // namespace liveqx::players
