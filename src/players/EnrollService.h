#pragma once
//
// Pairing (A4): POST /api/players/enroll. A player that holds a one-time
// bootstrap token and a CSR gets a client certificate from the device CA.
//
// The service owns the policy, the HTTP layer only carries bytes:
//   * rate limit per client address AND per player id, checked first, so a
//     locked address never gets to learn anything about a token;
//   * every token failure is the same 401 invalid_token for the caller — the
//     real reason is returned only in `audit_reason`;
//   * the token is claimed only after everything that can fail for server
//     reasons (CA missing, server clock unsynced) has passed, and a CSR that
//     turns out unusable releases the claim again, so the player can retry;
//   * the key never reaches the server; the CN is always the player id.

#include "players/PlayersDb.h"
#include "utils/DeviceCa.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace liveqx::auth { class UnlockLimiter; }

namespace liveqx::players {

struct EnrollRequest {
    std::string player_id;
    std::string bootstrap_token;
    std::string csr_pem;
    std::string client_ip;
};

struct EnrollOutcome {
    int            status{500};
    nlohmann::json body;                 // what goes on the wire
    int            retry_after_s{0};     // Retry-After for 429
    // For the audit row only:
    std::string    audit_action;         // player.enroll | player.enroll_denied | player.enroll_failed
    std::string    audit_reason;
    std::string    player_id;            // as supplied, if it looks like an id
};

struct EnrollOptions {
    // The device CA, or null while it is not set up (-> 503 ca_unavailable).
    std::function<std::shared_ptr<const tls::DeviceCa>()> ca;
    // |OS clock - NTP| in seconds when NTP is configured and measured;
    // nullopt = unknown (no protection, and that is fine).
    std::function<std::optional<std::int64_t>()> clock_offset_s;
    std::int64_t max_clock_skew_s{86400};   // the notBefore margin (D-8)
    int          validity_days{365};
    std::function<std::int64_t()> now;      // unix seconds; null = real clock
};

class EnrollService {
public:
    static constexpr int kSchema = 1;
    static constexpr std::size_t kMaxCsrBytes = 4096;   // A10

    EnrollService(PlayersDb& db, EnrollOptions opts);
    ~EnrollService();

    EnrollOutcome enroll(const EnrollRequest& req);

private:
    std::int64_t nowSeconds() const;

    PlayersDb&    db_;
    EnrollOptions opts_;
    std::unique_ptr<auth::UnlockLimiter> limiter_;
};

}  // namespace liveqx::players
