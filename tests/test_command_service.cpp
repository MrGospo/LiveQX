// players/CommandService — commands to a single screen (A12/A13, B13).
//
// What these tests protect:
//   * the command that goes out is exactly the agreed message (id, issued_at,
//     ttl_seconds, type), not retained, with a lifetime in the broker, and only
//     to the screen it is for;
//   * a screen waiting for confirmation of a command cannot be sent the same one
//     again — and the check is the database's, so two requests cannot both pass;
//     a different command type is not blocked;
//   * a command the broker could not take never blocks the screen;
//   * the block is lifted by the player's report (executed / refused), by the
//     timeout of its type, or by an administrator — and by nothing else: not by
//     another screen's report, not by a repeated one;
//   * a reboot is believed only if the machine's own uptime shows it;
//   * only paired, live screens get commands; unknown types are refused;
//   * every settlement is announced with the screen's place, for the tree filter.

#include "events/EventBus.h"
#include "players/CommandService.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace liveqx::players;
using liveqx::events::EventType;
using nlohmann::json;

namespace {

constexpr std::int64_t kT0 = 1'800'000'000;

class CommandServiceTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<PlayersDb> db_;
    std::unique_ptr<PlayersService> players_;
    std::unique_ptr<CommandService> svc_;
    liveqx::events::EventBus bus_;
    std::shared_ptr<liveqx::events::Subscription> sub_;
    std::mutex sent_mu_;
    std::vector<liveqx::mqtt::Message> sent_;
    liveqx::mqtt::PublishResult broker_{liveqx::mqtt::PublishResult::Ok};
    std::int64_t group_id_{0};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-command-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        PlayersServiceOptions po;
        players_ = std::make_unique<PlayersService>(*db_, std::move(po));
        auto o = db_->createObject("Main");
        auto g = db_->createGroup(o->id, "Hall");
        group_id_ = g->id;
        sub_ = bus_.subscribe();
        CommandServiceOptions co;
        co.events  = &bus_;
        co.publish = [this](const liveqx::mqtt::Message& m) {
            std::lock_guard<std::mutex> lk(sent_mu_);
            if (broker_ == liveqx::mqtt::PublishResult::Ok) sent_.push_back(m);
            return broker_;
        };
        svc_ = std::make_unique<CommandService>(*db_, *players_, std::move(co));
    }
    void TearDown() override {
        svc_.reset();
        players_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A paired (active) screen in the group.
    std::string screen(const std::string& name = "Lobby") {
        static std::atomic<int> n{0};
        auto c = db_->createPlayer(name, std::nullopt, "strict", 86400, kT0, group_id_);
        EXPECT_TRUE(c.has_value());
        auto claim = db_->claimToken(c->player.id, c->token, kT0 + 1);
        EXPECT_TRUE(claim.ok);
        CertRow cert;
        cert.player_id = c->player.id; cert.serial_hex = "AB" + std::to_string(++n); cert.generation = 1;
        cert.issued_at = kT0; cert.not_before = kT0 - 100; cert.not_after = kT0 + 365 * 86400;
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, cert, kT0 + 2));
        return c->player.id;
    }
    std::vector<liveqx::events::Event> events() { return sub_->drain(std::chrono::milliseconds(20)); }
};

}  // namespace

TEST_F(CommandServiceTest, TheCommandThatGoesOutIsTheAgreedMessageToTheRightScreenOnly) {
    const auto a = screen("A");
    const auto b = screen("B");
    auto out = svc_->sendToPlayer(a, "restart_player", 7, kT0 + 100);
    ASSERT_TRUE(out.ok) << out.error;

    ASSERT_EQ(sent_.size(), 1u);
    const auto& m = sent_[0];
    EXPECT_EQ(m.topic, "liveqx/v1/players/" + a + "/commands");
    EXPECT_EQ(m.qos, 1);
    EXPECT_FALSE(m.retain) << "a retained command would repeat at every reconnect";
    ASSERT_TRUE(m.expiry_s.has_value());
    EXPECT_EQ(*m.expiry_s, 300u) << "the broker drops it if the screen stays away";
    const json j = json::parse(m.payload);
    EXPECT_EQ(j["id"], out.row.id);
    EXPECT_EQ(j["type"], "restart_player");
    EXPECT_EQ(j["ttl_seconds"], 300);
    EXPECT_EQ(j["issued_at"], "2027-01-15T08:01:40Z");
    EXPECT_EQ(j.size(), 4u) << "all four fields and nothing else";
    EXPECT_TRUE(db_->pendingCommandsOf(b).empty());
}

TEST_F(CommandServiceTest, AScreenWaitingForAConfirmationCannotBeSentTheSameCommandAgain) {
    const auto a = screen();
    auto first = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 100);
    ASSERT_TRUE(first.ok);
    auto again = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 101);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.error, "command_in_progress");
    EXPECT_EQ(again.busy_id, first.row.id) << "the caller is told which command holds the block";
    EXPECT_EQ(sent_.size(), 1u) << "nothing went out the second time";

    auto other = svc_->sendToPlayer(a, "reload_playlist", 1, kT0 + 102);
    EXPECT_TRUE(other.ok) << "a different command is not blocked by this one";
}

