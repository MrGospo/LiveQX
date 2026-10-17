// players/PresenceService — who is online, decided from heartbeats (A8).
//
// A player is online while its heartbeats keep coming; after about three missed
// intervals plus a short grace it is offline. Heartbeats arrive far more often
// than anything changes, so they are collected in memory and written to the
// database in batches, while the transitions that matter (online, offline,
// version change) are recorded and announced the moment they happen.
//
// What these tests protect:
//   * the first heartbeat makes a player online (one event, one SSE message);
//     the next ones change nothing;
//   * silence longer than the threshold makes it offline exactly once, and a
//     heartbeat after that makes it online again;
//   * a server restart does not turn the whole fleet offline: nobody is judged
//     before one full threshold has passed since startup;
//   * heartbeats are batched (not written one by one), nothing is lost to a
//     normal tick, and a player deleted meanwhile disappears without a trace;
//   * version changes are noticed once.

#include "events/EventBus.h"
#include "players/PresenceService.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace liveqx::players;
using liveqx::events::EventType;

namespace {

constexpr std::int64_t kT0 = 1'800'000'000;

class PresenceTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<PlayersDb> db_;
    liveqx::events::EventBus bus_;
    std::shared_ptr<liveqx::events::Subscription> sub_;
    std::atomic<std::int64_t> now_{kT0};
    std::unique_ptr<PresenceService> svc_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-presence-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        sub_ = bus_.subscribe();
        makeService();
    }
    void TearDown() override {
        svc_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // "The server starts": a new service over the same database.
    void makeService(std::int64_t flush_interval_s = 5) {
        svc_.reset();
        PresenceOptions o;
        o.offline_after_s   = 195;                 // 3 x 60 s + 15 s grace
        o.flush_interval_s  = flush_interval_s;
        o.events            = &bus_;
        o.now               = [this] { return now_.load(); };
        svc_ = std::make_unique<PresenceService>(*db_, std::move(o));
    }
    // Let the batch interval pass and write what has been collected.
    void flush() {
        now_ += 6;
        svc_->tick();
    }

    std::string newPlayer(const std::string& name = "Lobby") {
        auto c = db_->createPlayer(name, std::nullopt, "strict", 86400, now_);
        EXPECT_TRUE(c.has_value());
        return c->player.id;
    }

    std::vector<liveqx::events::Event> events() { return sub_->drain(std::chrono::milliseconds(20)); }
    static int count(const std::vector<liveqx::events::Event>& v, EventType t) {
        int n = 0;
        for (const auto& e : v) n += (e.type == t);
        return n;
    }
    int dbEvents(const std::string& id, const std::string& kind) {
        int n = 0;
        for (const auto& e : db_->events(id, 100)) n += (e.kind == kind);
        return n;
    }
    void beat(const std::string& id, const char* version = "0.3.5") {
        svc_->onHeartbeat(id, version, R"({"health":"ok"})");
    }
};

}  // namespace

TEST_F(PresenceTest, TheFirstHeartbeatMakesAPlayerOnlineOnce) {
    const auto id = newPlayer();
    beat(id);
    auto ev = events();
    ASSERT_EQ(count(ev, EventType::PlayerOnline), 1);
    for (const auto& e : ev)
        if (e.type == EventType::PlayerOnline) {
            EXPECT_EQ(e.payload["player_id"], id);
            EXPECT_EQ(e.payload["name"], "Lobby");
        }
    EXPECT_EQ(dbEvents(id, "online"), 1);

    now_ += 60;
    beat(id);
    now_ += 60;
    beat(id);
    EXPECT_EQ(count(events(), EventType::PlayerOnline), 0) << "staying online is not news";
    EXPECT_EQ(dbEvents(id, "online"), 1);
    EXPECT_TRUE(svc_->isOnline(id));
}

