// AuditDb — schema + insert/query + HMAC-chain + verifyChain + purge.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

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

// ── the place an event is about (schema v2) ────────────────────────────────
namespace {

using nlohmann::json;

// What a version-1 binary wrote: the same fields, in the same way, with no
// place in the signature. Kept here, independent of the current code, so the
// test proves that rows written years ago still verify.
std::string oldCanonical(std::int64_t ts, const std::string& category, const std::string& action,
                         const std::string& summary) {
    json j;
    j["ts_unix_ms"]     = ts;
    j["category"]       = category;
    j["action"]         = action;
    j["actor_user_id"]  = 42;
    j["actor_username"] = "admin";
    j["actor_role"]     = "admin";
    j["actor_ip"]       = "10.0.0.5";
    j["target_type"]    = "player";
    j["target_id"]      = "abc";
    j["http_method"]    = "POST";
    j["http_path"]      = "/api/players";
    j["http_status"]    = 200;
    j["elapsed_ms"]     = 0;
    j["summary"]        = summary;
    j["details_json"]   = "{}";
    j["request_id"]     = "";
    return j.dump();
}

// A real version-1 database with `n` signed rows.
void writeVersionOneDatabase(const fs::path& path, MasterKey& key, int n) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, R"sql(
CREATE TABLE audit_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts_unix_ms INTEGER NOT NULL, category TEXT NOT NULL,
    action TEXT NOT NULL, actor_user_id INTEGER, actor_username TEXT, actor_role TEXT, actor_ip TEXT,
    target_type TEXT, target_id TEXT, http_method TEXT, http_path TEXT, http_status INTEGER NOT NULL DEFAULT 0,
    elapsed_ms INTEGER NOT NULL DEFAULT 0, summary TEXT, details_json TEXT NOT NULL DEFAULT '{}',
    request_id TEXT, prev_mac BLOB, mac BLOB NOT NULL, key_fingerprint TEXT NOT NULL,
    legacy INTEGER NOT NULL DEFAULT 0);
CREATE INDEX idx_audit_events_ts ON audit_events(ts_unix_ms DESC);
CREATE TABLE audit_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
PRAGMA user_version = 1;)sql", nullptr, nullptr, nullptr), SQLITE_OK);

    std::vector<std::uint8_t> prev;
    for (int i = 0; i < n; ++i) {
        const std::int64_t ts = 1'700'000'000'000 + i;
        const std::string summary = "old row " + std::to_string(i);
        std::string msg(reinterpret_cast<const char*>(prev.data()), prev.size());
        msg += oldCanonical(ts, "player", "player.register", summary);
        const auto mac = key.hmacSha256(msg);
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(raw,
            "INSERT INTO audit_events(ts_unix_ms,category,action,actor_user_id,actor_username,actor_role,actor_ip,"
            "target_type,target_id,http_method,http_path,http_status,elapsed_ms,summary,details_json,request_id,"
            "prev_mac,mac,key_fingerprint,legacy) VALUES(?,?,?,42,'admin','admin','10.0.0.5','player','abc','POST',"
            "'/api/players',200,0,?,'{}','',?,?,?,0)", -1, &st, nullptr), SQLITE_OK);
        sqlite3_bind_int64(st, 1, ts);
        sqlite3_bind_text(st, 2, "player", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, "player.register", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, summary.c_str(), -1, SQLITE_TRANSIENT);
        if (prev.empty()) sqlite3_bind_null(st, 5); else sqlite3_bind_blob(st, 5, prev.data(), static_cast<int>(prev.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 6, mac.data(), static_cast<int>(mac.size()), SQLITE_TRANSIENT);
        const std::string fp = key.fingerprint();
        sqlite3_bind_text(st, 7, fp.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
        prev.assign(mac.begin(), mac.end());
    }
    // The chain tail the old binary kept.
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (auto b : prev) { hex.push_back(kHex[b >> 4]); hex.push_back(kHex[b & 15]); }
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO audit_meta(key,value) VALUES('last_mac',?)", -1, &st, nullptr), SQLITE_OK);
    sqlite3_bind_text(st, 1, hex.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

AuditEvent placed(const std::string& action, std::optional<std::int64_t> object, std::optional<std::int64_t> group,
                  Category c = Category::Player) {
    AuditEvent e = makeEvent(action, c);
    e.target_type = "player";
    e.target_id = "p";
    e.scope_object_id = object;
    e.scope_group_id = group;
    return e;
}

}  // namespace

TEST_F(AuditDbTest, ARowWrittenBeforePlacesExistedStillVerifiesAndTheChainContinuesWithPlacedRows) {
    const auto path = tmp_dir_ / "audit.db";
    writeVersionOneDatabase(path, *key_, 3);

    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open()) << "a version-1 database is migrated, not refused";
    auto before = db.verifyChain();
    EXPECT_EQ(before.scanned, 3);
    EXPECT_EQ(before.first_bad_id, 0) << before.first_bad_reason << " — old rows verify exactly as they always did";

    auto old_rows = db.list({});
    ASSERT_EQ(old_rows.size(), 3u);
    for (const auto& r : old_rows) {
        EXPECT_EQ(r.scoped, 0);
        EXPECT_FALSE(r.scope_object_id.has_value());
    }

    auto a = placed("player.command_sent", 7, 71);
    auto b = placed("player.move", 7, 72);
    ASSERT_TRUE(db.insert(a).has_value());
    std::vector<AuditEvent> batch = {b, placed("player.revoke", std::nullopt, std::nullopt)};
    ASSERT_EQ(db.insertBatch(batch), 2u);

    auto after = db.verifyChain();
    EXPECT_EQ(after.scanned, 6);
    EXPECT_EQ(after.first_bad_id, 0) << after.first_bad_reason << " — one unbroken chain across the two formats";
    auto all = db.list({});
    int scoped = 0;
    for (const auto& r : all) scoped += (r.scoped == 2);
    EXPECT_EQ(scoped, 3) << "every new row is of the current format, even without a place";
}