TEST_F(CommandServiceTest, ACommandTheBrokerCouldNotTakeNeverBlocksTheScreen) {
    const auto a = screen();
    broker_ = liveqx::mqtt::PublishResult::NotConnected;
    auto lost = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    EXPECT_FALSE(lost.ok);
    EXPECT_EQ(lost.error, "broker_unavailable");
    EXPECT_EQ(lost.row.state, "not_sent");
    EXPECT_TRUE(db_->pendingCommandsOf(a).empty());

    broker_ = liveqx::mqtt::PublishResult::Ok;
    EXPECT_TRUE(svc_->sendToPlayer(a, "restart_player", 1, kT0 + 101).ok) << "the next attempt is free to go";
}

TEST_F(CommandServiceTest, NoBrokerAtAllIsTheSameAnswer) {
    CommandServiceOptions co;                      // publish left empty
    CommandService bare(*db_, *players_, std::move(co));
    const auto a = screen();
    auto out = bare.sendToPlayer(a, "restart_player", 1, kT0 + 100);
    EXPECT_EQ(out.error, "broker_unavailable");
    EXPECT_TRUE(db_->pendingCommandsOf(a).empty());
}

TEST_F(CommandServiceTest, ThePlayersReportLiftsTheBlock) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    ASSERT_TRUE(out.ok);
    svc_->onResult(a, out.row.id, "executed", std::nullopt, kT0 + 130);
    auto row = db_->findCommand(out.row.id);
    EXPECT_EQ(row->state, "executed");
    EXPECT_EQ(*row->finished_at, kT0 + 130);
    EXPECT_TRUE(svc_->sendToPlayer(a, "restart_player", 1, kT0 + 140).ok);
}

TEST_F(CommandServiceTest, ARefusalByThePlayerIsRecordedAsSuchAndAlsoLiftsTheBlock) {
    const auto a = screen();
    for (const char* why : {"failed", "expired", "unsupported"}) {
        auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
        ASSERT_TRUE(out.ok) << why;
        svc_->onResult(a, out.row.id, why, std::nullopt, kT0 + 110);
        auto row = db_->findCommand(out.row.id);
        EXPECT_EQ(row->state, "failed") << why;
        EXPECT_EQ(row->result, why);
    }
}

TEST_F(CommandServiceTest, ADuplicateReportMeansAnEarlierDeliveryDidTheWork) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "reload_playlist", 1, kT0 + 100);
    svc_->onResult(a, out.row.id, "duplicate", std::nullopt, kT0 + 110);
    auto row = db_->findCommand(out.row.id);
    EXPECT_EQ(row->state, "executed");
    EXPECT_EQ(row->result, "duplicate");
}

TEST_F(CommandServiceTest, OnlyTheScreenTheCommandWentToCanSettleIt) {
    const auto a = screen("A");
    const auto b = screen("B");
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    svc_->onResult(b, out.row.id, "executed", std::nullopt, kT0 + 110);
    EXPECT_EQ(db_->findCommand(out.row.id)->state, "pending") << "another screen's word counts for nothing";
    svc_->onResult(a, "00000000-0000-4000-8000-000000000000", "executed", std::nullopt, kT0 + 110);
    EXPECT_EQ(db_->findCommand(out.row.id)->state, "pending") << "nor does an unknown id";
}

TEST_F(CommandServiceTest, ARepeatedReportChangesNothing) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    svc_->onResult(a, out.row.id, "executed", std::nullopt, kT0 + 110);
    svc_->onResult(a, out.row.id, "failed", std::nullopt, kT0 + 200);    // a later, contradictory echo
    auto row = db_->findCommand(out.row.id);
    EXPECT_EQ(row->state, "executed");
    EXPECT_EQ(*row->finished_at, kT0 + 110);
}

TEST_F(CommandServiceTest, ARebootIsBelievedOnlyIfTheMachineReallyRestarted) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 1000);
    ASSERT_TRUE(out.ok);
    // Reported at +300 s; the machine has been up for 4 hours: it did not reboot.
    svc_->onResult(a, out.row.id, "executed", 4 * 3600, kT0 + 1300);
    auto row = db_->findCommand(out.row.id);
    EXPECT_EQ(row->state, "failed");
    EXPECT_EQ(row->result, "os_not_rebooted");

    auto second = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 2000);
    ASSERT_TRUE(second.ok);
    svc_->onResult(a, second.row.id, "executed", 120, kT0 + 2300);       // up for 2 minutes: it did
    EXPECT_EQ(db_->findCommand(second.row.id)->state, "executed");

    auto third = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 3000);
    svc_->onResult(a, third.row.id, "executed", std::nullopt, kT0 + 3300);   // an older player says nothing
    EXPECT_EQ(db_->findCommand(third.row.id)->state, "executed");
}

