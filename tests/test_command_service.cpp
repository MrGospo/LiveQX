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
    // The screen is connected (or not), as presence would have recorded it.
    void setOnline(const std::string& id, bool online = true) {
        StatusRow st;
        st.player_id = id; st.updated_at = kT0; st.online = online; st.version = "0.3.5";
        db_->applyStatuses({st});
    }
    // A second object with one group and one connected screen.
    std::string otherObjectScreen() {
        auto o = db_->createObject("Annex");
        auto g = db_->createGroup(o->id, "Lobby");
        auto c = db_->createPlayer("annex", std::nullopt, "strict", 86400, kT0, g->id);
        auto claim = db_->claimToken(c->player.id, c->token, kT0 + 1);
        CertRow cert;
        cert.player_id = c->player.id; cert.serial_hex = "ANX1"; cert.generation = 1;
        cert.issued_at = kT0; cert.not_before = kT0 - 100; cert.not_after = kT0 + 365 * 86400;
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, cert, kT0 + 2));
        setOnline(c->player.id);
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

// ── group, object, everyone ─────────────────────────────────────────────────
namespace {
CommandTarget groupOf(std::int64_t id) { return {TargetKind::Group, id}; }
CommandTarget allScreens() { return {TargetKind::All, 0}; }
}  // namespace

TEST_F(CommandServiceTest, ThePhraseNamesTheCommandAndTheTargetAndDiffersByKind) {
    auto o = db_->findObject(db_->findGroup(group_id_)->object_id);
    const auto group = CommandService::requiredPhrase("reboot_screen", TargetKind::Group, "Main / Hall");
    const auto object = CommandService::requiredPhrase("reboot_screen", TargetKind::Object, "Main");
    const auto all = CommandService::requiredPhrase("reboot_screen", TargetKind::All, "");
    EXPECT_EQ(group, "reboot_screen group Main / Hall");
    EXPECT_EQ(object, "reboot_screen object Main");
    EXPECT_EQ(all, "reboot_screen all");
    EXPECT_NE(group, object);
    EXPECT_NE(object, all);
    EXPECT_NE(CommandService::requiredPhrase("restart_player", TargetKind::All, ""), all) << "the command is in it";
    EXPECT_EQ(o->name, "Main");
}

TEST_F(CommandServiceTest, ThePlanSeparatesConnectedFromAbsentAndShowsWhoIsBusy) {
    const auto a = screen("A"), b = screen("B"), c = screen("C");
    setOnline(a); setOnline(b);                      // C is not connected
    ASSERT_TRUE(svc_->sendToPlayer(b, "reboot_screen", 1, kT0 + 5).ok);

    auto plan = svc_->plan(groupOf(group_id_), "reboot_screen");
    ASSERT_TRUE(plan.found);
    EXPECT_EQ(plan.target_name, "Main / Hall");
    EXPECT_EQ(plan.required_phrase, "reboot_screen group Main / Hall");
    EXPECT_EQ(plan.total, 3u) << "all paired screens count, connected or not";
    EXPECT_EQ(plan.online.size(), 2u);
    ASSERT_EQ(plan.offline.size(), 1u);
    EXPECT_EQ(plan.offline[0], c);
    ASSERT_EQ(plan.busy.size(), 1u);
    EXPECT_EQ(plan.busy[0].player_id, b);
    EXPECT_FALSE(svc_->plan(groupOf(9999), "reboot_screen").found);
}

TEST_F(CommandServiceTest, AGroupCommandReachesTheConnectedScreensAndReportsTheRest) {
    const auto a = screen("A"), b = screen("B"), c = screen("C");
    setOnline(a); setOnline(b);
    auto out = svc_->sendToTarget(groupOf(group_id_), "restart_player", "restart_player group Main / Hall", 3, 7, kT0 + 10);
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_EQ(out.sent.size(), 2u);
    EXPECT_TRUE(out.failed.empty());
    ASSERT_EQ(out.plan.offline.size(), 1u);
    EXPECT_EQ(out.plan.offline[0], c);
    EXPECT_EQ(sent_.size(), 2u);
    EXPECT_TRUE(db_->pendingCommandsOf(c).empty()) << "an absent screen gets nothing";
    EXPECT_EQ(db_->pendingCommandsOf(a)[0].issued_by, 7);
}

