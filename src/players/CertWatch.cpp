#include "players/CertWatch.h"

#include "players/CertLifecycle.h"
#include "utils/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace liveqx::players {

CertWatch::CertWatch(PlayersDb& db, CertWatchOptions opts) : db_(db), opts_(std::move(opts)) {
    opts_.interval_s = std::max(1, opts_.interval_s);
    opts_.first_look_after_s = std::max(0, opts_.first_look_after_s);
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
    return changed;
}

}  // namespace liveqx::players
