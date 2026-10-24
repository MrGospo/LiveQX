// The certificates of the whole fleet seen together (CertSummary.h).
//
// What these tests protect:
//   * the counts and the soonest end cover bound screens only, and a screen that has run out
//     is counted as over but never as "the soonest to end";
//   * a screen is "renewing" only inside the renewal window; of those, the ones that are off
//     are told apart from the ones that are on and failing — they have different causes;
//   * the system-wide alarm needs both a minimum number of failing screens and half of those
//     that ought to be renewing, so one bad clock does not page anyone but a broken renewal
//     path does;
//   * the person's rights narrow every number, not only the list;
//   * the text a scrape gets and the JSON the page gets carry what they promise.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "players/CertSummary.h"
#include "players/PlayersDb.h"

namespace fs = std::filesystem;
using namespace liveqx::players;
using nlohmann::json;

namespace {

constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kT0  = 1'800'000'000;

class CertSummaryTest : public ::testing::Test {
protected:
    fs::path                   dir_;
    std::unique_ptr<PlayersDb> db_;
    std::int64_t               group_a_{0};
    std::int64_t               group_b_{0};
    int                        serial_{0};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-certsummary-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        auto o = db_->createObject("Main");
        group_a_ = db_->createGroup(o->id, "A")->id;
        group_b_ = db_->createGroup(o->id, "B")->id;
    }
    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A bound screen whose certificate ends `days` days after kT0; `online` and `renew_error`
    // are what its last report said (online < 0: it never reported).
    std::string screen(const std::string& name, double days, int online = 1,
                       const std::string& renew_error = "", std::int64_t group = 0) {
        auto p = db_->createPlayer(name, std::nullopt, "strict", 3600, kT0, group ? group : group_a_);
        EXPECT_TRUE(p.has_value());
        auto claim = db_->claimToken(p->player.id, p->token, kT0);
        EXPECT_TRUE(claim.ok);
        CertRow c;
        c.serial_hex = "S" + std::to_string(++serial_);
        c.generation = 1;
        c.player_id  = p->player.id;
        c.issued_at  = kT0 - kDay;
        c.not_before = kT0 - kDay;
        c.not_after  = kT0 + static_cast<std::int64_t>(days * kDay);
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, c, kT0));
        if (online >= 0) {
            StatusRow st;
            st.player_id = p->player.id;
            st.updated_at = kT0;
            st.online = online > 0;
            json payload = json::object();
            if (!renew_error.empty()) payload["last_renew_error"] = renew_error;
            st.payload_json = payload.dump();
            EXPECT_TRUE(db_->applyStatuses({st}));
        }
        return p->player.id;
    }
};

}  // namespace

TEST(CertRenewAlarm, NeedsEnoughFailingScreensAndHalfOfThoseThatOughtToRenew) {
    EXPECT_FALSE(renewAlarm(0, 0));
    EXPECT_FALSE(renewAlarm(2, 2)) << "two screens are two screens, not the fleet";
    EXPECT_TRUE(renewAlarm(3, 3));
    EXPECT_TRUE(renewAlarm(3, 6)) << "exactly half";
    EXPECT_FALSE(renewAlarm(3, 7)) << "under half";
    EXPECT_TRUE(renewAlarm(100, 150));
    EXPECT_FALSE(renewAlarm(3, 1000)) << "three bad clocks among a thousand";
}

TEST_F(CertSummaryTest, CountsCoverBoundScreensOnlyAndTheSoonestEndSkipsThoseThatAreOver) {
    screen("fine", 300);
    screen("soon", 20);
    const auto soonest = screen("sooner", 5);
    screen("over", -10);
    ASSERT_TRUE(db_->createPlayer("waiting", std::nullopt, "strict", 3600, kT0, group_a_).has_value());
    const auto gone = screen("gone", 100);
    ASSERT_TRUE(db_->revokePlayer(gone, "test", kT0).has_value());

    const auto s = summarizeScreens(*db_, kT0);
    EXPECT_EQ(s.bound, 4) << "the pending and the revoked one are not bound";
    EXPECT_EQ(s.count(CertStage::Ok), 1);
    EXPECT_EQ(s.count(CertStage::Soon30), 1);
    EXPECT_EQ(s.count(CertStage::Soon14), 1);
    EXPECT_EQ(s.count(CertStage::Expired), 1);
    ASSERT_TRUE(s.earliest.has_value());
    EXPECT_EQ(s.earliest->player_id, soonest) << "the one that is over is not 'the soonest to end'";
    EXPECT_EQ(s.earliest->not_after, kT0 + 5 * kDay);
}