TEST_F(CommandServiceTest, ThePhraseIsForgivingAboutCaseAndSpacingButNotAboutWords) {
    const auto a = screen(); setOnline(a);
    EXPECT_TRUE(svc_->sendToTarget(groupOf(group_id_), "reload_playlist", "  RELOAD_PLAYLIST   group   main / HALL ", 1, 1, kT0).ok);
    const auto b = screen("B"); setOnline(b);
    for (const char* bad : {"", "reload_playlist group Hall", "reload_playlist object Main / Hall",
                            "reboot_screen group Main / Hall", "yes"}) {
        auto out = svc_->sendToTarget(groupOf(group_id_), "restart_player", bad, 2, 1, kT0 + 1);
        EXPECT_EQ(out.error, "confirm_phrase_mismatch") << bad;
    }
    EXPECT_EQ(sent_.size(), 1u) << "nothing went out on a wrong phrase";
}

TEST_F(CommandServiceTest, AChangedTargetStopsTheCommand) {
    const auto a = screen(); setOnline(a);
    auto out = svc_->sendToTarget(groupOf(group_id_), "restart_player", "restart_player group Main / Hall", 5, 1, kT0);
    EXPECT_EQ(out.error, "expected_count_mismatch");
    EXPECT_EQ(out.plan.total, 1u) << "the caller is told what is there now";
    EXPECT_TRUE(sent_.empty());
}

TEST_F(CommandServiceTest, AnyScreenStillWaitingForTheSameCommandStopsTheWholeThing) {
    const auto a = screen("A"), b = screen("B");
    setOnline(a); setOnline(b);
    auto first = svc_->sendToPlayer(b, "reboot_screen", 1, kT0);
    ASSERT_TRUE(first.ok);
    sent_.clear();
    auto out = svc_->sendToTarget(groupOf(group_id_), "reboot_screen", "reboot_screen group Main / Hall", 2, 1, kT0 + 5);
    EXPECT_EQ(out.error, "screens_busy");
    ASSERT_EQ(out.plan.busy.size(), 1u);
    EXPECT_EQ(out.plan.busy[0].player_id, b);
    EXPECT_EQ(out.plan.busy[0].command_id, first.row.id);
    EXPECT_TRUE(sent_.empty()) << "not even the free screen got it";
    EXPECT_TRUE(db_->pendingCommandsOf(a).empty());
}

TEST_F(CommandServiceTest, ObjectAndEveryoneCoverMoreAndLeaveOutScreensThatAreNotLive) {
    const auto a = screen("A"); setOnline(a);
    const auto annex = otherObjectScreen();
    const auto g2 = db_->createGroup(db_->findGroup(group_id_)->object_id, "Second");
    auto c = db_->createPlayer("B", std::nullopt, "strict", 86400, kT0, g2->id);   // never paired: pending
    (void)c;

    auto obj = svc_->plan({TargetKind::Object, db_->findGroup(group_id_)->object_id}, "restart_player");
    EXPECT_EQ(obj.total, 1u) << "the unpaired screen is not a recipient; the other object is not in it";
    auto all = svc_->plan(allScreens(), "restart_player");
    EXPECT_EQ(all.total, 2u);
    EXPECT_EQ(all.required_phrase, "restart_player all");
    auto out = svc_->sendToTarget(allScreens(), "restart_player", "restart_player all", 2, 1, kT0);
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_EQ(out.sent.size(), 2u);
    EXPECT_FALSE(db_->pendingCommandsOf(annex).empty());
}

TEST_F(CommandServiceTest, RevokedScreensAreNeverRecipients) {
    const auto a = screen("A"), b = screen("B");
    setOnline(a); setOnline(b);
    db_->revokePlayer(b, "gone", kT0 + 1);
    auto plan = svc_->plan(groupOf(group_id_), "restart_player");
    EXPECT_EQ(plan.total, 1u);
}

TEST_F(CommandServiceTest, NothingToSendToAndNoSuchTargetAreSaidPlainly) {
    EXPECT_EQ(svc_->sendToTarget(groupOf(group_id_), "restart_player", "restart_player group Main / Hall", 0, 1, kT0).error,
              "no_targets");
    EXPECT_EQ(svc_->sendToTarget(groupOf(4242), "restart_player", "x", 0, 1, kT0).error, "target_not_found");
    EXPECT_EQ(svc_->sendToTarget(allScreens(), "format_disk", "x", 0, 1, kT0).error, "unsupported_type");
}

