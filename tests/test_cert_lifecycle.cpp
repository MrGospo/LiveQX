// How close the screens' certificates are to their end, and the watch that announces it.
//
// What these tests protect:
//   * the steps (30 / 14 / 3 days, over) change exactly at their edges, and a screen with
//     no valid certificate at all counts as over;
//   * the watch speaks once per change: a second look, or a look after the server has been
//     restarted, repeats nothing; a screen that renews (or is bound again) is announced
//     as fine once;
//   * only bound screens are watched — a pending or revoked one has no certificate to count;
//   * the line the watch remembers its last word by is not lost to the trimming of the
//     journal, or a screen that stays off for months would be announced again and again.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "events/EventBus.h"
#include "players/CertLifecycle.h"
#include "players/CertWatch.h"
#include "players/PlayersDb.h"

namespace fs = std::filesystem;
using namespace liveqx::players;
using liveqx::events::EventType;
using nlohmann::json;

namespace {

constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kT0  = 1'800'000'000;

}  // namespace

TEST(CertLifecycle, TheStepsChangeExactlyAtTheirEdges) {
    const std::int64_t now = kT0;
    EXPECT_EQ(certStageOf(now + 30 * kDay + 1, now), CertStage::Ok);
    EXPECT_EQ(certStageOf(now + 30 * kDay,     now), CertStage::Soon30);
    EXPECT_EQ(certStageOf(now + 14 * kDay + 1, now), CertStage::Soon30);
    EXPECT_EQ(certStageOf(now + 14 * kDay,     now), CertStage::Soon14);
    EXPECT_EQ(certStageOf(now + 3 * kDay + 1,  now), CertStage::Soon14);
    EXPECT_EQ(certStageOf(now + 3 * kDay,      now), CertStage::Soon3);
    EXPECT_EQ(certStageOf(now + 1,             now), CertStage::Soon3);
    EXPECT_EQ(certStageOf(now,                 now), CertStage::Expired) << "the very second it ends it is over";
    EXPECT_EQ(certStageOf(now - 5 * kDay,      now), CertStage::Expired);
}

TEST(CertLifecycle, AScreenWithNoValidCertificateIsOver) {
    EXPECT_EQ(certStageOf(std::nullopt, kT0), CertStage::Expired);
}

TEST(CertLifecycle, DaysLeftAreWholeDaysRoundedDownAndNegativeAfterTheEnd) {
    EXPECT_EQ(certDaysLeft(kT0 + 10 * kDay, kT0), 10);
    EXPECT_EQ(certDaysLeft(kT0 + 10 * kDay - 1, kT0), 9);
    EXPECT_EQ(certDaysLeft(kT0, kT0), 0);
    EXPECT_EQ(certDaysLeft(kT0 - 1, kT0), -1) << "one second past the end is not 'today'";
    EXPECT_EQ(certDaysLeft(kT0 - kDay, kT0), -1);
    EXPECT_EQ(certDaysLeft(kT0 - kDay - 1, kT0), -2);
}

TEST(CertLifecycle, NamesRoundTripAndGroupsAreRight) {
    for (auto s : {CertStage::Ok, CertStage::Soon30, CertStage::Soon14, CertStage::Soon3, CertStage::Expired}) {
        auto back = parseCertStage(certStageName(s));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, s);
    }
    EXPECT_FALSE(parseCertStage("soon").has_value()) << "'soon' is a filter, not a step";
    EXPECT_FALSE(parseCertStage("").has_value());
    EXPECT_TRUE(certIsSoon(CertStage::Soon30) && certIsSoon(CertStage::Soon14) && certIsSoon(CertStage::Soon3));
    EXPECT_FALSE(certIsSoon(CertStage::Ok));
    EXPECT_FALSE(certIsSoon(CertStage::Expired));
    EXPECT_FALSE(certNeedsAttention(CertStage::Ok));
    EXPECT_TRUE(certNeedsAttention(CertStage::Soon3));
    EXPECT_TRUE(certNeedsAttention(CertStage::Expired));
}

