#include "players/PresenceService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

namespace liveqx::players {

PresenceService::PresenceService(PlayersDb& db, PresenceOptions opts)
    : db_(db), opts_(std::move(opts)) {
    boot_at_    = nowSeconds();
    last_flush_ = boot_at_;

    // Remember what the database knows, so a restart neither forgets who was
    // online nor reports a version as new.
    std::unordered_map<std::string, std::string> names;
    for (const auto& p : db_.listPlayers({})) names[p.id] = p.name;
    for (const auto& st : db_.allStatuses()) {
        auto it = names.find(st.player_id);
        if (it == names.end()) continue;
        Entry e;
        e.name         = it->second;
        e.online       = st.online;
        e.version      = st.version;
        e.payload_json = st.payload_json;
        e.updated_at   = st.updated_at;
        entries_.emplace(st.player_id, std::move(e));
    }
}

PresenceService::~PresenceService() { stop(); }

std::int64_t PresenceService::nowSeconds() const {
    return opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

StatusRow PresenceService::rowOf(const std::string& id, const Entry& e) const {
    StatusRow r;
    r.player_id    = id;
    r.updated_at   = e.updated_at;
    r.online       = e.online;
    r.version      = e.version;
    r.payload_json = e.payload_json;
    return r;
}

void PresenceService::publish(const std::vector<Note>& notes) {
    using nlohmann::json;
    // Every announcement says where the player sits NOW, so that whoever sees
    // only part of the tree gets only its own news.
    auto announce = [&](events::EventType type, const std::string& player_id, json payload) {
        if (!opts_.events) return;
        if (auto place = db_.placeOfPlayer(player_id)) {
            payload["group_id"]  = place->group_id;
            payload["object_id"] = place->object_id;
        }
        opts_.events->publish(type, -1, std::move(payload));
    };
    for (const auto& n : notes) {
        switch (n.kind) {
        case Note::Online:
            db_.addEvent(n.player_id, "online", "", nowSeconds());
            announce(events::EventType::PlayerOnline, n.player_id,
                     {{"player_id", n.player_id}, {"name", n.name}});
            break;
        case Note::Offline:
            db_.addEvent(n.player_id, "offline",
                         json{{"reason", n.reason}, {"last_seen", n.last_seen}}.dump(), nowSeconds());
            announce(events::EventType::PlayerOffline, n.player_id,
                     {{"player_id", n.player_id}, {"name", n.name}, {"reason", n.reason}});
            break;
        case Note::Started:
            db_.addEvent(n.player_id, "started", "", nowSeconds());
            break;
        case Note::VersionChanged:
            db_.addEvent(n.player_id, "version_changed", json{{"from", n.from}, {"to", n.to}}.dump(), nowSeconds());
            announce(events::EventType::PlayerVersionChanged, n.player_id,
                     {{"player_id", n.player_id}, {"name", n.name}, {"from", n.from}, {"to", n.to}});
            break;
        }
    }
}

void PresenceService::onHeartbeat(const std::string& player_id, const std::string& version,
                                  const std::string& payload_json, bool startup) {
    const std::int64_t now = nowSeconds();

    // A player seen for the first time since startup: find out who it is
    // before taking the lock (a database read).
    bool known;
    {
        std::lock_guard<std::mutex> lk(mu_);
        known = entries_.count(player_id) != 0;
    }
    std::string name;
    std::optional<StatusRow> stored;
    if (!known) {
        auto p = db_.findPlayer(player_id);
        if (!p) return;                                  // not (or no longer) a player
        name   = p->name;
        stored = db_.status(player_id);
    }

    std::vector<Note> notes;
    StatusRow write_now;
    bool do_write = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(player_id);
        if (it == entries_.end()) {
            Entry e;
            e.name = name;
            if (stored) {
                e.online       = stored->online;
                e.version      = stored->version;
                e.payload_json = stored->payload_json;
                e.updated_at   = stored->updated_at;
            }
            it = entries_.emplace(player_id, std::move(e)).first;
        }
        Entry& e = it->second;

        const bool was_online = e.online;
        const std::string prev_version = e.version;
        e.online       = true;
        e.updated_at   = now;
        e.mqtt_offline_at = 0;                 // a heartbeat contradicts a broker "offline"
        e.payload_json = payload_json;
        if (!version.empty()) e.version = version;
        e.dirty = true;

        if (!was_online) notes.push_back({Note::Online, player_id, e.name, "", "", 0});
        if (startup) notes.push_back({Note::Started, player_id, e.name, "", "", 0});
        if (!prev_version.empty() && !version.empty() && prev_version != version)
            notes.push_back({Note::VersionChanged, player_id, e.name, prev_version, version, 0});

        // A transition stores the state at once; so does write-through mode.
        if (!notes.empty() || opts_.flush_interval_s <= 0) {
            write_now = rowOf(player_id, e);
            e.dirty   = false;
            do_write  = true;
        }
    }
    if (do_write) db_.applyStatuses({write_now});
    publish(notes);
}

std::optional<std::pair<std::string, bool>> PresenceService::parsePresenceMessage(const std::string& topic,
                                                                                 const std::string& payload) {
    static const std::string kPrefix = "liveqx/v1/players/", kSuffix = "/presence";
    if (topic.size() <= kPrefix.size() + kSuffix.size() || topic.compare(0, kPrefix.size(), kPrefix) != 0 ||
        topic.compare(topic.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        return std::nullopt;
    const std::string id = topic.substr(kPrefix.size(), topic.size() - kPrefix.size() - kSuffix.size());
    if (id.find('/') != std::string::npos || id.empty()) return std::nullopt;
    std::string word;
    for (unsigned char c : payload) if (!std::isspace(c)) word.push_back(static_cast<char>(std::tolower(c)));
    if (word == "online") return std::make_pair(id, true);
    if (word == "offline") return std::make_pair(id, false);
    return std::nullopt;
}

void PresenceService::onMqttPresence(const std::string& player_id, bool online) {
    const std::int64_t now = nowSeconds();
    if (!online) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(player_id);
        if (it != entries_.end() && it->second.online && it->second.mqtt_offline_at == 0)
            it->second.mqtt_offline_at = now;               // judged by tick() after the grace
        return;
    }

    bool known;
    {
        std::lock_guard<std::mutex> lk(mu_);
        known = entries_.count(player_id) != 0;
    }
    std::string name;
    std::optional<StatusRow> stored;
    if (!known) {
        auto p = db_.findPlayer(player_id);
        if (!p || p->state == PlayerState::Revoked) return;   // unknown, or no longer allowed to be a player
        name   = p->name;
        stored = db_.status(player_id);
    }

    std::vector<Note> notes;
    StatusRow write_now;
    bool do_write = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(player_id);
        if (it == entries_.end()) {
            Entry e;
            e.name = name;
            if (stored) {
                e.online       = stored->online;
                e.version      = stored->version;
                e.payload_json = stored->payload_json;
                e.updated_at   = stored->updated_at;
            }
            it = entries_.emplace(player_id, std::move(e)).first;
        }
        Entry& e = it->second;
        e.mqtt_offline_at = 0;
        if (!e.online) {
            e.online     = true;
            e.updated_at = now;
            notes.push_back({Note::Online, player_id, e.name, "", "", 0});
            write_now = rowOf(player_id, e);
            e.dirty   = false;
            do_write  = true;
        }
    }
    if (do_write) db_.applyStatuses({write_now});
    publish(notes);
}

PresenceService::TickResult PresenceService::tick() {
    const std::int64_t now = nowSeconds();
    TickResult result;

    std::vector<Note> notes;
    std::vector<StatusRow> batch;
    std::vector<std::string> stale;       // online but silent: candidates, confirmed outside the lock
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool judging = now >= boot_at_ + opts_.offline_after_s;
        for (const auto& [id, e] : entries_) {
            if (!e.online) continue;
            // The broker said offline and no heartbeat came since: believed after the grace.
            const bool by_broker = e.mqtt_offline_at > 0 && e.updated_at <= e.mqtt_offline_at &&
                                   now - e.mqtt_offline_at >= opts_.mqtt_offline_grace_s;
            if (by_broker || (judging && now - e.updated_at > opts_.offline_after_s)) stale.push_back(id);
        }
    }

