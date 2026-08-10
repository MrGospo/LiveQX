// fix17 step 2 — unit tests for ChannelStatePersistence.
//
// Each test gets its own tmp dir under <build>/Testing/<test_name>/state.db
// so parallel ctest runs don't fight over the same SQLite file. We use the
// pid + a counter to keep the path unique even if a prior run crashed
// before cleanup.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "persistence/StatePersistence.h"

namespace fs = std::filesystem;
using liveqx::persistence::ChannelStatePersistence;
using liveqx::persistence::ChannelStateSnapshot;

namespace {

fs::path freshTmpDir(const char* tag) {
    static std::atomic<int> counter{0};
    auto base = fs::temp_directory_path() / "liveqx-state-tests";
    fs::create_directories(base);
    auto dir = base /
        (std::string(tag) + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(counter.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST(ChannelStatePersistence, CreatesFileAndOpensCleanly) {
    const auto dir = freshTmpDir("create");
    const auto db_path = dir / "state.db";
    ChannelStatePersistence p(db_path);
    EXPECT_TRUE(p.ok());
    EXPECT_TRUE(fs::exists(db_path));
}

TEST(ChannelStatePersistence, EmptySnapshotSaveIsNoop) {
    const auto db_path = freshTmpDir("empty") / "state.db";
    ChannelStatePersistence p(db_path);
    EXPECT_TRUE(p.save({}));
    auto snap = p.load();
    EXPECT_TRUE(snap.empty());
}

TEST(ChannelStatePersistence, RoundTripsCursorPausedSchedule) {
    const auto db_path = freshTmpDir("rt") / "state.db";
    ChannelStateSnapshot s;
    s.playlist_index   = 3;
    s.clip_path        = "/share/movie.mp4";
    s.slot_pos_sec     = 12.5;
    s.paused           = false;
    s.schedule_active  = nlohmann::json{{"mode", "schedule"},
                                        {"entry_id", 7},
                                        {"window_end_ns", 1700000000000000000LL}};
    {
        ChannelStatePersistence p(db_path);
        ASSERT_TRUE(p.save(s));
    }
    {
        ChannelStatePersistence p(db_path);
        auto out = p.load();
        ASSERT_TRUE(out.playlist_index.has_value());
        EXPECT_EQ(*out.playlist_index, 3);
        ASSERT_TRUE(out.clip_path.has_value());
        EXPECT_EQ(*out.clip_path, "/share/movie.mp4");
        ASSERT_TRUE(out.slot_pos_sec.has_value());
        EXPECT_DOUBLE_EQ(*out.slot_pos_sec, 12.5);
        ASSERT_TRUE(out.paused.has_value());
        EXPECT_FALSE(*out.paused);
        ASSERT_TRUE(out.schedule_active.has_value());
        EXPECT_EQ((*out.schedule_active)["mode"], "schedule");
        EXPECT_EQ((*out.schedule_active)["entry_id"], 7);
    }
}

TEST(ChannelStatePersistence, OverwriteSavesUseLatestValue) {
    const auto db_path = freshTmpDir("overwrite") / "state.db";
    ChannelStatePersistence p(db_path);

    ChannelStateSnapshot s1;
    s1.playlist_index = 0;
    s1.slot_pos_sec   = 0.0;
    s1.paused         = false;
    ASSERT_TRUE(p.save(s1));

    ChannelStateSnapshot s2;
    s2.playlist_index = 4;
    s2.slot_pos_sec   = 7.7;
    s2.paused         = true;
    ASSERT_TRUE(p.save(s2));

    auto out = p.load();
    ASSERT_TRUE(out.playlist_index.has_value());
    EXPECT_EQ(*out.playlist_index, 4);
    ASSERT_TRUE(out.slot_pos_sec.has_value());
    EXPECT_DOUBLE_EQ(*out.slot_pos_sec, 7.7);
    ASSERT_TRUE(out.paused.has_value());
    EXPECT_TRUE(*out.paused);
}

TEST(ChannelStatePersistence, MissingFileLoadsEmpty) {
    const auto db_path = freshTmpDir("missing") / "state.db";
    ChannelStatePersistence p(db_path);  // creates fresh empty file
    auto out = p.load();
    EXPECT_TRUE(out.empty());
}

TEST(ChannelStatePersistence, CorruptFileGetsRenamedAndStartsFresh) {
    const auto dir = freshTmpDir("corrupt");
    const auto db_path = dir / "state.db";

    // Plant a non-SQLite blob.
    {
        std::ofstream f(db_path, std::ios::binary);
        f << "not a sqlite database — definitely garbage";
    }

    ChannelStatePersistence p(db_path);
    // Either openAndPrepare bailed (ok==false and the corrupt file got
    // renamed), or sqlite_open silently succeeded on the garbage and the
    // first PRAGMA / read failed instead. In both cases we expect:
    //   1. a *.corrupt-* sibling now exists
    //   2. load() returns empty
    auto snap = p.load();
    EXPECT_TRUE(snap.empty());

    bool found_corrupt_rename = false;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind("state.db.corrupt-", 0) == 0) {
            found_corrupt_rename = true;
            break;
        }
    }
    EXPECT_TRUE(found_corrupt_rename);

    // After a corrupt rename the next save() must reopen a fresh file.
    ChannelStateSnapshot fresh;
    fresh.playlist_index = 1;
    EXPECT_TRUE(p.save(fresh));
    auto out2 = p.load();
    ASSERT_TRUE(out2.playlist_index.has_value());
    EXPECT_EQ(*out2.playlist_index, 1);
}

TEST(ChannelStatePersistence, NewerSchemaVersionIsRefused) {
    const auto dir = freshTmpDir("future-schema");
    const auto db_path = dir / "state.db";

    // Hand-craft a SQLite file with user_version=99 — far above ours.
    {
        ChannelStatePersistence p(db_path);
        ASSERT_TRUE(p.ok());
    }
    // Bump pragma manually using sqlite3 API (re-open then exec).
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=99;",
                               nullptr, nullptr, &err), SQLITE_OK)
            << (err ? err : "?");
        sqlite3_close(db);
    }
    // Reopening through the wrapper must refuse without renaming
    // (operator might be downgrading).
    ChannelStatePersistence p2(db_path);
    EXPECT_FALSE(p2.ok());
    EXPECT_TRUE(fs::exists(db_path));   // not renamed
}