namespace {

class CertWatchTest : public ::testing::Test {
protected:
    fs::path                    dir_;
    std::unique_ptr<PlayersDb>  db_;
    liveqx::events::EventBus    bus_;
    std::shared_ptr<liveqx::events::Subscription> sub_;
    std::int64_t                clock_{kT0};
    std::int64_t                group_id_{0};
    std::int64_t                object_id_{0};
    int                         serial_{0};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-certwatch-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        auto o = db_->createObject("Main");
        auto g = db_->createGroup(o->id, "Hall");
        object_id_ = o->id;
        group_id_  = g->id;
        sub_ = bus_.subscribe();
    }
    void TearDown() override {
        sub_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<CertWatch> watch() {
        CertWatchOptions o;
        o.events = &bus_;
        o.now    = [this] { return clock_; };
        return std::make_unique<CertWatch>(*db_, std::move(o));
    }

    CertRow cert(const std::string& player, std::int64_t not_after) {
        CertRow c;
        c.serial_hex = "C" + std::to_string(++serial_);
        c.generation = 1;
        c.player_id  = player;
        c.issued_at  = kT0 - kDay;
        c.not_before = kT0 - kDay;
        c.not_after  = not_after;
        return c;
    }

    // A screen that has been bound, with a certificate that ends `days` days after kT0.
    std::string boundScreen(const std::string& name, std::int64_t days) {
        auto p = db_->createPlayer(name, std::nullopt, "strict", 3600, kT0, group_id_);
        EXPECT_TRUE(p.has_value());
        auto claim = db_->claimToken(p->player.id, p->token, kT0);
        EXPECT_TRUE(claim.ok);
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, cert(p->player.id, kT0 + days * kDay), kT0));
        return p->player.id;
    }

    std::vector<std::string> stageLines(const std::string& id) {
        std::vector<std::string> out;
        auto rows = db_->events(id, 100);
        std::sort(rows.begin(), rows.end(), [](const EventRow& a, const EventRow& b) { return a.id < b.id; });
        for (const auto& r : rows)
            if (r.kind == "cert_stage") {
                const auto j = json::parse(r.details);
                out.push_back(j["previous"].get<std::string>() + ">" + j["stage"].get<std::string>());
            }
        return out;
    }

    std::vector<liveqx::events::Event> news() {
        std::vector<liveqx::events::Event> out;
        for (auto& e : sub_->drain(std::chrono::milliseconds(0)))
            if (e.type == EventType::PlayerCertStage) out.push_back(std::move(e));
        return out;
    }
};

}  // namespace

TEST_F(CertWatchTest, ScreenWithPlentyOfTimeIsNotAnnounced) {
    const auto id = boundScreen("Lobby", 300);
    auto w = watch();
    EXPECT_EQ(w->scan(), 0u);
    EXPECT_TRUE(stageLines(id).empty());
    EXPECT_TRUE(news().empty());
}

TEST_F(CertWatchTest, EachStepIsAnnouncedOnceInTheJournalAndOnTheEventStream) {
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();

    clock_ = kT0 + 75 * kDay;                                      // 25 days left
    EXPECT_EQ(w->scan(), 1u);
    EXPECT_EQ(w->scan(), 0u) << "a second look says nothing new";
    clock_ = kT0 + 80 * kDay;                                      // 20 days: same step
    EXPECT_EQ(w->scan(), 0u);

    clock_ = kT0 + 90 * kDay;                                      // 10 days
    EXPECT_EQ(w->scan(), 1u);
    clock_ = kT0 + 98 * kDay;                                      // 2 days
    EXPECT_EQ(w->scan(), 1u);
    clock_ = kT0 + 101 * kDay;                                     // over
    EXPECT_EQ(w->scan(), 1u);

    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"ok>soon30", "soon30>soon14", "soon14>soon3", "soon3>expired"}));

    auto ev = news();
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[0].payload["player_id"], id);
    EXPECT_EQ(ev[0].payload["name"], "Lobby");
    EXPECT_EQ(ev[0].payload["stage"], "soon30");
    EXPECT_EQ(ev[0].payload["previous"], "ok");
    EXPECT_EQ(ev[0].payload["not_after"], kT0 + 100 * kDay);
    EXPECT_EQ(ev[0].payload["days_left"], 25);
    EXPECT_EQ(ev[0].payload["group_id"], group_id_) << "so a user who sees only this place gets the news";
    EXPECT_EQ(ev[0].payload["object_id"], object_id_);
    EXPECT_EQ(ev[3].payload["stage"], "expired");
    EXPECT_LT(ev[3].payload["days_left"].get<std::int64_t>(), 0);
}

TEST_F(CertWatchTest, ARestartOfTheServerDoesNotAnnounceTheSameThingAgain) {
    const auto id = boundScreen("Lobby", 100);
    clock_ = kT0 + 90 * kDay;
    {
        auto w = watch();
        EXPECT_EQ(w->scan(), 1u);
    }
    (void)news();
    auto again = watch();                                           // a fresh watch: nothing in memory
    EXPECT_EQ(again->scan(), 0u);
    EXPECT_TRUE(news().empty());
    EXPECT_EQ(stageLines(id).size(), 1u);
}

TEST_F(CertWatchTest, ASkippedStepIsAnnouncedAsWhereTheScreenNowIs) {
    // The server was down for weeks: the screen went from "fine" straight to "3 days".
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();
    clock_ = kT0 + 98 * kDay;
    EXPECT_EQ(w->scan(), 1u);
    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"ok>soon3"}));
}

