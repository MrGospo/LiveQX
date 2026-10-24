#include "players/CertSummary.h"

#include <algorithm>
#include <sstream>

namespace liveqx::players {

namespace {

constexpr std::int64_t kDay = 86400;

std::size_t idx(CertStage s) { return static_cast<std::size_t>(s); }

// What the screen said about its last renewal in its last report; "" when nothing.
std::string lastRenewError(const std::string& payload_json) {
    const auto j = nlohmann::json::parse(payload_json, nullptr, false);
    if (!j.is_object()) return {};
    auto it = j.find("last_renew_error");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool renewAlarm(std::int64_t failing, std::int64_t renewing_online) noexcept {
    return failing >= kRenewAlarmMinFailing && failing * 2 >= renewing_online;
}

ScreenCertSummary summarizeScreens(PlayersDb& db, std::int64_t now,
                                   const std::function<bool(const PlayerRow&)>& visible) {
    ScreenCertSummary out;
    const auto ends = db.liveCertEnds();
    ListFilter only_active;
    only_active.state = PlayerState::Active;
    for (const auto& p : db.listPlayers(only_active)) {
        if (visible && !visible(p)) continue;
        std::optional<std::int64_t> end;
        if (auto it = ends.find(p.id); it != ends.end()) end = it->second;
        const CertStage stage = certStageOf(end, now);
        ++out.by_stage[idx(stage)];
        ++out.bound;
        if (stage == CertStage::Expired || !end) continue;       // nothing to renew with, and no end to show

        if (!out.earliest || *end < out.earliest->not_after) out.earliest = EarliestCert{p.id, p.name, *end};
        if (*end - now > kRenewWindowDays * kDay) continue;

        ++out.renewing;
        const auto st = db.status(p.id);
        if (!st || !st->online) { ++out.renewing_offline; continue; }
        ++out.renewing_online;
        if (auto err = lastRenewError(st->payload_json); !err.empty()) {
            ++out.failing;
            out.failures.push_back(RenewFailure{p.id, p.name, stage, end, std::move(err)});
        }
    }
    std::sort(out.failures.begin(), out.failures.end(), [](const RenewFailure& a, const RenewFailure& b) {
        return a.not_after != b.not_after ? a.not_after < b.not_after : a.player_id < b.player_id;
    });
    out.systemic = renewAlarm(out.failing, out.renewing_online);
    return out;
}

nlohmann::json screenSummaryJson(const ScreenCertSummary& s, std::int64_t now, std::size_t max_failures) {
    using nlohmann::json;
    json counts = json::object();
    for (auto st : {CertStage::Ok, CertStage::Soon30, CertStage::Soon14, CertStage::Soon3, CertStage::Expired})
        counts[certStageName(st)] = s.count(st);

    json failures = json::array();
    for (std::size_t i = 0; i < s.failures.size() && i < max_failures; ++i) {
        const auto& f = s.failures[i];
        json o = {{"player_id", f.player_id}, {"name", f.name}, {"stage", certStageName(f.stage)}, {"error", f.error}};
        if (f.not_after) {
            o["not_after"] = *f.not_after;
            o["days_left"] = certDaysLeft(*f.not_after, now);
        }
        failures.push_back(std::move(o));
    }
    json earliest = nullptr;
    if (s.earliest)
        earliest = {{"player_id", s.earliest->player_id}, {"name", s.earliest->name},
                    {"not_after", s.earliest->not_after}, {"days_left", certDaysLeft(s.earliest->not_after, now)}};
    return {{"bound", s.bound},
            {"counts", std::move(counts)},
            {"earliest", std::move(earliest)},
            {"renew", {{"window_days", kRenewWindowDays},
                       {"renewing", s.renewing},
                       {"online", s.renewing_online},
                       {"offline", s.renewing_offline},
                       {"failing", s.failing},
                       {"systemic", s.systemic},
                       {"failures", std::move(failures)}}}};
}

nlohmann::json serverCertsJson(const std::vector<ServerCertEntry>& certs, std::int64_t now) {
    using nlohmann::json;
    json arr = json::array();
    for (const auto& c : certs) {
        json o = {{"name", c.name}};
        if (c.not_after > 0) {
            o["not_after"] = c.not_after;
            o["days_left"] = certDaysLeft(c.not_after, now);
            o["stage"]     = certStageName(certStageOf(c.not_after, now));
        }
        if (!c.error.empty()) o["error"] = c.error;
        arr.push_back(std::move(o));
    }
    return arr;
}

std::string certMetricsText(const ScreenCertSummary& s, const std::vector<ServerCertEntry>& server,
                            std::int64_t now) {
    std::ostringstream out;
    out << "# HELP liveqx_players_cert_stage Bound screens by how close their certificate is to its end.\n"
           "# TYPE liveqx_players_cert_stage gauge\n";
    for (auto st : {CertStage::Ok, CertStage::Soon30, CertStage::Soon14, CertStage::Soon3, CertStage::Expired})
        out << "liveqx_players_cert_stage{stage=\"" << certStageName(st) << "\"} " << s.count(st) << "\n";

    if (s.earliest) {
        out << "# HELP liveqx_players_cert_min_days_remaining Whole days left on the soonest-ending certificate among screens that still have a valid one.\n"
               "# TYPE liveqx_players_cert_min_days_remaining gauge\n"
               "liveqx_players_cert_min_days_remaining "
            << certDaysLeft(s.earliest->not_after, now) << "\n";
    }
    out << "# HELP liveqx_players_renewing Bound screens inside the renewal window (they ought to be renewing now).\n"
           "# TYPE liveqx_players_renewing gauge\n"
           "liveqx_players_renewing " << s.renewing << "\n"
           "# HELP liveqx_players_renew_failing Of the renewing screens that are on, those whose last renewal failed.\n"
           "# TYPE liveqx_players_renew_failing gauge\n"
           "liveqx_players_renew_failing " << s.failing << "\n"
           "# HELP liveqx_players_renew_offline Renewing screens that are off or silent, so cannot renew.\n"
           "# TYPE liveqx_players_renew_offline gauge\n"
           "liveqx_players_renew_offline " << s.renewing_offline << "\n"
           "# HELP liveqx_players_renew_alarm 1 when renewals are failing across the fleet, not on single screens.\n"
           "# TYPE liveqx_players_renew_alarm gauge\n"
           "liveqx_players_renew_alarm " << (s.systemic ? 1 : 0) << "\n";

    bool header = false;
    for (const auto& c : server) {
        if (c.not_after <= 0) continue;
        if (!header) {
            out << "# HELP liveqx_server_cert_days_remaining Whole days left on a certificate of the server itself (negative once over).\n"
                   "# TYPE liveqx_server_cert_days_remaining gauge\n";
            header = true;
        }
        out << "liveqx_server_cert_days_remaining{name=\"" << c.name << "\"} " << certDaysLeft(c.not_after, now) << "\n";
    }
    return out.str();
}

}  // namespace liveqx::players
