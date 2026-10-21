// AuditLogger — async batching + emergency fallback + broken-glass.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <thread>

#include "audit/AuditDb.h"
#include "audit/AuditLogger.h"
#include "audit/AuditTypes.h"
#include "auth/MasterKey.h"

namespace fs = std::filesystem;
using liveqx::audit::AuditDb;
using liveqx::audit::AuditEvent;
using liveqx::audit::AuditLogger;
using liveqx::audit::Category;
using liveqx::auth::MasterKey;

namespace {

class AuditLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stem = "audit_logger_test_" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        tmp_dir_ = fs::temp_directory_path() / stem;
        fs::create_directories(tmp_dir_);
        key_ = std::make_unique<MasterKey>((tmp_dir_ / "master.key").string());
        ASSERT_TRUE(key_->load());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    fs::path                    tmp_dir_;
    std::unique_ptr<MasterKey>  key_;
};

AuditEvent mk(std::string action) {
    AuditEvent e;
    e.category = Category::Channel;
    e.action = std::move(action);
    e.actor_user_id = 1;
    e.actor_username = "admin";
    return e;
}

std::size_t countLines(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return 0;
    std::string line;
    std::size_t n = 0;
    while (std::getline(f, line)) if (!line.empty()) ++n;
    return n;
}

// Poll until predicate true or timeout.
template <class Pred>
bool waitFor(Pred p, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p();
}

}  // namespace

TEST_F(AuditLoggerTest, AsyncWriteLandsInDb) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    AuditLogger lg(&db, tmp_dir_ / "audit-emergency.jsonl");
    lg.start();

    for (int i = 0; i < 25; ++i) lg.log(mk("channel.update"));

    ASSERT_TRUE(waitFor([&] { return lg.stats().written_db >= 25; },
                        std::chrono::seconds(3)));
    lg.stop();
    EXPECT_EQ(25u, lg.stats().written_db);
    EXPECT_EQ(0u,  lg.stats().written_emergency);
    EXPECT_EQ(25,  db.count({}));
}

TEST_F(AuditLoggerTest, EmergencyFallbackWhenDbAbsent) {
    // db=nullptr → logger boots in emergency-only mode.
    const auto emerg = tmp_dir_ / "audit-emergency.jsonl";
    AuditLogger lg(nullptr, emerg);
    lg.start();

    for (int i = 0; i < 10; ++i) lg.log(mk("channel.create"));

    ASSERT_TRUE(waitFor([&] { return lg.stats().written_emergency >= 10; },
                        std::chrono::seconds(3)));
    lg.stop();
    EXPECT_EQ(10u, lg.stats().written_emergency);
    EXPECT_EQ(10u, countLines(emerg));
}

TEST_F(AuditLoggerTest, BrokenGlassAlwaysWritesEvenWithoutDb) {
    const auto emerg = tmp_dir_ / "audit-emergency.jsonl";
    AuditLogger lg(nullptr, emerg);
    lg.start();

    auto ev = mk("login.ok");
    ev.category = Category::Auth;
    lg.logSyncBrokenGlass(std::move(ev));

    lg.stop();
    EXPECT_EQ(1u, lg.stats().written_emergency);
    EXPECT_EQ(1u, countLines(emerg));
}

TEST_F(AuditLoggerTest, BrokenGlassPrefersDbWhenHealthy) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    AuditLogger lg(&db, tmp_dir_ / "audit-emergency.jsonl");
    lg.start();

    lg.logSyncBrokenGlass(mk("login.ok"));

    lg.stop();
    EXPECT_EQ(1u, lg.stats().written_db);
    EXPECT_EQ(0u, lg.stats().written_emergency);
    EXPECT_EQ(1,  db.count({}));
}

TEST_F(AuditLoggerTest, ShouldRejectMutationFalseAtStart) {
    AuditLogger lg(nullptr, tmp_dir_ / "audit-emergency.jsonl");
    lg.start();
    EXPECT_FALSE(lg.shouldRejectMutation());
    lg.stop();
}

// ── places ─────────────────────────────────────────────────────────────────
TEST_F(AuditLoggerTest, AnEventIsPlacedWhereItsTargetIsWhenItIsLoggedAndTheProducerCanSayOtherwise) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    AuditLogger lg(&db, tmp_dir_ / "audit-emergency.jsonl");
    // The screen "p1" sits in group 11 of object 1.
    lg.setScopeResolver([](const AuditEvent& e) -> std::optional<liveqx::audit::AuditScope> {
        if (e.target_type == "player" && e.target_id == "p1") return liveqx::audit::AuditScope{1, 11};
        return std::nullopt;
    });
    lg.start();

    AuditEvent resolved = mk("player.command_sent");
    resolved.category = Category::Player;
    resolved.target_type = "player";
    resolved.target_id = "p1";
    lg.log(resolved);

    AuditEvent unknown = mk("player.command_sent");
    unknown.category = Category::Player;
    unknown.target_type = "player";
    unknown.target_id = "someone-else";
    lg.log(unknown);

    AuditEvent told = mk("player.delete");                 // the producer knew better: the screen is already gone
    told.category = Category::Player;
    told.target_type = "player";
    told.target_id = "p1";
    told.scope_object_id = 5;
    told.scope_group_id = 55;
    lg.log(told);

    ASSERT_TRUE(waitFor([&] { return lg.stats().written_db >= 3; }, std::chrono::seconds(3)));
    lg.stop();
    auto rows = db.list({});
    ASSERT_EQ(rows.size(), 3u);
    std::map<std::string, AuditEvent> by;                  // newest first
    int n = 0;
    for (const auto& r : rows) by[std::to_string(n++)] = r;
    EXPECT_EQ(by["2"].scope_group_id, 11);
    EXPECT_EQ(by["2"].scope_object_id, 1);
    EXPECT_FALSE(by["1"].scope_group_id.has_value());
    EXPECT_EQ(by["0"].scope_group_id, 55) << "what the producer said is not overwritten";
    EXPECT_EQ(by["0"].scope_object_id, 5);
    EXPECT_EQ(db.verifyChain().first_bad_id, 0);
}

TEST_F(AuditLoggerTest, WithoutAResolverEventsSimplyHaveNoPlace) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    AuditLogger lg(&db, tmp_dir_ / "audit-emergency.jsonl");
    lg.start();
    lg.log(mk("channel.update"));
    ASSERT_TRUE(waitFor([&] { return lg.stats().written_db >= 1; }, std::chrono::seconds(3)));
    lg.stop();
    EXPECT_FALSE(db.list({})[0].scope_group_id.has_value());
}

TEST_F(AuditLoggerTest, TheEmergencyFileCarriesThePlaceToo) {
    const auto emerg = tmp_dir_ / "audit-emergency.jsonl";
    AuditLogger lg(nullptr, emerg);
    lg.setScopeResolver([](const AuditEvent&) { return std::optional<liveqx::audit::AuditScope>{{3, 33}}; });
    lg.start();
    lg.log(mk("player.command_sent"));
    ASSERT_TRUE(waitFor([&] { return lg.stats().written_emergency >= 1; }, std::chrono::seconds(3)));
    lg.stop();
    std::ifstream f(emerg);
    std::string line;
    ASSERT_TRUE(std::getline(f, line));
    EXPECT_NE(line.find("\"scope_group_id\":33"), std::string::npos) << line;
    EXPECT_NE(line.find("\"scope_object_id\":3"), std::string::npos) << line;
}
