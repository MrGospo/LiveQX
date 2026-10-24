#include "players/CertWatch.h"

#include "players/CertLifecycle.h"
#include "utils/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace liveqx::players {

namespace {
// Kept in the module's settings table (an empty value removes the key, and "nothing" means "fine").
constexpr const char* kAlarmKey            = "watch.renew_alarm";
constexpr const char* kServerCertKeyPrefix = "watch.server_cert.";
}  // namespace

CertWatch::CertWatch(PlayersDb& db, CertWatchOptions opts) : db_(db), opts_(std::move(opts)) {
    opts_.interval_s = std::max(1, opts_.interval_s);
    opts_.first_look_after_s = std::max(0, opts_.first_look_after_s);

    // What was announced before the server last stopped, so a fault that is still there is not announced twice.
    const auto saved = db_.settings();
    if (auto it = saved.find(kAlarmKey); it != saved.end()) alarm_ = it->second == "1";
    for (const auto& [key, value] : saved)
        if (key.rfind(kServerCertKeyPrefix, 0) == 0)
            if (auto stage = parseCertStage(value)) server_stage_[key.substr(std::string(kServerCertKeyPrefix).size())] = *stage;
}

CertWatch::~CertWatch() { stop(); }

void CertWatch::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void CertWatch::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void CertWatch::run() {
    std::unique_lock<std::mutex> lk(mu_);
    int wait_s = opts_.first_look_after_s;
    while (!stopping_) {
        if (cv_.wait_for(lk, std::chrono::seconds(wait_s), [&] { return stopping_; })) break;
        lk.unlock();
        try {
            scan();
        } catch (const std::exception& e) {
            LOG_ERROR("Certificate watch: {}", e.what());
        }
        lk.lock();
        wait_s = opts_.interval_s;
    }
}

std::size_t CertWatch::scan() {
    using nlohmann::json;
    const std::int64_t now = opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
    const auto ends = db_.liveCertEnds();
    const auto last = db_.lastCertStages();

    ListFilter only_active;
    only_active.state = PlayerState::Active;
    std::size_t changed = 0;
    for (const auto& p : db_.listPlayers(only_active)) {
        std::optional<std::int64_t> end;
        if (auto it = ends.find(p.id); it != ends.end()) end = it->second;
        const CertStage stage = certStageOf(end, now);

        CertStage before = CertStage::Ok;                       // never written = nothing to report yet
        if (auto it = last.find(p.id); it != last.end())
            if (auto parsed = parseCertStage(it->second)) before = *parsed;
        if (stage == before) continue;

        json details = {{"stage", certStageName(stage)}, {"previous", certStageName(before)}};
        if (end) details["not_after"] = *end;
        if (!db_.addEvent(p.id, "cert_stage", details.dump(), now)) continue;   // not written: look again next time
        ++changed;

        if (opts_.events) {
            json payload = {{"player_id", p.id}, {"name", p.name}, {"stage", certStageName(stage)},
                            {"previous", certStageName(before)}};
            if (end) {
                payload["not_after"] = *end;
                payload["days_left"] = certDaysLeft(*end, now);
            }
            if (auto place = db_.placeOfPlayer(p.id)) {          // so that whoever sees part of the tree gets its own news
                payload["group_id"]  = place->group_id;
                payload["object_id"] = place->object_id;
            }
            opts_.events->publish(events::EventType::PlayerCertStage, -1, std::move(payload));
        }
        if (stage == CertStage::Expired)
            LOG_WARN("Certificate of screen '{}' has run out: it can only be bound again", p.name);
        else if (certNeedsAttention(stage))
            LOG_WARN("Certificate of screen '{}' ends in {} day(s) and it has not renewed", p.name,
                     end ? certDaysLeft(*end, now) : 0);
    }

    // Renewal failing across the fleet is one fault, not one per screen: say so once, and when it ends.
    const ScreenCertSummary fleet = summarizeScreens(db_, now);
    if (fleet.systemic != alarm_) {
        alarm_ = fleet.systemic;
        db_.setSettings({{kAlarmKey, alarm_ ? "1" : ""}});
        if (opts_.events)
            opts_.events->publish(events::EventType::PlayersRenewAlarm, -1,
                                  {{"active", alarm_}, {"failing", fleet.failing},
                                   {"online", fleet.renewing_online}, {"window_days", kRenewWindowDays}});
        if (alarm_)
            LOG_ERROR("Certificate renewal is failing on {} of {} screens that are on: a common fault, not single screens",
                      fleet.failing, fleet.renewing_online);
        else
            LOG_INFO("Certificate renewal is no longer failing across the fleet");
        if (opts_.on_renew_alarm) opts_.on_renew_alarm(alarm_, fleet);
    }

    if (opts_.server_certs) {
        for (const auto& c : opts_.server_certs()) {
            if (c.not_after <= 0) continue;                  // no date (missing or unreadable): the page says why
            const CertStage stage = certStageOf(c.not_after, now);
            auto [it, fresh] = server_stage_.emplace(c.name, CertStage::Ok);
            const CertStage previous = it->second;
            (void)fresh;
            if (stage == previous) continue;
            it->second = stage;
            db_.setSettings({{std::string(kServerCertKeyPrefix) + c.name, stage == CertStage::Ok ? "" : certStageName(stage)}});
            const std::int64_t left = certDaysLeft(c.not_after, now);
            if (opts_.events)
                opts_.events->publish(events::EventType::PlayersServerCert, -1,
                                      {{"name", c.name}, {"stage", certStageName(stage)}, {"previous", certStageName(previous)},
                                       {"not_after", c.not_after}, {"days_left", left}});
            if (stage == CertStage::Expired)
                LOG_ERROR("Certificate of the server ({}) has run out", c.name);
            else if (certNeedsAttention(stage))
                LOG_WARN("Certificate of the server ({}) ends in {} day(s)", c.name, left);
            else
                LOG_INFO("Certificate of the server ({}) is fine again", c.name);
            if (opts_.on_server_cert) opts_.on_server_cert(c, stage, previous, left);
        }
    }
    return changed;
}

}  // namespace liveqx::players