TEST_F(CommandServiceTest, ABrokerThatIsAwayStopsABulkCommandWithoutBlockingAnyone) {
    const auto a = screen("A"), b = screen("B");
    setOnline(a); setOnline(b);
    broker_ = liveqx::mqtt::PublishResult::NotConnected;
    auto out = svc_->sendToTarget(groupOf(group_id_), "restart_player", "restart_player group Main / Hall", 2, 1, kT0);
    EXPECT_EQ(out.error, "broker_unavailable");
    EXPECT_TRUE(db_->pendingCommandsOf(a).empty());
    EXPECT_TRUE(db_->pendingCommandsOf(b).empty());
    broker_ = liveqx::mqtt::PublishResult::Ok;
    EXPECT_TRUE(svc_->sendToTarget(groupOf(group_id_), "restart_player", "restart_player group Main / Hall", 2, 1, kT0 + 1).ok);
}

// ── batches ─────────────────────────────────────────────────────────────────
namespace {
struct JobLog {
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> sent;      // player, job
    std::vector<std::pair<std::string, JobSummary>> ended;      // state, summary
    std::vector<std::string> ended_reason;
};
}  // namespace

class BatchTest : public CommandServiceTest {
protected:
    JobLog log_;
    int ok_left_{-1};                      // publishes the broker still takes; -1 = no limit
    std::unique_ptr<CommandService> batcher_;

    void makeBatcher() {
        CommandServiceOptions co;
        co.events = &bus_;
        co.publish = [this](const liveqx::mqtt::Message& m) {
            std::lock_guard<std::mutex> lk(sent_mu_);
            if (broker_ == liveqx::mqtt::PublishResult::Ok) {
                if (ok_left_ == 0) return liveqx::mqtt::PublishResult::NotConnected;   // the broker drops out here
                if (ok_left_ > 0) --ok_left_;
                sent_.push_back(m);
            }
            return broker_;
        };
        co.on_bulk_sent = [this](const CommandRow& c, const JobRow& j) {
            std::lock_guard<std::mutex> lk(log_.mu);
            log_.sent.push_back({c.player_id, j.id});
        };
        co.on_job_end = [this](const JobRow& j, const JobSummary& s) {
            std::lock_guard<std::mutex> lk(log_.mu);
            log_.ended.push_back({j.state, s});
            log_.ended_reason.push_back(j.stop_reason);
        };
        batcher_ = std::make_unique<CommandService>(*db_, *players_, std::move(co));
    }
    void SetUp() override {
        CommandServiceTest::SetUp();
        makeBatcher();
    }
    std::vector<std::string> connected(int n) {
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i) {
            ids.push_back(screen("S" + std::to_string(i)));
            setOnline(ids.back());
        }
        return ids;
    }
    BulkOutcome startGroup(std::size_t count, const char* type = "restart_player") {
        return batcher_->sendToTarget(CommandTarget{TargetKind::Group, group_id_}, type,
                                      std::string(type) + " group Main / Hall", count, 3, kT0, "boss", "admin");
    }
    void confirm(const std::string& player, const std::string& cmd, const char* status = "executed") {
        batcher_->onResult(player, cmd, status, std::nullopt, kT0 + 5);
    }
    std::size_t pendingTotal(const std::vector<std::string>& ids) {
        std::size_t n = 0;
        for (const auto& id : ids) n += db_->pendingCommandsOf(id).size();
        return n;
    }
};

TEST_F(BatchTest, OnlyTheFirstBatchGoesOutAndTheNextWaitsForTheWholeBatchToBeSettled) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    auto ids = connected(5);
    auto out = startGroup(5);
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_EQ(out.sent.size(), 2u);
    EXPECT_EQ(out.queued, 3u);
    EXPECT_EQ(out.batch_size, 2);
    EXPECT_EQ(sent_.size(), 2u);

    batcher_->tick(kT0 + 10);
    EXPECT_EQ(sent_.size(), 2u) << "nothing yet: the first two have not answered";

    confirm(out.sent[0].player_id, out.sent[0].command_id);
    batcher_->tick(kT0 + 11);
    EXPECT_EQ(sent_.size(), 2u) << "half a batch is not a batch";

    confirm(out.sent[1].player_id, out.sent[1].command_id);
    batcher_->tick(kT0 + 12);
    EXPECT_EQ(sent_.size(), 4u) << "now the second batch";

    auto v = batcher_->jobView(out.job_id);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->job.state, "running");
    EXPECT_EQ(v->summary.queued, 1u);
    EXPECT_EQ(v->summary.pending, 2u);
    EXPECT_EQ(v->summary.executed, 2u);
}