TEST_F(CommandServiceTest, ARestartIsNotJudgedByTheMachinesUptime) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 1000);
    svc_->onResult(a, out.row.id, "executed", 4 * 3600, kT0 + 1100);     // only the program restarts
    EXPECT_EQ(db_->findCommand(out.row.id)->state, "executed");
}

TEST_F(CommandServiceTest, SilenceEndsInATimeoutThatDependsOnTheTypeAndLiftsTheBlock) {
    const auto a = screen();
    auto restart = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    auto reboot  = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 100);
    ASSERT_TRUE(restart.ok && reboot.ok);

    EXPECT_EQ(svc_->sweep(kT0 + 100 + 299), 0u) << "five minutes not yet";
    EXPECT_EQ(svc_->sweep(kT0 + 100 + 300), 1u) << "a restart gets five minutes";
    EXPECT_EQ(db_->findCommand(restart.row.id)->state, "timeout");
    EXPECT_EQ(db_->findCommand(restart.row.id)->result, "no_confirmation");
    EXPECT_EQ(db_->findCommand(reboot.row.id)->state, "pending") << "a reboot is given twenty";
    EXPECT_EQ(svc_->sweep(kT0 + 100 + 1200), 1u);
    EXPECT_TRUE(svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 2000).ok) << "the block is gone";
}

TEST_F(CommandServiceTest, TheTimeoutsAndTheLifetimeAreSettings) {
    ASSERT_EQ(players_->updateSettings({{"command_ttl_s", "120"}, {"confirm_timeout_restart_s", "60"}}), "");
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.row.ttl_seconds, 120);
    EXPECT_EQ(*sent_.back().expiry_s, 120u);
    EXPECT_EQ(out.row.deadline_at, kT0 + 160);

    EXPECT_EQ(players_->updateSettings({{"command_ttl_s", "5"}}), "invalid_command_ttl_s");
    EXPECT_EQ(players_->updateSettings({{"confirm_timeout_reboot_s", "999999"}}), "invalid_confirm_timeout_reboot_s");
    EXPECT_EQ(players_->updateSettings({{"command_ttl_s", ""}}), "") << "empty resets to the default";
    EXPECT_EQ(players_->settings().command_ttl_s, 300);
}

TEST_F(CommandServiceTest, AnAdministratorCanLiftTheBlockByHandOnce) {
    const auto a = screen();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    auto released = svc_->release(out.row.id, kT0 + 150);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ(released->state, "released");
    EXPECT_FALSE(svc_->release(out.row.id, kT0 + 160).has_value()) << "already settled";
    EXPECT_FALSE(svc_->release("nope", kT0 + 160).has_value());
    EXPECT_TRUE(svc_->sendToPlayer(a, "restart_player", 1, kT0 + 170).ok);
}

TEST_F(CommandServiceTest, OnlyPairedLiveScreensGetCommandsAndOnlyKnownTypes) {
    const auto a = screen();
    EXPECT_EQ(svc_->sendToPlayer(a, "format_disk", 1, kT0 + 100).error, "unsupported_type");
    EXPECT_EQ(svc_->sendToPlayer(a, "update_now", 1, kT0 + 100).error, "unsupported_type") << "a later phase";
    EXPECT_EQ(svc_->sendToPlayer("no-such-player", "restart_player", 1, kT0 + 100).error, "player_not_found");

    auto pending = db_->createPlayer("New", std::nullopt, "strict", 86400, kT0, group_id_);
    EXPECT_EQ(svc_->sendToPlayer(pending->player.id, "restart_player", 1, kT0 + 100).error, "player_not_active");

    db_->revokePlayer(a, "test", kT0 + 50);
    EXPECT_EQ(svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100).error, "player_not_active");
    EXPECT_TRUE(sent_.empty());
}

TEST_F(CommandServiceTest, EverySettlementIsAnnouncedWithTheScreensPlace) {
    const auto a = screen();
    events();
    auto out = svc_->sendToPlayer(a, "restart_player", 1, kT0 + 100);
    svc_->onResult(a, out.row.id, "executed", std::nullopt, kT0 + 110);
    auto ev = events();
    int seen = 0;
    for (const auto& e : ev) {
        if (e.type != EventType::PlayerCommand) continue;
        ++seen;
        EXPECT_EQ(e.payload["player_id"], a);
        EXPECT_EQ(e.payload["group_id"], group_id_) << "the filter needs the place";
        EXPECT_TRUE(e.payload.contains("object_id"));
    }
    EXPECT_EQ(seen, 2) << "sent, then executed";
    bool logged = false;
    for (const auto& e : db_->events(a, 20)) logged = logged || e.kind == "command_executed";
    EXPECT_TRUE(logged);
}

TEST_F(CommandServiceTest, TwoRequestsAtOnceCannotBothGetThrough) {
    const auto a = screen();
    std::atomic<int> ok{0}, busy{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&] {
            auto out = svc_->sendToPlayer(a, "reboot_screen", 1, kT0 + 100);
            if (out.ok) ++ok; else if (out.error == "command_in_progress") ++busy;
        });
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), 1);
    EXPECT_EQ(busy.load(), 7);
}