TEST_F(PresenceTest, SilenceMakesAPlayerOfflineExactlyOnce) {
    const auto id = newPlayer();
    beat(id);
    events();                                       // clear
    now_ += 1000;                                   // the player never spoke again
    auto t = svc_->tick();
    EXPECT_EQ(t.went_offline, 1u);
    auto ev = events();
    ASSERT_EQ(count(ev, EventType::PlayerOffline), 1);
    for (const auto& e : ev)
        if (e.type == EventType::PlayerOffline) {
            EXPECT_EQ(e.payload["player_id"], id);
            EXPECT_EQ(e.payload["reason"], "heartbeat_timeout");
        }
    EXPECT_EQ(dbEvents(id, "offline"), 1);
    EXPECT_FALSE(svc_->isOnline(id));
    ASSERT_TRUE(db_->status(id).has_value()) << "the transition is written at once";
    EXPECT_FALSE(db_->status(id)->online);

    now_ += 100;
    EXPECT_EQ(svc_->tick().went_offline, 0u);
    EXPECT_EQ(count(events(), EventType::PlayerOffline), 0) << "once, not at every tick";
}

TEST_F(PresenceTest, TheThresholdIsInclusiveAtTheEdgeAndStrictBeyond) {
    const auto id = newPlayer();
    beat(id);
    now_ += 195;                                    // exactly the threshold since the heartbeat
    EXPECT_EQ(svc_->tick().went_offline, 0u) << "195 s of silence is still within 3 intervals + grace";
    now_ += 1;
    EXPECT_EQ(svc_->tick().went_offline, 1u);
}

TEST_F(PresenceTest, ABeatAfterBeingOfflineMakesItOnlineAgain) {
    const auto id = newPlayer();
    beat(id);
    now_ += 500;
    ASSERT_EQ(svc_->tick().went_offline, 1u);
    events();

    beat(id);
    EXPECT_EQ(count(events(), EventType::PlayerOnline), 1);
    EXPECT_TRUE(svc_->isOnline(id));
    EXPECT_EQ(dbEvents(id, "online"), 2);
}

TEST_F(PresenceTest, ARestartDoesNotTurnTheWholeFleetOffline) {
    const auto id = newPlayer();
    beat(id);
    flush();                                        // the database says: online
    now_ += 600;                                    // the server was down for ten minutes
    makeService();                                  // startup
    EXPECT_EQ(svc_->tick().went_offline, 0u) << "players get a full threshold to report in";
    now_ += 100;
    EXPECT_EQ(svc_->tick().went_offline, 0u);

    beat(id);                                       // it did report in
    now_ += 150;
    EXPECT_EQ(svc_->tick().went_offline, 0u);
    EXPECT_TRUE(svc_->isOnline(id));
}

TEST_F(PresenceTest, AFleetThatNeverReportsAfterARestartGoesOfflineAfterOneThreshold) {
    const auto id = newPlayer();
    beat(id);
    flush();
    now_ += 600;
    makeService();
    now_ += 196;
    EXPECT_EQ(svc_->tick().went_offline, 1u);
    EXPECT_FALSE(svc_->isOnline(id));
}

TEST_F(PresenceTest, HeartbeatsAreWrittenInBatchesNotOneByOne) {
    const auto a = newPlayer("A");
    const auto b = newPlayer("B");
    beat(a, "1.0");
    beat(b, "1.0");
    // The first sight of a player is a transition and is stored at once; the
    // steady stream after that is what gets batched.
    ASSERT_TRUE(db_->status(a).has_value());
    const auto first_seen = db_->status(a)->updated_at;

    now_ += 2;
    beat(a, "1.0");
    svc_->tick();
    EXPECT_EQ(db_->status(a)->updated_at, first_seen) << "the flush interval has not passed";

    now_ += 4;
    auto t = svc_->tick();
    EXPECT_GE(t.flushed, 1u);
    EXPECT_EQ(db_->status(a)->updated_at, now_.load() - 4) << "the latest beat is stored";
    EXPECT_EQ(db_->status(a)->version, "1.0");
    EXPECT_TRUE(db_->status(a)->online);
    EXPECT_EQ(db_->status(a)->payload_json, R"({"health":"ok"})");

    now_ += 10;
    EXPECT_EQ(svc_->tick().flushed, 0u) << "nothing new, nothing written";
}

