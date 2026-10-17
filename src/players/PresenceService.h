#pragma once
//
// Who is online (A8, B6). Decided from heartbeats: a player is online while its
// heartbeats keep coming, and offline after `offline_after_s` of silence
// (about three missed intervals plus a short grace).
//
// Heartbeats arrive far more often than anything changes, so:
//   * the steady stream is kept in memory and written to the database in one
//     batch every `flush_interval_s`;
//   * the transitions that matter — first sight, offline, back online, version
//     change — are written to the event log and announced over SSE at the
//     moment they happen, and a transition stores the status at once.
//
// A restart must not look like a mass outage: nobody is judged offline before
// one full threshold has passed since startup, so players get the chance to
// report in first. Whatever was not flushed when the process died (at most one
// flush interval of heartbeats) is lost, which only makes a player look a few
// seconds older than it is.
//
// Presence from the MQTT broker (stage 7) joins here as a second source.

#include "events/EventBus.h"
#include "players/PlayersDb.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace liveqx::players {

struct PresenceOptions {
    std::int64_t offline_after_s{195};      // 3 x 60 s + 15 s grace
    std::int64_t flush_interval_s{5};       // 0 = write every heartbeat at once
    std::int64_t tick_period_ms{1000};      // the background thread's period
    events::EventBus* events{nullptr};
    std::function<std::int64_t()> now;      // unix seconds; null = real clock
};

class PresenceService {
public:
    PresenceService(PlayersDb& db, PresenceOptions opts);
    ~PresenceService();
    PresenceService(const PresenceService&)            = delete;
    PresenceService& operator=(const PresenceService&) = delete;

    // An accepted heartbeat. `payload_json` is the already sanitised body.
    // `startup` marks the first heartbeat after the player process started
    // (A8): it leaves a "started" event, which also shows restarts that happen
    // faster than the offline threshold. Unknown players are ignored.
    void onHeartbeat(const std::string& player_id, const std::string& version,
                     const std::string& payload_json, bool startup = false);

    struct TickResult {
        std::size_t flushed{0};
        std::size_t went_offline{0};
    };
    // Writes the batch if it is due and turns the silent offline. Called by
    // the background thread; tests call it directly.
    TickResult tick();

    void start();
    void stop();

    bool isOnline(const std::string& player_id) const;

private:
    struct Entry {
        std::string  name;
        bool         online{false};
        std::string  version;
        std::string  payload_json;
        std::int64_t updated_at{0};
        bool         dirty{false};
    };
    struct Note {                           // something to record and announce
        enum Kind { Online, Offline, VersionChanged, Started } kind;
        std::string  player_id, name, from, to;
        std::int64_t last_seen{0};
    };

    std::int64_t nowSeconds() const;
    void         publish(const std::vector<Note>& notes);
    StatusRow    rowOf(const std::string& id, const Entry& e) const;

    PlayersDb&       db_;
    PresenceOptions  opts_;
    std::int64_t     boot_at_{0};

    mutable std::mutex                     mu_;
    std::unordered_map<std::string, Entry> entries_;
    std::int64_t                           last_flush_{0};

    std::thread             thread_;
    std::mutex              stop_mu_;
    std::condition_variable stop_cv_;
    bool                    stopping_{false};
};

}  // namespace liveqx::players