TEST_F(AuditDbTest, ThePlaceIsInsideTheSignatureOfANewRowAndTheFormatFlagCannotBeFlipped) {
    const auto path = tmp_dir_ / "audit.db";
    {
        AuditDb db(path, key_.get());
        ASSERT_TRUE(db.open());
        auto e1 = placed("player.command_sent", 7, 71);
        auto e2 = placed("player.command_sent", 7, 72);
        ASSERT_TRUE(db.insert(e1).has_value());
        ASSERT_TRUE(db.insert(e2).has_value());
        ASSERT_EQ(db.verifyChain().first_bad_id, 0);
    }
    auto tamper = [&](const char* sql) {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    };
    {   // someone moves an event to a group they would rather it were in
        tamper("UPDATE audit_events SET scope_group_id = 99 WHERE id = 1;");
        AuditDb db(path, key_.get());
        ASSERT_TRUE(db.open());
        auto v = db.verifyChain();
        EXPECT_EQ(v.first_bad_id, 1);
        EXPECT_EQ(v.first_bad_reason, "mac_mismatch");
    }
    tamper("UPDATE audit_events SET scope_group_id = 71 WHERE id = 1;");
    {   // ...or strips the place by claiming the row is of the old format
        tamper("UPDATE audit_events SET scoped = 0 WHERE id = 2;");
        AuditDb db(path, key_.get());
        ASSERT_TRUE(db.open());
        auto v = db.verifyChain();
        EXPECT_EQ(v.first_bad_id, 2);
        EXPECT_EQ(v.first_bad_reason, "mac_mismatch");
    }
}

TEST_F(AuditDbTest, AnOldRowCannotBeDressedUpWithAPlaceEither) {
    const auto path = tmp_dir_ / "audit.db";
    writeVersionOneDatabase(path, *key_, 2);
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        // the migration adds the columns; here we need them to forge with
        { AuditDb db(path, key_.get()); ASSERT_TRUE(db.open()); }
        ASSERT_EQ(sqlite3_exec(raw, "UPDATE audit_events SET scoped = 1, scope_group_id = 5 WHERE id = 1;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }
    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open());
    EXPECT_EQ(db.verifyChain().first_bad_id, 1);
}

TEST_F(AuditDbTest, MigratingTwiceChangesNothing) {
    const auto path = tmp_dir_ / "audit.db";
    writeVersionOneDatabase(path, *key_, 2);
    { AuditDb db(path, key_.get()); ASSERT_TRUE(db.open()); }
    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open());
    EXPECT_EQ(db.count({}), 2);
    EXPECT_EQ(db.verifyChain().first_bad_id, 0);
}