TEST_F(CertSummaryTest, NoBoundScreenMeansNoEarliestAndNoAlarm) {
    const auto s = summarizeScreens(*db_, kT0);
    EXPECT_EQ(s.bound, 0);
    EXPECT_FALSE(s.earliest.has_value());
    EXPECT_FALSE(s.systemic);
    const auto j = screenSummaryJson(s, kT0);
    EXPECT_TRUE(j["earliest"].is_null());
    EXPECT_EQ(j["counts"]["expired"], 0);
}

TEST_F(CertSummaryTest, OnlyScreensInsideTheRenewalWindowAreRenewingAndOffAndFailingAreToldApart) {
    screen("far", 70, 1, "left over from long ago");          // outside the window: its error is not counted
    screen("ok-50", 50, 1);                                    // on, renewing, fine
    const auto bad = screen("bad-40", 40, 1, "clock is wrong");
    screen("off-20", 20, 0, "last said before it went off");   // off: cannot renew, a different cause
    screen("silent-10", 10, -1);                               // never reported
    screen("over", -3, 1, "too late");                         // over: nothing to renew with

    const auto s = summarizeScreens(*db_, kT0);
    EXPECT_EQ(s.renewing, 4);
    EXPECT_EQ(s.renewing_online, 2);
    EXPECT_EQ(s.renewing_offline, 2);
    EXPECT_EQ(s.failing, 1);
    ASSERT_EQ(s.failures.size(), 1u);
    EXPECT_EQ(s.failures[0].player_id, bad);
    EXPECT_EQ(s.failures[0].error, "clock is wrong");
    EXPECT_EQ(s.failures[0].stage, CertStage::Ok);             // 40 days: not yet in a "soon" step
    EXPECT_FALSE(s.systemic);
}

TEST_F(CertSummaryTest, FailuresComeSoonestEndFirstAndTheAlarmFollowsTheRule) {
    screen("a", 50, 1, "e1");
    screen("b", 10, 1, "e2");
    screen("c", 30, 1, "e3");
    screen("d", 40, 1);
    auto s = summarizeScreens(*db_, kT0);
    ASSERT_EQ(s.failures.size(), 3u);
    EXPECT_EQ(s.failures[0].error, "e2");
    EXPECT_EQ(s.failures[1].error, "e3");
    EXPECT_EQ(s.failures[2].error, "e1");
    EXPECT_EQ(s.failing, 3);
    EXPECT_EQ(s.renewing_online, 4);
    EXPECT_TRUE(s.systemic) << "3 of 4 that ought to be renewing";

    for (int i = 0; i < 5; ++i) screen("fine-" + std::to_string(i), 45, 1);
    s = summarizeScreens(*db_, kT0);
    EXPECT_EQ(s.failing, 3);
    EXPECT_EQ(s.renewing_online, 9);
    EXPECT_FALSE(s.systemic) << "3 of 9 is under half";
}

TEST_F(CertSummaryTest, TheRightsNarrowEveryNumber) {
    screen("a-1", 10, 1, "err-a");
    screen("a-2", 200);
    screen("b-1", -5, 1, "", group_b_);
    screen("b-2", 2, 1, "err-b", group_b_);
    const auto only_b = [this](const PlayerRow& p) { return p.group_id && *p.group_id == group_b_; };

    const auto all = summarizeScreens(*db_, kT0);
    const auto b   = summarizeScreens(*db_, kT0, only_b);
    EXPECT_EQ(all.bound, 4);
    EXPECT_EQ(b.bound, 2);
    EXPECT_EQ(b.count(CertStage::Expired), 1);
    EXPECT_EQ(b.count(CertStage::Soon3), 1);
    EXPECT_EQ(b.failing, 1);
    ASSERT_EQ(b.failures.size(), 1u);
    EXPECT_EQ(b.failures[0].error, "err-b") << "the other group's error is not shown";
    ASSERT_TRUE(b.earliest.has_value());
    EXPECT_EQ(b.earliest->name, "b-2");
}