TEST_F(PresenceTest, WriteThroughModeStoresEveryBeatAtOnce) {
    makeService(/*flush_interval_s=*/0);
    const auto id = newPlayer();
    beat(id);
    ASSERT_TRUE(db_->status(id).has_value());
    EXPECT_EQ(db_->status(id)->updated_at, now_.load());
    now_ += 7;
    beat(id);
    EXPECT_EQ(db_->status(id)->updated_at, now_.load());
}

TEST_F(PresenceTest, AVersionChangeIsNoticedOnceAndTheFirstReportIsNotAChange) {
    const auto id = newPlayer();
    beat(id, "0.3.5");
    beat(id, "0.3.5");
    EXPECT_EQ(count(events(), EventType::PlayerVersionChanged), 0);
    beat(id, "0.3.6");
    auto ev = events();
    ASSERT_EQ(count(ev, EventType::PlayerVersionChanged), 1);
    for (const auto& e : ev)
        if (e.type == EventType::PlayerVersionChanged) {
            EXPECT_EQ(e.payload["from"], "0.3.5");
            EXPECT_EQ(e.payload["to"], "0.3.6");
            EXPECT_EQ(e.payload["player_id"], id);
        }
    beat(id, "0.3.6");
    EXPECT_EQ(count(events(), EventType::PlayerVersionChanged), 0);
    EXPECT_EQ(dbEvents(id, "version_changed"), 1);
}

TEST_F(PresenceTest, TheVersionSurvivesARestartSoItIsNotReportedAsNewAgain) {
    const auto id = newPlayer();
    beat(id, "0.3.5");
    flush();
    makeService();
    beat(id, "0.3.5");
    EXPECT_EQ(count(events(), EventType::PlayerVersionChanged), 0);
    EXPECT_EQ(dbEvents(id, "version_changed"), 0);
    beat(id, "0.4.0");
    EXPECT_EQ(dbEvents(id, "version_changed"), 1);
}

TEST_F(PresenceTest, AStartupHeartbeatLeavesAStartedEventAndAnOrdinaryOneDoesNot) {
    const auto id = newPlayer();
    svc_->onHeartbeat(id, "1.0", "{}", /*startup=*/true);
    EXPECT_EQ(dbEvents(id, "started"), 1);
    EXPECT_EQ(dbEvents(id, "online"), 1) << "the first sight is still the player coming online";

    now_ += 60;
    beat(id);
    EXPECT_EQ(dbEvents(id, "started"), 1) << "only a heartbeat that says it is the first after a start";

    now_ += 60;
    svc_->onHeartbeat(id, "1.0", "{}", /*startup=*/true);   // restarted while still "online"
    EXPECT_EQ(dbEvents(id, "started"), 2) << "restarts inside the offline threshold are not lost";
    EXPECT_EQ(dbEvents(id, "online"), 1);
}

TEST_F(PresenceTest, EventsSayWhereThePlayerSitsSoTheyCanBeFilteredByRights) {
    auto obj = db_->createObject("Mall");
    auto grp = db_->createGroup(obj->id, "Hall");
    auto c = db_->createPlayer("Placed", std::nullopt, "strict", 86400, now_, grp->id);
    ASSERT_TRUE(c.has_value());
    const auto id = c->player.id;
    beat(id, "1.0");
    beat(id, "2.0");
    now_ += 1000;
    svc_->tick();
    int seen = 0;
    for (const auto& e : events()) {
        if (e.type != EventType::PlayerOnline && e.type != EventType::PlayerOffline &&
            e.type != EventType::PlayerVersionChanged) continue;
        ++seen;
        EXPECT_EQ(e.payload["group_id"], grp->id) << liveqx::events::eventTypeName(e.type);
        EXPECT_EQ(e.payload["object_id"], obj->id) << liveqx::events::eventTypeName(e.type);
    }
    EXPECT_EQ(seen, 3) << "online, version change, offline";
}