TEST_F(AuditDbTest, EventsCanBeFilteredByObjectAndByGroup) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    for (auto [o, g] : std::vector<std::pair<int, int>>{{1, 11}, {1, 12}, {2, 21}}) {
        auto e = placed("player.command_sent", o, g);
        ASSERT_TRUE(db.insert(e).has_value());
    }
    auto none = placed("player.settings_update", std::nullopt, std::nullopt);
    ASSERT_TRUE(db.insert(none).has_value());

    AuditFilter f;
    f.scope_object_id = 1;
    EXPECT_EQ(db.count(f), 2) << "an object finds the events of all its groups";
    f.scope_group_id = 12;
    EXPECT_EQ(db.count(f), 1);
    AuditFilter g;
    g.scope_group_id = 21;
    ASSERT_EQ(db.list(g).size(), 1u);
    EXPECT_EQ(*db.list(g)[0].scope_object_id, 2);
    EXPECT_EQ(db.count({}), 4) << "no place filter = everything, placed or not";
}

TEST_F(AuditDbTest, SomeoneWithRightsOnPlacesSeesPlayerEventsOfThosePlacesOnly) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    auto add = [&](const char* action, int o, int g, Category c = Category::Player) {
        auto e = placed(action, o, g, c);
        ASSERT_TRUE(db.insert(e).has_value());
    };
    add("a-g11", 1, 11);
    add("a-g12", 1, 12);
    add("a-g21", 2, 21);
    add("system-in-g11", 1, 11, Category::System);        // right place, wrong kind of event
    auto unplaced = placed("unplaced", std::nullopt, std::nullopt);
    ASSERT_TRUE(db.insert(unplaced).has_value());

    auto actions = [&](AuditFilter f) {
        std::set<std::string> out;
        for (const auto& e : db.list(f)) out.insert(e.action);
        return out;
    };
    AuditFilter by_group;
    by_group.restrict_to_places = true;
    by_group.allowed_group_ids = {11};
    EXPECT_EQ(actions(by_group), (std::set<std::string>{"a-g11"})) << "not the system row, not the sibling group";
    EXPECT_EQ(db.count(by_group), 1) << "the count obeys the same rule";

    AuditFilter by_object;
    by_object.restrict_to_places = true;
    by_object.allowed_object_ids = {1};
    EXPECT_EQ(actions(by_object), (std::set<std::string>{"a-g11", "a-g12"}));

    AuditFilter both;
    both.restrict_to_places = true;
    both.allowed_object_ids = {2};
    both.allowed_group_ids = {12};
    EXPECT_EQ(actions(both), (std::set<std::string>{"a-g12", "a-g21"}));

    AuditFilter nothing;
    nothing.restrict_to_places = true;
    EXPECT_TRUE(actions(nothing).empty()) << "no right to any place is no events, never all of them";
    EXPECT_EQ(db.count(nothing), 0);

    AuditFilter narrowed = by_object;
    narrowed.scope_group_id = 12;                         // asking for less is fine
    EXPECT_EQ(actions(narrowed), (std::set<std::string>{"a-g12"}));
    AuditFilter escaping = by_group;
    escaping.scope_group_id = 21;                         // asking for more is not
    EXPECT_TRUE(actions(escaping).empty());
}