TEST_F(CertSummaryTest, TheJsonGivesDaysLeftAndCutsTheListOfFailures) {
    for (int i = 0; i < 5; ++i) screen("f-" + std::to_string(i), 10 + i, 1, "err");
    const auto s = summarizeScreens(*db_, kT0);
    const auto j = screenSummaryJson(s, kT0, /*max_failures=*/2);
    EXPECT_EQ(j["bound"], 5);
    EXPECT_EQ(j["renew"]["failing"], 5) << "the count is the whole, the list is cut";
    EXPECT_EQ(j["renew"]["failures"].size(), 2u);
    EXPECT_EQ(j["renew"]["failures"][0]["days_left"], 10);
    EXPECT_EQ(j["renew"]["window_days"], kRenewWindowDays);
    EXPECT_EQ(j["earliest"]["days_left"], 10);
    EXPECT_EQ(j["renew"]["systemic"], true);
}

TEST(CertServerCerts, ADateGivesStageAndDaysAndAMissingOneGivesOnlyTheReason) {
    const std::vector<ServerCertEntry> certs = {
        {"api_tls", kT0 + 200 * kDay, ""},
        {"device_ca", kT0 + 20 * kDay, ""},
        {"broker_tls", 0, "missing"},
        {"crl", kT0 - kDay, ""},
    };
    const auto j = serverCertsJson(certs, kT0);
    ASSERT_EQ(j.size(), 4u);
    EXPECT_EQ(j[0]["stage"], "ok");
    EXPECT_EQ(j[0]["days_left"], 200);
    EXPECT_EQ(j[1]["stage"], "soon30");
    EXPECT_FALSE(j[2].contains("stage"));
    EXPECT_FALSE(j[2].contains("not_after"));
    EXPECT_EQ(j[2]["error"], "missing");
    EXPECT_EQ(j[3]["stage"], "expired");
    EXPECT_EQ(j[3]["days_left"], -1);
}

TEST_F(CertSummaryTest, TheScrapeCarriesTheStepsTheSoonestEndTheRenewalsAndTheServerDates) {
    screen("fine", 300);
    screen("soon", 10, 1, "err");
    screen("over", -2);
    const auto s = summarizeScreens(*db_, kT0);
    const std::string text = certMetricsText(s, {{"api_tls", kT0 + 100 * kDay, ""}, {"broker_tls", 0, "missing"}}, kT0);

    EXPECT_NE(text.find("liveqx_players_cert_stage{stage=\"ok\"} 1\n"), std::string::npos) << text;
    EXPECT_NE(text.find("liveqx_players_cert_stage{stage=\"soon14\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_cert_stage{stage=\"expired\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_cert_stage{stage=\"soon3\"} 0\n"), std::string::npos) << "every step is always there";
    EXPECT_NE(text.find("liveqx_players_cert_min_days_remaining 10\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_renewing 1\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_renew_failing 1\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_renew_alarm 0\n"), std::string::npos);
    EXPECT_NE(text.find("liveqx_server_cert_days_remaining{name=\"api_tls\"} 100\n"), std::string::npos);
    EXPECT_EQ(text.find("name=\"broker_tls\""), std::string::npos) << "no date, no sample";
    EXPECT_EQ(text.find("TYPE liveqx_players_cert_stage gauge"), text.rfind("TYPE liveqx_players_cert_stage gauge"))
        << "each family is declared once";
}

TEST(CertMetricsText, NoBoundScreensGivesNoMinimumAndNoServerFamilyWithoutDates) {
    ScreenCertSummary empty;
    const std::string text = certMetricsText(empty, {}, kT0);
    EXPECT_EQ(text.find("liveqx_players_cert_min_days_remaining"), std::string::npos)
        << "a minimum over nothing would be a lie";
    EXPECT_EQ(text.find("liveqx_server_cert_days_remaining"), std::string::npos);
    EXPECT_NE(text.find("liveqx_players_cert_stage{stage=\"ok\"} 0\n"), std::string::npos);
}