TEST_F(PresenceTest, ThePlaceInAnEventIsWhereThePlayerSitsNow) {
    auto obj = db_->createObject("Mall");
    auto g1 = db_->createGroup(obj->id, "G1");
    auto g2 = db_->createGroup(obj->id, "G2");
    auto c = db_->createPlayer("Mover", std::nullopt, "strict", 86400, now_, g1->id);
    const auto id = c->player.id;
    beat(id);
    events();
    ASSERT_EQ(db_->movePlayer(id, g2->id, now_), GroupResult::Ok);
    now_ += 1000;
    svc_->tick();
    bool offline = false;
    for (const auto& e : events())
        if (e.type == EventType::PlayerOffline) {
            offline = true;
            EXPECT_EQ(e.payload["group_id"], g2->id) << "a moved player's news goes to its new group";
        }
    EXPECT_TRUE(offline);
}

TEST_F(PresenceTest, ADeletedPlayerDisappearsWithoutEventsOrErrors) {
    const auto id = newPlayer();
    beat(id);
    flush();
    events();
    ASSERT_TRUE(db_->deletePlayer(id, "gone", now_));
    now_ += 1000;
    makeService();                                  // loads whatever the database still has
    now_ += 1000;
    auto t = svc_->tick();
    EXPECT_EQ(t.went_offline, 0u);
    EXPECT_EQ(count(events(), EventType::PlayerOffline), 0);
    EXPECT_FALSE(svc_->isOnline(id));
}

TEST_F(PresenceTest, ADeletionWhileRunningIsAlsoSilent) {
    const auto id = newPlayer();
    beat(id);
    events();
    ASSERT_TRUE(db_->deletePlayer(id, "gone", now_));
    now_ += 1000;
    EXPECT_EQ(svc_->tick().went_offline, 0u);
    EXPECT_EQ(count(events(), EventType::PlayerOffline), 0);
}

TEST_F(PresenceTest, AHeartbeatFromAnUnknownPlayerIsIgnored) {
    svc_->onHeartbeat("00000000-0000-4000-8000-000000000000", "1", "{}");
    EXPECT_EQ(count(events(), EventType::PlayerOnline), 0);
    EXPECT_FALSE(svc_->isOnline("00000000-0000-4000-8000-000000000000"));
}

TEST_F(PresenceTest, ABusyFleetAndTheSweeperCanRunTogether) {
    constexpr int kPlayers = 200;
    std::vector<std::string> ids;
    for (int i = 0; i < kPlayers; ++i) ids.push_back(newPlayer("P" + std::to_string(i)));

    std::atomic<bool> stop{false};
    std::thread sweeper([&] { while (!stop.load()) { svc_->tick(); std::this_thread::yield(); } });
    std::vector<std::thread> beaters;
    for (int t = 0; t < 4; ++t)
        beaters.emplace_back([&, t] {
            for (int i = t; i < kPlayers; i += 4) for (int k = 0; k < 5; ++k) beat(ids[i]);
        });
    for (auto& t : beaters) t.join();
    stop = true;
    sweeper.join();

    flush();
    int online = 0;
    for (const auto& id : ids) online += svc_->isOnline(id);
    EXPECT_EQ(online, kPlayers);
    for (const auto& id : ids) {
        ASSERT_TRUE(db_->status(id).has_value()) << id;
        EXPECT_TRUE(db_->status(id)->online);
    }
}

TEST_F(PresenceTest, TheBackgroundSweeperRunsAndStopsCleanly) {
    PresenceOptions o;
    o.offline_after_s  = 195;
    o.flush_interval_s = 0;
    o.tick_period_ms   = 20;
    o.now              = [this] { return now_.load(); };
    o.events           = &bus_;
    auto svc = std::make_unique<PresenceService>(*db_, std::move(o));
    const auto id = newPlayer();
    svc->onHeartbeat(id, "1", "{}");
    svc->start();
    now_ += 400;                                    // silence, seen by the thread
    for (int i = 0; i < 100 && svc->isOnline(id); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(svc->isOnline(id)) << "the thread noticed on its own";
    svc->stop();
    svc->stop();                                    // twice is fine
}