TEST_F(CertWatchTest, ARenewalIsAnnouncedAsFineOnce) {
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();
    clock_ = kT0 + 90 * kDay;
    ASSERT_EQ(w->scan(), 1u);

    ASSERT_TRUE(db_->recordRenewal(cert(id, clock_ + 365 * kDay), clock_));
    EXPECT_EQ(w->scan(), 1u);
    EXPECT_EQ(w->scan(), 0u);
    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"ok>soon14", "soon14>ok"}));
}

TEST_F(CertWatchTest, ABoundAgainScreenIsFineAgainAndOnlyScreensThatAreBoundAreWatched) {
    const auto id = boundScreen("Lobby", 100);
    auto pending = db_->createPlayer("New", std::nullopt, "strict", 3600, kT0, group_id_);
    ASSERT_TRUE(pending.has_value());
    const auto gone = boundScreen("Gone", 100);
    ASSERT_TRUE(db_->revokePlayer(gone, "test", kT0).has_value());

    auto w = watch();
    clock_ = kT0 + 120 * kDay;                                      // everything bound has run out
    EXPECT_EQ(w->scan(), 1u) << "the one bound screen; not the pending one, not the revoked one";
    EXPECT_TRUE(stageLines(pending->player.id).empty());
    EXPECT_TRUE(stageLines(gone).empty());
    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"ok>expired"}));

    // Bound again with a fresh token: a new certificate, a new start.
    auto re = db_->reEnrollPlayer(id, "rebind", 3600, clock_);
    ASSERT_TRUE(re.has_value());
    EXPECT_EQ(w->scan(), 0u) << "while it waits for its token it is not watched";
    auto claim = db_->claimToken(id, re->token.token, clock_);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(db_->finishEnrollment(claim.token_id, cert(id, clock_ + 365 * kDay), clock_));
    EXPECT_EQ(w->scan(), 1u);
    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"ok>expired", "expired>ok"}));
}

TEST_F(CertWatchTest, AScreenWhoseCertificateRecordIsGoneStaysOverAndIsNotAnnouncedAgain) {
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();
    clock_ = kT0 + 101 * kDay;
    ASSERT_EQ(w->scan(), 1u);
    // A month after the end the record is removed (see applyRetention); the screen is as over as before.
    ASSERT_GE(db_->purgeExpiredCerts(clock_ + 40 * kDay), 1u);
    EXPECT_EQ(w->scan(), 0u);
    const auto ends = db_->liveCertEnds();
    EXPECT_EQ(ends.count(id), 0u);
}

TEST_F(CertWatchTest, TrimmingTheJournalKeepsTheLastStepOfEachScreenButNothingElseOld) {
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();
    clock_ = kT0 + 90 * kDay;
    ASSERT_EQ(w->scan(), 1u);                                       // soon14
    clock_ = kT0 + 101 * kDay;
    ASSERT_EQ(w->scan(), 1u);                                       // expired
    ASSERT_TRUE(db_->addEvent(id, "note", "{}", kT0 + 1));

    // Everything is older than the cutoff; only the screen's latest word remains.
    EXPECT_GE(db_->purgeEventsOlderThan(clock_ + 1000 * kDay), 1u);
    EXPECT_EQ(stageLines(id), (std::vector<std::string>{"soon14>expired"}));
    auto rows = db_->events(id, 100);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, "cert_stage");

    auto again = watch();
    EXPECT_EQ(again->scan(), 0u) << "a trimmed journal does not make the screen 'newly over'";
}

TEST_F(CertWatchTest, TheLastStepOfADeletedScreenIsTrimmedLikeAnyOtherLine) {
    const auto id = boundScreen("Lobby", 100);
    auto w = watch();
    clock_ = kT0 + 101 * kDay;
    ASSERT_EQ(w->scan(), 1u);
    ASSERT_TRUE(db_->deletePlayer(id, "gone", clock_));
    db_->purgeEventsOlderThan(clock_ + 1000 * kDay);
    EXPECT_TRUE(db_->events(id, 100).empty());
}

TEST_F(CertWatchTest, TheThreadLooksOnceSoonAfterStartAndStopsPromptly) {
    boundScreen("Lobby", 100);
    clock_ = kT0 + 101 * kDay;
    CertWatchOptions o;
    o.events = &bus_;
    o.now    = [this] { return clock_; };
    o.first_look_after_s = 0;
    o.interval_s         = 3600;
    CertWatch w(*db_, std::move(o));
    w.start();
    bool announced = false;
    for (int i = 0; i < 100 && !announced; ++i) {
        if (!sub_->drain(std::chrono::milliseconds(50)).empty()) announced = true;
    }
    EXPECT_TRUE(announced) << "the first look did not happen";
    const auto t0 = std::chrono::steady_clock::now();
    w.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2)) << "stop must not wait out the hour";
}