// ── two places in one row (schema v3) ───────────────────────────────────────
namespace {

// What a version-2 binary wrote: the place is signed, the place it came from does not exist yet.
std::string v2Canonical(std::int64_t ts, const std::string& summary, std::optional<std::int64_t> object,
                        std::optional<std::int64_t> group) {
    json j = json::parse(oldCanonical(ts, "player", "player.command_sent", summary));
    j["scope_object_id"] = object ? json(*object) : json(nullptr);
    j["scope_group_id"]  = group ? json(*group) : json(nullptr);
    return j.dump();
}

// A database that went through both older versions: `v1` rows of the first format,
// then `v2` rows of the second, all signed in one chain as those binaries would have.
void writeVersionTwoDatabase(const fs::path& path, MasterKey& key, int v1, int v2) {
    writeVersionOneDatabase(path, key, v1);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,
        "ALTER TABLE audit_events ADD COLUMN scoped INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE audit_events ADD COLUMN scope_object_id INTEGER;"
        "ALTER TABLE audit_events ADD COLUMN scope_group_id INTEGER;"
        "PRAGMA user_version = 2;", nullptr, nullptr, nullptr), SQLITE_OK);
    std::vector<std::uint8_t> prev;
    {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(raw, "SELECT mac FROM audit_events ORDER BY id DESC LIMIT 1", -1, &st, nullptr), SQLITE_OK);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const auto* b = static_cast<const std::uint8_t*>(sqlite3_column_blob(st, 0));
            prev.assign(b, b + sqlite3_column_bytes(st, 0));
        }
        sqlite3_finalize(st);
    }
    for (int i = 0; i < v2; ++i) {
        const std::int64_t ts = 1'710'000'000'000 + i;
        const std::string summary = "v2 row " + std::to_string(i);
        std::string msg(reinterpret_cast<const char*>(prev.data()), prev.size());
        msg += v2Canonical(ts, summary, 7, 70 + i);
        const auto mac = key.hmacSha256(msg);
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(raw,
            "INSERT INTO audit_events(ts_unix_ms,category,action,actor_user_id,actor_username,actor_role,actor_ip,"
            "target_type,target_id,http_method,http_path,http_status,elapsed_ms,summary,details_json,request_id,"
            "prev_mac,mac,key_fingerprint,legacy,scoped,scope_object_id,scope_group_id) VALUES(?,'player','player.command_sent',42,"
            "'admin','admin','10.0.0.5','player','abc','POST','/api/players',200,0,?,'{}','',?,?,?,0,1,7,?)", -1, &st, nullptr), SQLITE_OK);
        sqlite3_bind_int64(st, 1, ts);
        sqlite3_bind_text(st, 2, summary.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, prev.data(), static_cast<int>(prev.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 4, mac.data(), static_cast<int>(mac.size()), SQLITE_TRANSIENT);
        const std::string fp = key.fingerprint();
        sqlite3_bind_text(st, 5, fp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, 70 + i);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
        prev.assign(mac.begin(), mac.end());
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (auto b : prev) { hex.push_back(kHex[b >> 4]); hex.push_back(kHex[b & 15]); }
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE audit_meta SET value=? WHERE key='last_mac'", -1, &st, nullptr), SQLITE_OK);
    sqlite3_bind_text(st, 1, hex.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

AuditEvent moved(const std::string& action, std::int64_t to_object, std::int64_t to_group,
                 std::int64_t from_object, std::int64_t from_group) {
    AuditEvent e = placed(action, to_object, to_group);
    e.scope_from_object_id = from_object;
    e.scope_from_group_id = from_group;
    return e;
}

}  // namespace

TEST_F(AuditDbTest, RowsOfAllThreeFormatsLiveInOneChainAndEachVerifiesByItsOwnRule) {
    const auto path = tmp_dir_ / "audit.db";
    writeVersionTwoDatabase(path, *key_, 2, 2);

    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open()) << "a version-2 database is migrated, not refused";
    auto before = db.verifyChain();
    EXPECT_EQ(before.scanned, 4);
    EXPECT_EQ(before.first_bad_id, 0) << before.first_bad_reason << " — rows of both older formats verify as they did";

    auto m = moved("player.move", 7, 72, 7, 71);
    ASSERT_TRUE(db.insert(m).has_value());
    auto plain = placed("player.command_sent", 7, 71);
    ASSERT_TRUE(db.insert(plain).has_value());

    auto after = db.verifyChain();
    EXPECT_EQ(after.scanned, 6);
    EXPECT_EQ(after.first_bad_id, 0) << after.first_bad_reason;
    std::map<int, int> by_format;
    for (const auto& r : db.list({})) ++by_format[r.scoped];
    EXPECT_EQ(by_format[0], 2);
    EXPECT_EQ(by_format[1], 2);
    EXPECT_EQ(by_format[2], 2);
}