TEST_F(BatchTest, TheJobEndsDoneAfterTheLastBatchIsSettledAndTheAuditSeesEveryCommand) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(3);
    auto out = startGroup(3);
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);
    batcher_->tick(kT0 + 10);
    auto jv = batcher_->jobView(out.job_id);
    int guard = 0;
    while (jv->job.state == "running" && guard++ < 5) {
        for (const auto& it : jv->items)
            if (it.item.state == "sent" && it.command_state == "pending") confirm(it.item.player_id, it.item.command_id);
        batcher_->tick(kT0 + 20 + guard);
        jv = batcher_->jobView(out.job_id);
    }
    EXPECT_EQ(jv->job.state, "done");
    EXPECT_EQ(jv->summary.executed, 3u);
    std::lock_guard<std::mutex> lk(log_.mu);
    EXPECT_EQ(log_.sent.size(), 3u) << "one audit callback per command, whatever the batch";
    for (const auto& e : log_.sent) EXPECT_EQ(e.second, out.job_id);
    ASSERT_EQ(log_.ended.size(), 1u);
    EXPECT_EQ(log_.ended[0].first, "done");
    EXPECT_EQ(log_.ended[0].second.executed, 3u);
}

TEST_F(BatchTest, MoreThanThirtyPercentUnconfirmedStopsTheJobAndCancelsTheRest) {
    auto ids = connected(20);                               // batches of 10
    auto out = startGroup(20);
    ASSERT_EQ(out.sent.size(), 10u);
    for (std::size_t i = 0; i < 6; ++i) confirm(out.sent[i].player_id, out.sent[i].command_id);
    for (std::size_t i = 6; i < 10; ++i) confirm(out.sent[i].player_id, out.sent[i].command_id, "failed");   // 4 of 10

    batcher_->tick(kT0 + 10);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "stopped");
    EXPECT_EQ(v->job.stop_reason, "too_many_unconfirmed");
    EXPECT_EQ(v->summary.cancelled, 10u) << "the second batch never went out";
    EXPECT_EQ(sent_.size(), 10u);
    std::lock_guard<std::mutex> lk(log_.mu);
    ASSERT_EQ(log_.ended.size(), 1u);
    EXPECT_EQ(log_.ended[0].first, "stopped");
}

TEST_F(BatchTest, ExactlyThirtyPercentIsStillAcceptable) {
    connected(20);
    auto out = startGroup(20);
    for (std::size_t i = 0; i < 7; ++i) confirm(out.sent[i].player_id, out.sent[i].command_id);
    for (std::size_t i = 7; i < 10; ++i) confirm(out.sent[i].player_id, out.sent[i].command_id, "failed");   // 3 of 10
    batcher_->tick(kT0 + 10);
    EXPECT_EQ(batcher_->jobView(out.job_id)->job.state, "running");
    EXPECT_EQ(sent_.size(), 20u) << "the second batch went";
}

TEST_F(BatchTest, SilenceCountsAsNotComingBack) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "4"}}), "");
    connected(8);
    auto out = startGroup(8);
    ASSERT_EQ(out.sent.size(), 4u);
    batcher_->tick(kT0 + 100);
    EXPECT_EQ(sent_.size(), 4u) << "still waiting inside the time allowed";
    batcher_->sweep(kT0 + 400);                              // five minutes pass: all four time out
    batcher_->tick(kT0 + 401);
    EXPECT_EQ(batcher_->jobView(out.job_id)->job.state, "stopped");
    EXPECT_EQ(sent_.size(), 4u);
}

TEST_F(BatchTest, AScreenThatDisappearedBeforeItsTurnIsSkippedNotCounted) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    auto ids = connected(4);
    auto out = startGroup(4);
    setOnline(out.sent[0].player_id, true);
    // the two of the second batch: one drops off the network
    auto before = batcher_->jobView(out.job_id);
    std::vector<std::string> second;
    for (const auto& it : before->items) if (it.item.state == "queued") second.push_back(it.item.player_id);
    ASSERT_EQ(second.size(), 2u);
    setOnline(second[0], false);

    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);
    batcher_->tick(kT0 + 10);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->summary.skipped_offline, 1u);
    EXPECT_EQ(v->summary.pending, 1u) << "the other one went";
    confirm(second[1], db_->pendingCommandsOf(second[1])[0].id);
    batcher_->tick(kT0 + 20);
    EXPECT_EQ(batcher_->jobView(out.job_id)->job.state, "done") << "a skipped screen is not a failure";
}