    // Players deleted meanwhile just disappear.
    std::vector<std::string> gone;
    for (const auto& id : stale)
        if (!db_.findPlayer(id)) gone.push_back(id);

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& id : gone) entries_.erase(id);
        for (const auto& id : stale) {
            auto it = entries_.find(id);
            if (it == entries_.end() || !it->second.online) continue;
            Entry& e = it->second;
            const bool by_broker = e.mqtt_offline_at > 0 && e.updated_at <= e.mqtt_offline_at &&
                                   now - e.mqtt_offline_at >= opts_.mqtt_offline_grace_s;
            const bool by_silence = now - e.updated_at > opts_.offline_after_s;
            if (!by_broker && !by_silence) continue;                       // it spoke up meanwhile
            e.online = false;
            e.mqtt_offline_at = 0;
            notes.push_back({Note::Offline, id, e.name, "", "", e.updated_at, by_broker && !by_silence ? "mqtt_offline" : "heartbeat_timeout"});
            batch.push_back(rowOf(id, e));
            e.dirty = false;
            ++result.went_offline;
        }
        // The regular batch.
        if (now - last_flush_ >= opts_.flush_interval_s) {
            for (auto& [id, e] : entries_)
                if (e.dirty) { batch.push_back(rowOf(id, e)); e.dirty = false; ++result.flushed; }
            last_flush_ = now;
        }
    }
    if (!batch.empty() && !db_.applyStatuses(batch)) {
        // Keep the data: try again at the next tick.
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& r : batch)
            if (auto it = entries_.find(r.player_id); it != entries_.end()) it->second.dirty = true;
        last_flush_ = 0;
    }
    publish(notes);
    return result;
}

bool PresenceService::isOnline(const std::string& player_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(player_id);
    return it != entries_.end() && it->second.online;
}

void PresenceService::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stopping_ = false;
    }
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(stop_mu_);
        while (!stopping_) {
            lk.unlock();
            tick();
            lk.lock();
            stop_cv_.wait_for(lk, std::chrono::milliseconds(std::max<std::int64_t>(opts_.tick_period_ms, 5)),
                              [this] { return stopping_; });
        }
    });
}

void PresenceService::stop() {
    {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stopping_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

}  // namespace liveqx::players