TEST_F(AuditDbTest, BothPlacesOfAMoveAreSignedAndTheFormatCannotBeDowngradedOrUpgraded) {
    const auto path = tmp_dir_ / "audit.db";
    {
        AuditDb db(path, key_.get());
        ASSERT_TRUE(db.open());
        auto a = moved("player.move", 7, 72, 7, 71);
        ASSERT_TRUE(db.insert(a).has_value());
        ASSERT_EQ(db.verifyChain().first_bad_id, 0);
    }
    auto verdict = [&](const char* tamper) {
        sqlite3* raw = nullptr;
        EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(raw, tamper, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
        AuditDb db(path, key_.get());
        EXPECT_TRUE(db.open());
        return db.verifyChain();
    };
    auto v = verdict("UPDATE audit_events SET scope_from_group_id = 99 WHERE id = 1;");
    EXPECT_EQ(v.first_bad_id, 1) << "the place it came from is signed too";
    verdict("UPDATE audit_events SET scope_from_group_id = 71 WHERE id = 1;");
    v = verdict("UPDATE audit_events SET scoped = 1 WHERE id = 1;");
    EXPECT_EQ(v.first_bad_id, 1) << "calling it an older row hides the origin only if the signature allowed it";
    verdict("UPDATE audit_events SET scoped = 2 WHERE id = 1;");
    v = verdict("UPDATE audit_events SET scoped = 0 WHERE id = 1;");
    EXPECT_EQ(v.first_bad_id, 1);
}

TEST_F(AuditDbTest, ADatabaseWrittenAtFormatTwoCannotGainAnOriginAfterTheFact) {
    const auto path = tmp_dir_ / "audit.db";
    writeVersionTwoDatabase(path, *key_, 0, 2);
    { AuditDb db(path, key_.get()); ASSERT_TRUE(db.open()); }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE audit_events SET scoped = 2, scope_from_group_id = 5 WHERE id = 1;",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    AuditDb db(path, key_.get());
    ASSERT_TRUE(db.open());
    EXPECT_EQ(db.verifyChain().first_bad_id, 1);
}

TEST_F(AuditDbTest, AMoveIsFoundFromEitherEndAndCountedOnce) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    auto m = moved("player.move", 1, 12, 1, 11);
    ASSERT_TRUE(db.insert(m).has_value());
    auto other = moved("player.move", 2, 21, 2, 22);
    ASSERT_TRUE(db.insert(other).has_value());

    AuditFilter f;
    f.scope_group_id = 11;                                // where it came from
    EXPECT_EQ(db.count(f), 1);
    f.scope_group_id = 12;                                // where it went
    EXPECT_EQ(db.count(f), 1);
    f.scope_group_id = 13;
    EXPECT_EQ(db.count(f), 0);
    AuditFilter o;
    o.scope_object_id = 1;
    EXPECT_EQ(db.count(o), 1) << "one row, not one per end";
    o.scope_object_id = 2;
    EXPECT_EQ(db.list(o).size(), 1u);
}

TEST_F(AuditDbTest, WhoeverLostAScreenAndWhoeverGotItBothMayReadTheMove) {
    AuditDb db(tmp_dir_ / "audit.db", key_.get());
    ASSERT_TRUE(db.open());
    auto m = moved("player.move", 1, 12, 1, 11);
    ASSERT_TRUE(db.insert(m).has_value());
    auto elsewhere = moved("player.move", 2, 21, 2, 22);
    ASSERT_TRUE(db.insert(elsewhere).has_value());

    auto sees = [&](std::vector<std::int64_t> groups, std::vector<std::int64_t> objects) {
        AuditFilter f;
        f.restrict_to_places = true;
        f.allowed_group_ids = std::move(groups);
        f.allowed_object_ids = std::move(objects);
        return db.count(f);
    };
    EXPECT_EQ(sees({11}, {}), 1) << "the group that lost it";
    EXPECT_EQ(sees({12}, {}), 1) << "the group that received it";
    EXPECT_EQ(sees({13}, {}), 0);
    EXPECT_EQ(sees({}, {1}), 1);
    EXPECT_EQ(sees({11, 22}, {}), 2);
}