// Every change of a job is announced, so a page showing jobs never has to ask again.
namespace {
std::vector<nlohmann::json> jobEvents(const std::vector<liveqx::events::Event>& ev) {
    std::vector<nlohmann::json> out;
    for (const auto& e : ev)
        if (e.type == EventType::PlayerCommandJob) out.push_back(e.payload);
    return out;
}
}  // namespace

TEST_F(BatchTest, AJobIsAnnouncedWhenItStartsWhenABatchGoesOutAndWhenItIsDone) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "1"}}), "");
    connected(2);
    events();
    auto out = startGroup(2);
    ASSERT_TRUE(out.ok) << out.error;
    auto started = jobEvents(events());
    ASSERT_FALSE(started.empty());
    for (const auto& p : started) {
        EXPECT_EQ(p["job_id"], out.job_id);
        EXPECT_EQ(p["state"], "running");
        EXPECT_EQ(p["created_by"], 3) << "the filter shows the news to whoever started the job";
    }

    confirm(out.sent[0].player_id, out.sent[0].command_id);
    batcher_->tick(kT0 + 10);                                 // the second batch goes out
    EXPECT_FALSE(jobEvents(events()).empty()) << "a batch going out is news, though no state changed";

    auto second = db_->pendingCommandsOf(batcher_->jobView(out.job_id)->items[1].item.player_id);
    ASSERT_FALSE(second.empty());
    confirm(second[0].player_id, second[0].id);
    events();
    batcher_->tick(kT0 + 20);                                 // the last screen has answered: the job closes
    auto closing = jobEvents(events());
    ASSERT_FALSE(closing.empty()) << "the closing comes after the last command's own news, so it needs its own";
    EXPECT_EQ(closing.back()["state"], "done");
}

TEST_F(BatchTest, ACancelledOrStoppedJobIsAnnouncedToo) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(5);
    auto out = startGroup(5);
    events();
    ASSERT_TRUE(batcher_->cancelJob(out.job_id, kT0 + 5).has_value());
    auto ev = jobEvents(events());
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.back()["state"], "cancelled");
}

TEST_F(BatchTest, ABatchWhollyAbsentWhenItsTurnComesDoesNotStallTheJob) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "1"}}), "");
    auto ids = connected(3);
    auto out = startGroup(3);
    auto before = batcher_->jobView(out.job_id);
    std::vector<std::string> later;
    for (const auto& it : before->items) if (it.item.state == "queued") later.push_back(it.item.player_id);
    setOnline(later[0], false);                             // the whole second batch is gone
    confirm(out.sent[0].player_id, out.sent[0].command_id);
    batcher_->tick(kT0 + 10);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->summary.skipped_offline, 1u);
    EXPECT_EQ(v->summary.pending, 1u) << "the job went straight on to the third";
}

TEST_F(BatchTest, ACancelledJobSendsNothingMoreButWhatWentOutStays) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(5);
    auto out = startGroup(5);
    auto cancelled = batcher_->cancelJob(out.job_id, kT0 + 5);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->state, "cancelled");
    EXPECT_EQ(cancelled->stop_reason, "cancelled_by_user");
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);
    batcher_->tick(kT0 + 10);
    EXPECT_EQ(sent_.size(), 2u);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->summary.cancelled, 3u);
    EXPECT_EQ(v->summary.executed, 2u);
    EXPECT_FALSE(batcher_->cancelJob(out.job_id, kT0 + 20).has_value()) << "once";
    EXPECT_FALSE(batcher_->cancelJob("nope", kT0 + 20).has_value());
    std::lock_guard<std::mutex> lk(log_.mu);
    ASSERT_EQ(log_.ended.size(), 1u);
    EXPECT_EQ(log_.ended[0].first, "cancelled");
}

TEST_F(BatchTest, AJobInProgressContinuesAfterTheServerRestarts) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(4);
    auto out = startGroup(4);
    ASSERT_EQ(out.sent.size(), 2u);
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);

    // The server restarts: a fresh service over the same database.
    makeBatcher();
    batcher_->tick(kT0 + 30);
    EXPECT_EQ(sent_.size(), 4u) << "the second batch went out after the restart";
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "running");
    EXPECT_EQ(v->summary.pending, 2u);
}

