#pragma once
//
// The certificates of the whole fleet seen together: how many screens stand on which step,
// which end first, and whether screens that ought to be renewing are failing to — plus the
// server's own certificates, whose end takes every screen down with it.
//
// The same numbers feed the summary page (`GET /api/players/lifecycle`, narrowed to what
// the person may see) and the Prometheus scrape (the whole fleet).

#include "players/CertLifecycle.h"
#include "players/PlayersDb.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::players {

// A screen that is switched on starts to renew this many days before the end of its
// certificate (the server takes a renewal from 90). Screens inside this window "ought to
// be renewing now".
inline constexpr int kRenewWindowDays = 60;

// One system-wide alarm instead of one per screen: at least this many screens, and at least
// half of those that are on and ought to be renewing, report that their last renewal failed.
inline constexpr std::int64_t kRenewAlarmMinFailing = 3;
bool renewAlarm(std::int64_t failing, std::int64_t renewing_online) noexcept;

struct EarliestCert {
    std::string  player_id;
    std::string  name;
    std::int64_t not_after{0};
};

struct RenewFailure {
    std::string                 player_id;
    std::string                 name;
    CertStage                   stage{CertStage::Ok};
    std::optional<std::int64_t> not_after;
    std::string                 error;               // what the screen itself reported (cut by the device API)
};

struct ScreenCertSummary {
    std::array<std::int64_t, 5> by_stage{};          // indexed by CertStage
    std::int64_t                bound{0};
    std::optional<EarliestCert> earliest;            // the soonest end among screens whose certificate has not run out
    std::int64_t                renewing{0};         // not over, and inside the renewal window
    std::int64_t                renewing_online{0};  // ... of those, on and reporting
    std::int64_t                renewing_offline{0}; // ... off or silent: they cannot renew, that is a different cause
    std::int64_t                failing{0};          // ... of the online ones, the last renewal failed
    std::vector<RenewFailure>   failures;            // those, soonest end first (all of them; the API cuts)
    bool                        systemic{false};     // renewAlarm(failing, renewing_online)

    std::int64_t count(CertStage s) const { return by_stage[static_cast<std::size_t>(s)]; }
};

// Bound (active) screens only. `visible` narrows the set to what a person may see; empty = all.
ScreenCertSummary summarizeScreens(PlayersDb& db, std::int64_t now,
                                   const std::function<bool(const PlayerRow&)>& visible = {});

// A certificate of the server itself. `not_after` 0 = unknown, and `error` says why.
struct ServerCertEntry {
    std::string  name;             // api_tls | device_ca | device_ca_root | broker_tls | crl
    std::int64_t not_after{0};
    std::string  error;
};

nlohmann::json screenSummaryJson(const ScreenCertSummary& s, std::int64_t now, std::size_t max_failures = 20);
nlohmann::json serverCertsJson(const std::vector<ServerCertEntry>& certs, std::int64_t now);

// Prometheus text (version 0.0.4) for the fleet and the server's certificates.
std::string certMetricsText(const ScreenCertSummary& screens, const std::vector<ServerCertEntry>& server,
                            std::int64_t now);

}  // namespace liveqx::players
