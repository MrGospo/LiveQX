// AuditDb — schema + insert/query + HMAC-chain + verifyChain + purge.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include <sqlite3.h>

#include "audit/AuditDb.h"
#include "audit/AuditTypes.h"
#include "auth/MasterKey.h"

namespace fs = std::filesystem;
using liveqx::audit::AuditDb;
using liveqx::audit::AuditEvent;
using liveqx::audit::AuditFilter;
using liveqx::audit::Category;
using liveqx::auth::MasterKey;

namespace {

class AuditDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stem = "audit_db_test_" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        tmp_dir_ = fs::temp_directory_path() / stem;
        fs::create_directories(tmp_dir_);
        key_path_ = tmp_dir_ / "master.key";
        key_ = std::make_unique<MasterKey>(key_path_.string());
        ASSERT_TRUE(key_->load());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    fs::path                    tmp_dir_;
    fs::path                    key_path_;
    std::unique_ptr<MasterKey>  key_;
};

AuditEvent makeEvent(const std::string& action, Category c = Category::Channel) {
    AuditEvent e;
    e.category = c;
    e.action = action;
    e.actor_user_id = 42;
    e.actor_username = "admin";
    e.actor_role = "admin";
    e.actor_ip = "10.0.0.5";
    e.target_type = "channel";
    e.target_id = "7";
    e.http_method = "PATCH";
    e.http_path = "/api/channels/7";
    e.http_status = 200;
    e.summary = "channel 7 updated";
    e.details_json = R"({"field":"name","from":"a","to":"b"})";
    return e;
}

}  // namespace

TEST_F(AuditDbTest, OpenCreatesSchemaAndIsIdempotent) {
    {
        AuditDb db(tmp_dir_ / "audit.db", key_.get());
        ASSERT_TRUE(db.open());
        EXPECT_TRUE(db.ok());
    }
    AuditDb db2(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db2.open());
    EXPECT_EQ(0, db2.count({}));
}

TEST_F(AuditDbTest, InsertFillsIdAndMacAndFingerprint) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    auto ev = makeEvent("channel.update");
    auto id = db.insert(ev);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(1, *id);
    EXPECT_EQ(32u, ev.mac.size());
    EXPECT_TRUE(ev.prev_mac.empty());
    EXPECT_EQ(16u, ev.key_fingerprint.size());
}

TEST_F(AuditDbTest, ChainLinksSuccessiveRows) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    auto a = makeEvent("channel.create");
    auto b = makeEvent("channel.update");
    ASSERT_TRUE(db.insert(a).has_value());
    ASSERT_TRUE(db.insert(b).has_value());

    EXPECT_EQ(a.mac, b.prev_mac);
    EXPECT_NE(a.mac, b.mac);
}

TEST_F(AuditDbTest, InsertBatchIsAtomicAndChained) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    std::vector<AuditEvent> batch;
    for (int i = 0; i < 5; ++i)
        batch.push_back(makeEvent("channel.update" + std::to_string(i)));

    const auto n = db.insertBatch(batch);
    ASSERT_EQ(5u, n);
    for (std::size_t i = 1; i < batch.size(); ++i)
        EXPECT_EQ(batch[i - 1].mac, batch[i].prev_mac);
}

TEST_F(AuditDbTest, ListFiltersByCategoryAndActor) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    auto ch = makeEvent("channel.update", Category::Channel);
    auto au = makeEvent("login.ok",       Category::Auth);
    au.actor_user_id = 99;
    au.actor_username = "root";
    ASSERT_TRUE(db.insert(ch).has_value());
    ASSERT_TRUE(db.insert(au).has_value());

    AuditFilter f;
    f.category = Category::Auth;
    auto rows = db.list(f);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ("login.ok", rows[0].action);

    AuditFilter g;
    g.actor_user_id = 42;
    auto rows2 = db.list(g);
    ASSERT_EQ(1u, rows2.size());
    EXPECT_EQ("channel.update", rows2[0].action);
}

TEST_F(AuditDbTest, ListReturnsNewestFirst) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    auto first  = makeEvent("first");
    first.ts_unix_ms = 1000;
    auto second = makeEvent("second");
    second.ts_unix_ms = 2000;
    ASSERT_TRUE(db.insert(first).has_value());
    ASSERT_TRUE(db.insert(second).has_value());

    auto rows = db.list({});
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ("second", rows[0].action);
    EXPECT_EQ("first",  rows[1].action);
}

TEST_F(AuditDbTest, VerifyChainDetectsClean) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    for (int i = 0; i < 20; ++i) {
        auto ev = makeEvent("a" + std::to_string(i));
        ASSERT_TRUE(db.insert(ev).has_value());
    }
    auto v = db.verifyChain();
    EXPECT_EQ(20, v.scanned);
    EXPECT_EQ(0, v.first_bad_id);
    EXPECT_TRUE(v.first_bad_reason.empty());
}

TEST_F(AuditDbTest, VerifyChainDetectsRowTamper) {
    const auto path = tmp_dir_ / "audit.db";
    {
        AuditDb db(path, key_.get());
        ASSERT_TRUE(db.open());
        for (int i = 0; i < 5; ++i) {
            auto ev = makeEvent("a" + std::to_string(i));
            ASSERT_TRUE(db.insert(ev).has_value());
        }
    }

    // Tamper: rewrite one row's summary directly. mac stays put, so
    // verifyChain must flag mac_mismatch for that id.
    sqlite3* raw = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(path.string().c_str(), &raw));
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(raw,
        "UPDATE audit_events SET summary='tampered' WHERE id=3",
        nullptr, nullptr, nullptr));
    sqlite3_close(raw);

    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open());
    auto v = db.verifyChain();
    EXPECT_EQ(3, v.first_bad_id);
    EXPECT_EQ("mac_mismatch", v.first_bad_reason);
}

TEST_F(AuditDbTest, PurgeOlderThanKeepsNewerRows) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    for (int i = 0; i < 10; ++i) {
        auto ev = makeEvent("a" + std::to_string(i), Category::Channel);
        ev.ts_unix_ms = 1000 + i * 100;
        ASSERT_TRUE(db.insert(ev).has_value());
    }
    // Cutoff drops the first three (ts_unix_ms < 1300).
    const auto removed = db.purgeOlderThan(Category::Channel, 1300);
    EXPECT_EQ(3, removed);
    EXPECT_EQ(7, db.count({}));
}

TEST_F(AuditDbTest, PurgeRespectsCategoryScope) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());

    auto ch = makeEvent("channel.x", Category::Channel);
    ch.ts_unix_ms = 100;
    auto au = makeEvent("login.ok",  Category::Auth);
    au.ts_unix_ms = 100;
    ASSERT_TRUE(db.insert(ch).has_value());
    ASSERT_TRUE(db.insert(au).has_value());

    const auto removed = db.purgeOlderThan(Category::Channel, 1000);
    EXPECT_EQ(1, removed);
    // Auth row survives even though it is older than cutoff.
    EXPECT_EQ(1, db.count({}));
}