TEST_F(BatchTest, ABatchThatMeetsAnAbsentBrokerWaitsForItInsteadOfFailingAndGoesOutWhenItIsBack) {
    // Found under load: after a restart the first look at a running job comes a moment before the
    // server has reconnected to the broker. Failing the batch for that stopped a job that had done nothing wrong.
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(4);
    auto out = startGroup(4);
    ASSERT_EQ(out.sent.size(), 2u);
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);
    makeBatcher();                                                  // the server restarts...
    broker_ = liveqx::mqtt::PublishResult::NotConnected;            // ...and the broker is not there yet

    batcher_->tick(kT0 + 30);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "running") << "the job waits";
    EXPECT_EQ(v->summary.failed, 0u);
    EXPECT_EQ(v->summary.queued, 2u) << "the second batch is untouched";
    EXPECT_EQ(sent_.size(), 2u);
    EXPECT_TRUE(log_.ended.empty());

    batcher_->tick(kT0 + 60);                                       // still away, but within the allowance
    EXPECT_EQ(batcher_->jobView(out.job_id)->job.state, "running");

    broker_ = liveqx::mqtt::PublishResult::Ok;                      // the link is up
    batcher_->tick(kT0 + 62);
    EXPECT_EQ(sent_.size(), 4u) << "the second batch went out as soon as it could";
    v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "running");
    EXPECT_EQ(v->summary.pending, 2u);
    EXPECT_EQ(v->summary.failed, 0u);
}

TEST_F(BatchTest, ABrokerThatStaysAwayEndsTheJobSayingSoAndNotThatTheScreensDidNotAnswer) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "2"}}), "");
    connected(4);
    auto out = startGroup(4);
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);
    broker_ = liveqx::mqtt::PublishResult::NotConnected;

    batcher_->tick(kT0 + 30);                                       // the wait begins
    EXPECT_EQ(batcher_->jobView(out.job_id)->job.state, "running");
    batcher_->tick(kT0 + 30 + 301);                                 // longer than the allowance (300 s)
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "stopped");
    EXPECT_EQ(v->job.stop_reason, "broker_unavailable");
    ASSERT_EQ(log_.ended.size(), 1u);
    EXPECT_EQ(log_.ended_reason[0], "broker_unavailable");
    EXPECT_EQ(sent_.size(), 2u) << "nothing more went out";
}

TEST_F(BatchTest, ABrokerThatDropsOutInTheMiddleOfABatchLetsTheRestOfTheBatchFollowWhenItIsBack) {
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "4"}}), "");
    connected(8);
    auto out = startGroup(8);
    ASSERT_EQ(out.sent.size(), 4u);
    for (const auto& s : out.sent) confirm(s.player_id, s.command_id);

    ok_left_ = 1;                                                   // the second batch starts, and the broker goes
    batcher_->tick(kT0 + 30);
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.state, "running");
    EXPECT_EQ(v->summary.failed, 0u) << "what could not go yet is waiting, not failed";
    EXPECT_EQ(v->summary.pending, 1u);
    EXPECT_EQ(v->summary.queued, 3u);

    ok_left_ = -1;                                                  // the broker is back
    batcher_->tick(kT0 + 32);
    EXPECT_EQ(batcher_->jobView(out.job_id)->summary.queued, 3u) << "the one that went out is still to be settled first";
    const auto now_out = batcher_->jobView(out.job_id);
    for (const auto& it : now_out->items)
        if (it.item.state == "sent" && it.command_state == "pending") confirm(it.item.player_id, it.item.command_id);
    batcher_->tick(kT0 + 35);
    v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->summary.queued, 0u);
    EXPECT_EQ(v->summary.pending, 3u) << "the rest of the batch followed";
    EXPECT_EQ(v->summary.failed, 0u);
}

TEST_F(BatchTest, TheOverviewOfAJobGivesTheSameCountsAsItsFullViewWithoutReadingItsItems) {
    // The list of jobs needs only the counts; summing them in the database is what keeps that list quick.
    ASSERT_EQ(players_->updateSettings({{"command_batch_size", "4"}}), "");
    connected(9);
    auto offline = screen("Away");                                    // not connected: skipped at the start
    (void)offline;
    auto out = startGroup(10);                                        // nine connected and the one away
    ASSERT_EQ(out.sent.size(), 4u);

    auto same = [&](const char* when) {
        const auto full = batcher_->jobView(out.job_id);
        const auto over = batcher_->jobOverview(out.job_id);
        ASSERT_TRUE(full && over) << when;
        EXPECT_EQ(over->summary.total, full->summary.total) << when;
        EXPECT_EQ(over->summary.queued, full->summary.queued) << when;
        EXPECT_EQ(over->summary.pending, full->summary.pending) << when;
        EXPECT_EQ(over->summary.executed, full->summary.executed) << when;
        EXPECT_EQ(over->summary.unconfirmed, full->summary.unconfirmed) << when;
        EXPECT_EQ(over->summary.failed, full->summary.failed) << when;
        EXPECT_EQ(over->summary.skipped_offline, full->summary.skipped_offline) << when;
        EXPECT_EQ(over->summary.cancelled, full->summary.cancelled) << when;
        EXPECT_EQ(over->job.id, full->job.id) << when;
        EXPECT_TRUE(over->items.empty()) << "no items read for a list";
    };
    same("batch out, nothing answered");
    confirm(out.sent[0].player_id, out.sent[0].command_id);
    confirm(out.sent[1].player_id, out.sent[1].command_id, "failed");           // answered, but refused: unconfirmed
    same("two answered, two pending");
    batcher_->cancelJob(out.job_id, kT0 + 10);
    same("cancelled with a batch still pending");
    EXPECT_FALSE(batcher_->jobOverview("no-such-job"));
}

