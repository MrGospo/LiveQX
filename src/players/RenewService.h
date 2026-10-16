#pragma once
//
// Certificate renewal (A5): POST /api/players/{id}/renew over mTLS. The device
// port has already proved who the caller is; this decides whether a new
// certificate is issued.
//
//   * the window opens 90 days before the presented certificate ends (the
//     player starts at 60); earlier -> 409 renew_too_early;
//   * the new certificate is recorded together with a "renewed" event;
//   * the OLD certificate is not revoked: a player that dies before saving the
//     new one must still be able to come back. If the old key is compromised,
//     the administrator revokes the player (all certificates);
//   * the answer also carries the current server-TLS root (`ca_cert_pem`), so
//     a rotated root reaches players that are already paired.

#include "players/PlayersDb.h"
#include "utils/DeviceCa.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace liveqx::players {

struct RenewRequest {
    std::string player_id;          // CN of the presented certificate
    std::string presented_serial;   // serial of the presented certificate
    std::string csr_pem;
};

struct RenewOutcome {
    int            status{500};
    nlohmann::json body;
    std::string    audit_action;    // player.renew | player.renew_denied | player.renew_failed
    std::string    audit_reason;
};

struct RenewOptions {
    std::function<std::shared_ptr<const tls::DeviceCa>()> ca;
    std::function<std::optional<std::int64_t>()>          clock_offset_s;
    std::function<std::string()>                          ca_cert_pem;   // current server-TLS root
    std::int64_t max_clock_skew_s{86400};
    int          validity_days{365};
    int          window_days{90};
    std::function<std::int64_t()> now;                                   // null = real clock
};

class RenewService {
public:
    static constexpr int kSchema = 1;
    static constexpr std::size_t kMaxCsrBytes = 4096;

    RenewService(PlayersDb& db, RenewOptions opts);

    RenewOutcome renew(const RenewRequest& req);

private:
    std::int64_t nowSeconds() const;

    PlayersDb&   db_;
    RenewOptions opts_;
};

}  // namespace liveqx::players