TEST_F(BatchTest, TheBatchSizeAndTheThresholdAreSettingsWithLimits) {
    EXPECT_EQ(players_->updateSettings({{"command_batch_size", "0"}}), "invalid_command_batch_size");
    EXPECT_EQ(players_->updateSettings({{"command_batch_size", "101"}}), "invalid_command_batch_size");
    EXPECT_EQ(players_->updateSettings({{"command_batch_stop_percent", "0"}}), "invalid_command_batch_stop_percent");
    EXPECT_EQ(players_->updateSettings({{"command_batch_stop_percent", "50"}, {"command_batch_size", "20"}}), "");
    EXPECT_EQ(players_->settings().command_batch_size, 20);
    EXPECT_EQ(players_->settings().command_batch_stop_percent, 50);
    EXPECT_EQ(players_->updateSettings({{"command_batch_size", ""}}), "");
    EXPECT_EQ(players_->settings().command_batch_size, 10);
}

TEST_F(BatchTest, TheJobRecordsWhoStartedItAndWhatWasAbsentAtTheStart) {
    auto ids = connected(2);
    const auto absent = screen("gone");                     // paired but not connected
    auto out = startGroup(3);
    ASSERT_TRUE(out.ok) << out.error;
    auto v = batcher_->jobView(out.job_id);
    EXPECT_EQ(v->job.created_by, 3);
    EXPECT_EQ(v->job.created_by_name, "boss");
    EXPECT_EQ(v->job.created_by_role, "admin");
    EXPECT_EQ(v->job.target_kind, "group");
    EXPECT_EQ(v->job.target_name, "Main / Hall");
    EXPECT_EQ(v->summary.skipped_offline, 1u);
    EXPECT_EQ(v->items.front().item.player_id, absent) << "absent screens are listed first";
}

// ── retention of the players' journals ──────────────────────────────────────
class RetentionTest : public CommandServiceTest {
protected:
    static constexpr std::int64_t kDay = 86400;

    // A command of `type` sent at kT0 and settled (or left pending).
    std::string command(const std::string& player, const char* type, bool settle, std::int64_t finished_at = 0) {
        CommandRow c;
        c.id = PlayersDb::newId(); c.player_id = player; c.type = type;
        c.issued_at = kT0; c.ttl_seconds = 300; c.deadline_at = kT0 + 300;
        EXPECT_EQ(db_->createCommand(c), CommandCreate::Ok);
        if (settle) EXPECT_TRUE(db_->settleCommand(c.id, "executed", "executed", finished_at).has_value());
        return c.id;
    }
    std::string job(const char* state, std::int64_t created, std::int64_t finished) {
        JobRow j;
        j.id = PlayersDb::newId(); j.type = "restart_player"; j.target_kind = "all"; j.target_name = "all";
        j.batch_size = 10; j.stop_percent = 30; j.created_at = created;
        EXPECT_TRUE(db_->createJob(j, {{"", "p1", 0, "queued", "", ""}}));
        if (std::string(state) != "running") EXPECT_TRUE(db_->finishJob(j.id, state, "", finished));
        return j.id;
    }
};

TEST_F(RetentionTest, ByDefaultEventsAreKeptNinetyDaysAndCommandsAYear) {
    EXPECT_EQ(players_->settings().events_retention_days, 90);
    EXPECT_EQ(players_->settings().commands_retention_days, 365);
}

TEST_F(RetentionTest, OldEventsGoAndRecentOnesStay) {
    const auto a = screen();                                  // its "enrolled" event is from kT0: long past by `now`
    const std::int64_t now = kT0 + 400 * kDay;
    db_->addEvent(a, "old-online", "", now - 91 * kDay);
    db_->addEvent(a, "edge-offline", "", now - 89 * kDay);
    db_->addEvent(a, "recent-online", "", now - kDay);

    auto r = players_->applyRetention(now);
    EXPECT_EQ(r.events, 2u) << "the enrollment event and the one of 91 days";
    std::set<std::string> left;
    for (const auto& e : db_->events(a, 100)) left.insert(e.kind);
    EXPECT_EQ(left, (std::set<std::string>{"edge-offline", "recent-online"}));
}

TEST_F(RetentionTest, PairingTokensOfEnrolledScreensAreRemovedAfterAMonthAndReportedSeparately) {
    const auto a = screen();                                  // enrolled: its token was used at kT0
    const std::int64_t now = kT0 + 100 * kDay;
    const auto r = players_->applyRetention(now);
    EXPECT_EQ(r.tokens, 1u);
    EXPECT_EQ(players_->applyRetention(now).tokens, 0u);
    EXPECT_TRUE(db_->findPlayer(a).has_value()) << "the screen itself is untouched";
}

TEST_F(RetentionTest, SettledCommandsOlderThanTheLimitGoAndUnsettledOnesNeverDoWhateverTheirAge) {
    const auto a = screen();
    const std::int64_t now = kT0 + 1000 * kDay;
    const auto old_done = command(a, "restart_player", true, now - 366 * kDay);
    const auto new_done = command(a, "reboot_screen", true, now - 364 * kDay);
    const auto forgotten = command(a, "reload_playlist", false);            // pending since long ago

    auto r = players_->applyRetention(now);
    EXPECT_EQ(r.commands, 1u);
    EXPECT_FALSE(db_->findCommand(old_done).has_value());
    EXPECT_TRUE(db_->findCommand(new_done).has_value());
    ASSERT_TRUE(db_->findCommand(forgotten).has_value()) << "an unsettled command still blocks its screen: not for retention to remove";
    EXPECT_EQ(db_->findCommand(forgotten)->state, "pending");
}

TEST_F(RetentionTest, FinishedJobsGoWithTheirScreensAndRunningOnesStay) {
    const std::int64_t now = kT0 + 1000 * kDay;
    const auto old_job = job("done", now - 400 * kDay, now - 399 * kDay);
    const auto recent_job = job("stopped", now - 10 * kDay, now - 9 * kDay);
    const auto running = job("running", now - 500 * kDay, 0);

    auto r = players_->applyRetention(now);
    EXPECT_EQ(r.jobs, 1u);
    EXPECT_FALSE(db_->findJob(old_job).has_value());
    EXPECT_TRUE(db_->jobItems(old_job).empty()) << "its screens went with it";
    EXPECT_TRUE(db_->findJob(recent_job).has_value());
    EXPECT_TRUE(db_->findJob(running).has_value()) << "a job in progress is never removed";
    EXPECT_EQ(db_->jobItems(running).size(), 1u);
}

TEST_F(RetentionTest, TheLimitsAreSettingsWithBounds) {
    ASSERT_EQ(players_->updateSettings({{"events_retention_days", "30"}, {"commands_retention_days", "180"}}), "");
    EXPECT_EQ(players_->settings().events_retention_days, 30);
    EXPECT_EQ(players_->settings().commands_retention_days, 180);
    EXPECT_EQ(players_->updateSettings({{"events_retention_days", "6"}}), "invalid_events_retention_days");
    EXPECT_EQ(players_->updateSettings({{"commands_retention_days", "4000"}}), "invalid_commands_retention_days");
    EXPECT_EQ(players_->updateSettings({{"events_retention_days", ""}}), "");
    EXPECT_EQ(players_->settings().events_retention_days, 90) << "empty = the default";

    const auto a = screen();
    const std::int64_t now = kT0 + 400 * kDay;
    ASSERT_EQ(players_->updateSettings({{"events_retention_days", "30"}}), "");
    db_->addEvent(a, "online", "", now - 31 * kDay);
    EXPECT_EQ(players_->applyRetention(now).events >= 1u, true) << "the shorter limit is the one used";
}

TEST_F(RetentionTest, RunningItTwiceFindsNothingTheSecondTime) {
    const auto a = screen();
    const std::int64_t now = kT0 + 1000 * kDay;
    command(a, "restart_player", true, now - 400 * kDay);
    EXPECT_GE(players_->applyRetention(now).total(), 1u);
    EXPECT_EQ(players_->applyRetention(now).total(), 0u);
}
