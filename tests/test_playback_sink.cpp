#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <sqlite3.h>

#include "logging/FilePlaybackSink.h"
#include "logging/NullPlaybackSink.h"
#include "logging/SqlitePlaybackSink.h"

using liveqx::logging::FilePlaybackSink;
using liveqx::logging::IPlaybackSink;
using liveqx::logging::NullPlaybackSink;
using liveqx::logging::PlaybackEvent;
using liveqx::logging::SqlitePlaybackSink;
namespace fs = std::filesystem;

TEST(NullPlaybackSink, LogIsNoOp) {
    NullPlaybackSink s;
    PlaybackEvent ev{1, "/x.mp4", "video", 1000, 2000, 1.0,
                     "crossfade", "completed", ""};
    s.log(ev);  // must not throw / write anywhere
    SUCCEED();
}

TEST(NullPlaybackSink, QueryReturnsEmpty) {
    NullPlaybackSink s;
    IPlaybackSink::QueryParams p;
    p.channel_id = 1;
    const auto j = s.query(p);
    EXPECT_TRUE(j.contains("events"));
    EXPECT_TRUE(j["events"].is_array());
    EXPECT_EQ(j["events"].size(), 0u);
    EXPECT_TRUE(j["next_after_ns"].is_null());
}

TEST(NullPlaybackSink, StatusJsonReportsNoneType) {
    NullPlaybackSink s;
    const auto j = s.statusJson();
    EXPECT_EQ(j.value("sink_type", std::string{}), "none");
    EXPECT_EQ(j.value("queue_depth", -1), 0);
    EXPECT_EQ(j.value("dropped_count", -1), 0);
}

namespace {

class FilePlaybackSinkTest : public ::testing::Test {
protected:
    fs::path tmp_;
    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("file_sink_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(tmp_);
        fs::create_directories(tmp_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    static int64_t day_ns(int y, int m, int d, int hh = 12, int mm = 0) {
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
        tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = 0;
        return static_cast<int64_t>(timegm(&tm)) * 1'000'000'000LL;
    }

    static PlaybackEvent makeEv(int channel, int64_t start_ns,
                                const std::string& path = "/clip.mp4") {
        return PlaybackEvent{channel, path, "video",
                             start_ns, start_ns + 1'000'000'000LL,
                             1.0, "crossfade", "completed", ""};
    }

    static void waitDrained(IPlaybackSink& sink, int64_t expected_last_ns,
                            std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto j = sink.statusJson();
            if (!j["last_write_ns"].is_null() &&
                j["last_write_ns"].get<int64_t>() >= expected_last_ns)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL() << "writer did not drain to " << expected_last_ns;
    }
};

}  // namespace

TEST_F(FilePlaybackSinkTest, EmptyDirectoryQueryReturnsEmpty) {
    FilePlaybackSink sink(7, tmp_);
    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    const auto j = sink.query(p);
    EXPECT_EQ(j["events"].size(), 0u);
    EXPECT_TRUE(j["next_after_ns"].is_null());
    EXPECT_TRUE(fs::is_directory(tmp_ / "playback"));
}

TEST_F(FilePlaybackSinkTest, RoundTripPreservesFields) {
    FilePlaybackSink sink(7, tmp_);
    const auto base = day_ns(2026, 5, 3);
    int64_t last = 0;
    for (int i = 0; i < 10; ++i) {
        last = base + i * 1'000'000'000LL;
        sink.log(makeEv(7, last, "/clip-" + std::to_string(i) + ".mp4"));
    }
    waitDrained(sink, last);

    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 10u);
    EXPECT_EQ(j["events"][0].value("clip_path", std::string{}), "/clip-0.mp4");
    EXPECT_EQ(j["events"][9].value("clip_path", std::string{}), "/clip-9.mp4");
    EXPECT_EQ(j["events"][0].value("status", std::string{}), "completed");
}

TEST_F(FilePlaybackSinkTest, RotationByEventDateProducesTwoFiles) {
    FilePlaybackSink sink(7, tmp_);
    const auto d1 = day_ns(2026, 5, 3, 23, 59);
    const auto d2 = day_ns(2026, 5, 4, 0,  1);
    sink.log(makeEv(7, d1, "/a.mp4"));
    sink.log(makeEv(7, d2, "/b.mp4"));
    waitDrained(sink, d2);

    EXPECT_TRUE(fs::exists(tmp_ / "playback" / "file-2026-05-03.jsonl"));
    EXPECT_TRUE(fs::exists(tmp_ / "playback" / "file-2026-05-04.jsonl"));
}

TEST_F(FilePlaybackSinkTest, AfterNsCursorPagination) {
    FilePlaybackSink sink(7, tmp_);
    const auto base = day_ns(2026, 5, 3);
    std::vector<int64_t> ts;
    for (int i = 0; i < 5; ++i) {
        ts.push_back(base + i * 1'000'000'000LL);
        sink.log(makeEv(7, ts.back()));
    }
    waitDrained(sink, ts.back());

    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    p.after_ns = ts[1];   // expect ts[2..4]
    p.limit = 100;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 3u);
    EXPECT_EQ(j["events"][0].value("started_at_ns", int64_t{0}), ts[2]);
    EXPECT_EQ(j["events"][2].value("started_at_ns", int64_t{0}), ts[4]);
}

TEST_F(FilePlaybackSinkTest, NextAfterNsReturnedWhenLimitHit) {
    FilePlaybackSink sink(7, tmp_);
    const auto base = day_ns(2026, 5, 3);
    int64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        last = base + i * 1'000'000'000LL;
        sink.log(makeEv(7, last));
    }
    waitDrained(sink, last);

    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    p.limit = 3;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 3u);
    ASSERT_FALSE(j["next_after_ns"].is_null());
    EXPECT_EQ(j["next_after_ns"].get<int64_t>(), base + 2 * 1'000'000'000LL);
}

TEST_F(FilePlaybackSinkTest, StatusReportsFileType) {
    FilePlaybackSink sink(7, tmp_);
    const auto j = sink.statusJson();
    EXPECT_EQ(j.value("sink_type", std::string{}), "file");
    EXPECT_EQ(j.value("dropped_count", -1), 0);
    EXPECT_EQ(j.value("files_count", -1), 0);
}

namespace {

class SqlitePlaybackSinkTest : public ::testing::Test {
protected:
    fs::path tmp_;
    fs::path ch7_;
    fs::path ch12_;

    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("sqlite_sink_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(tmp_);
        fs::create_directories(tmp_);
        ch7_  = tmp_ / "ch7-Sport";
        ch12_ = tmp_ / "ch12-News";
        fs::create_directories(ch7_);
        fs::create_directories(ch12_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    static int64_t day_ns(int y, int m, int d, int hh = 12, int mm = 0) {
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
        tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = 0;
        return static_cast<int64_t>(timegm(&tm)) * 1'000'000'000LL;
    }

    static PlaybackEvent makeEv(int channel, int64_t start_ns,
                                const std::string& path = "/clip.mp4") {
        return PlaybackEvent{channel, path, "video",
                             start_ns, start_ns + 1'000'000'000LL,
                             1.0, "crossfade", "completed", ""};
    }

    static void waitDrained(IPlaybackSink& sink, int64_t expected_last_ns,
                            std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto j = sink.statusJson();
            if (!j["last_write_ns"].is_null() &&
                j["last_write_ns"].get<int64_t>() >= expected_last_ns)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL() << "writer did not drain to " << expected_last_ns;
    }
};

}  // namespace

TEST_F(SqlitePlaybackSinkTest, RoundTripSingleWeek) {
    SqlitePlaybackSink sink(/*default_retention_days=*/30);
    sink.registerChannel(7, ch7_);
    const auto base = day_ns(2026, 5, 4);  // Monday W19 of 2026
    int64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        last = base + i * 1'000'000'000LL;
        sink.log(makeEv(7, last, "/clip-" + std::to_string(i) + ".mp4"));
    }
    waitDrained(sink, last);

    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 5u);
    EXPECT_EQ(j["events"][0].value("clip_path", std::string{}), "/clip-0.mp4");
    EXPECT_EQ(j["events"][4].value("clip_path", std::string{}), "/clip-4.mp4");
}

TEST_F(SqlitePlaybackSinkTest, WeeklyPartitioningCreatesTwoFiles) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto wk_a = day_ns(2026, 5, 4);  // Mon W19
    const auto wk_b = day_ns(2026, 5, 11); // Mon W20
    sink.log(makeEv(7, wk_a));
    sink.log(makeEv(7, wk_b));
    waitDrained(sink, wk_b);

    int week_files = 0;
    for (const auto& e : fs::directory_iterator(ch7_ / "playback")) {
        if (e.is_regular_file() &&
            e.path().filename().string().rfind("db-", 0) == 0 &&
            e.path().extension() == ".db") {
            ++week_files;
        }
    }
    EXPECT_EQ(week_files, 2);
}

TEST_F(SqlitePlaybackSinkTest, ChannelsAreIsolated) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7,  ch7_);
    sink.registerChannel(12, ch12_);
    const auto base = day_ns(2026, 5, 4);
    sink.log(makeEv(7,  base,                    "/sport.mp4"));
    sink.log(makeEv(12, base + 500'000'000LL,    "/news.mp4"));
    waitDrained(sink, base + 500'000'000LL);

    IPlaybackSink::QueryParams p7;  p7.channel_id  = 7;
    IPlaybackSink::QueryParams p12; p12.channel_id = 12;
    const auto j7  = sink.query(p7);
    const auto j12 = sink.query(p12);
    ASSERT_EQ(j7 ["events"].size(), 1u);
    ASSERT_EQ(j12["events"].size(), 1u);
    EXPECT_EQ(j7 ["events"][0].value("clip_path", std::string{}), "/sport.mp4");
    EXPECT_EQ(j12["events"][0].value("clip_path", std::string{}), "/news.mp4");
}

TEST_F(SqlitePlaybackSinkTest, FromToFiltersEvents) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto base = day_ns(2026, 5, 4);
    for (int i = 0; i < 5; ++i)
        sink.log(makeEv(7, base + i * 1'000'000'000LL));
    waitDrained(sink, base + 4 * 1'000'000'000LL);

    IPlaybackSink::QueryParams p;
    p.channel_id = 7;
    p.from_ns = base + 1 * 1'000'000'000LL;
    p.to_ns   = base + 3 * 1'000'000'000LL;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 3u);
    EXPECT_EQ(j["events"][0].value("started_at_ns", int64_t{0}),
              base + 1'000'000'000LL);
    EXPECT_EQ(j["events"][2].value("started_at_ns", int64_t{0}),
              base + 3'000'000'000LL);
}

TEST_F(SqlitePlaybackSinkTest, Utf8ClipPathRoundTrip) {
    SqlitePlaybackSink sink;
    sink.registerChannel(5, ch7_);
    const auto t = day_ns(2026, 5, 4);
    sink.log(makeEv(5, t, "/share/Канал-1/файл №3.mp4"));
    waitDrained(sink, t);

    IPlaybackSink::QueryParams p; p.channel_id = 5;
    const auto j = sink.query(p);
    ASSERT_EQ(j["events"].size(), 1u);
    EXPECT_EQ(j["events"][0].value("clip_path", std::string{}),
              "/share/Канал-1/файл №3.mp4");
}

TEST_F(SqlitePlaybackSinkTest, StatusReportsDbSink) {
    SqlitePlaybackSink sink(42);
    sink.registerChannel(7, ch7_);
    const auto j = sink.statusJson();
    EXPECT_EQ(j.value("sink_type",       std::string{}), "db");
    EXPECT_EQ(j.value("retention_days",  -1), 42);
    EXPECT_EQ(j.value("channels_count",  -1), 1);
    EXPECT_EQ(j.value("dropped_count",   -1), 0);
}

TEST_F(SqlitePlaybackSinkTest, WeekStringIso8601) {
    // 2026-01-01 is Thursday → ISO week 2026-W01.
    // 2025-12-29 is Monday   → ISO week 2026-W01 (year shift).
    EXPECT_EQ(SqlitePlaybackSink::weekString(day_ns(2026, 1, 1)), "2026-W01");
    EXPECT_EQ(SqlitePlaybackSink::weekString(day_ns(2025, 12, 29)),
              "2026-W01");
    EXPECT_EQ(SqlitePlaybackSink::weekString(day_ns(2026, 5, 4)), "2026-W19");
}

TEST_F(SqlitePlaybackSinkTest, NewerSchemaIsRefused) {
    // Pre-seed a future-version DB at the path the writer would target.
    fs::create_directories(ch7_ / "playback");
    const auto path = (ch7_ / "playback" / "db-2026-W19.db").string();
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=99;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto t = day_ns(2026, 5, 4);
    sink.log(makeEv(7, t));

    // Writer opens, prepareSchema throws, handle is dropped without insert.
    // Wait long enough for the writer to attempt at least once.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto j = sink.statusJson();
    EXPECT_GE(j.value("schema_errors", uint64_t{0}), uint64_t{1});
    EXPECT_TRUE(j["last_write_ns"].is_null());
    EXPECT_EQ(j.value("schema_version", -1), 1);
}

TEST(NullPlaybackSink, PurgeIsNoOp) {
    NullPlaybackSink s;
    IPlaybackSink::PurgeParams p;
    p.channel_id = 1;
    const auto j = s.purge(p);
    EXPECT_EQ(j.value("deleted_rows",  -1), 0);
    EXPECT_EQ(j.value("removed_files", -1), 0);
}

TEST_F(FilePlaybackSinkTest, PurgeFullyCoveredDayFileIsRemoved) {
    FilePlaybackSink sink(7, tmp_);
    const auto d1 = day_ns(2026, 5, 3, 12, 0);
    const auto d2 = day_ns(2026, 5, 4, 12, 0);
    sink.log(makeEv(7, d1));
    sink.log(makeEv(7, d2));
    waitDrained(sink, d2);
    ASSERT_TRUE(fs::exists(tmp_ / "playback" / "file-2026-05-03.jsonl"));
    ASSERT_TRUE(fs::exists(tmp_ / "playback" / "file-2026-05-04.jsonl"));

    IPlaybackSink::PurgeParams p;
    p.channel_id = 7;
    p.from_ns = day_ns(2026, 5, 3, 0, 0);
    p.to_ns   = day_ns(2026, 5, 4, 0, 0) - 1;  // последняя ns 2026-05-03
    const auto j = sink.purge(p);
    EXPECT_EQ(j.value("removed_files", -1), 1);
    EXPECT_FALSE(fs::exists(tmp_ / "playback" / "file-2026-05-03.jsonl"));
    EXPECT_TRUE (fs::exists(tmp_ / "playback" / "file-2026-05-04.jsonl"));
}

TEST_F(FilePlaybackSinkTest, PurgePartialDayKeepsFile) {
    // FilePlaybackSink удаляет только целиком покрытые дневные файлы.
    // Частично перекрывающий диапазон — это шум, файл остаётся.
    FilePlaybackSink sink(7, tmp_);
    const auto morning = day_ns(2026, 5, 3, 6, 0);
    const auto evening = day_ns(2026, 5, 3, 22, 0);
    sink.log(makeEv(7, morning));
    sink.log(makeEv(7, evening));
    waitDrained(sink, evening);

    IPlaybackSink::PurgeParams p;
    p.channel_id = 7;
    p.from_ns = day_ns(2026, 5, 3, 12, 0);  // полдень
    p.to_ns   = day_ns(2026, 5, 3, 23, 59) + 999'999'999LL;
    const auto j = sink.purge(p);
    EXPECT_EQ(j.value("removed_files", -1), 0);
    EXPECT_TRUE(fs::exists(tmp_ / "playback" / "file-2026-05-03.jsonl"));
}

TEST_F(SqlitePlaybackSinkTest, PurgeAllRemovesAllWeekFiles) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto wk_a = day_ns(2026, 5, 4);   // Mon W19
    const auto wk_b = day_ns(2026, 5, 11);  // Mon W20
    sink.log(makeEv(7, wk_a, "/a.mp4"));
    sink.log(makeEv(7, wk_b, "/b.mp4"));
    waitDrained(sink, wk_b);

    IPlaybackSink::PurgeParams p;
    p.channel_id = 7;
    const auto j = sink.purge(p);
    EXPECT_EQ(j.value("removed_files", -1), 2);

    IPlaybackSink::QueryParams qp; qp.channel_id = 7;
    EXPECT_EQ(sink.query(qp)["events"].size(), 0u);
}

TEST_F(SqlitePlaybackSinkTest, PurgeRangePartialIntersectionDeletesRows) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto base = day_ns(2026, 5, 4);  // Mon W19
    for (int i = 0; i < 5; ++i)
        sink.log(makeEv(7, base + i * 1'000'000'000LL));
    waitDrained(sink, base + 4 * 1'000'000'000LL);

    // Диапазон покрывает только середину недели, не всю → файл остаётся,
    // удалятся только записи.
    IPlaybackSink::PurgeParams p;
    p.channel_id = 7;
    p.from_ns = base + 1 * 1'000'000'000LL;
    p.to_ns   = base + 3 * 1'000'000'000LL;
    const auto j = sink.purge(p);
    EXPECT_EQ(j.value("removed_files", -1), 0);
    EXPECT_EQ(j.value("deleted_rows",  -1), 3);

    IPlaybackSink::QueryParams qp; qp.channel_id = 7;
    const auto q = sink.query(qp);
    ASSERT_EQ(q["events"].size(), 2u);
    EXPECT_EQ(q["events"][0].value("started_at_ns", int64_t{0}), base);
    EXPECT_EQ(q["events"][1].value("started_at_ns", int64_t{0}),
              base + 4'000'000'000LL);
}

TEST_F(SqlitePlaybackSinkTest, PurgeRespectsChannelIsolation) {
    SqlitePlaybackSink sink;
    sink.registerChannel(7,  ch7_);
    sink.registerChannel(12, ch12_);
    const auto t = day_ns(2026, 5, 4);
    sink.log(makeEv(7,  t,                 "/sport.mp4"));
    sink.log(makeEv(12, t + 500'000'000LL, "/news.mp4"));
    waitDrained(sink, t + 500'000'000LL);

    IPlaybackSink::PurgeParams p;
    p.channel_id = 7;  // удаляем только канал 7
    sink.purge(p);

    IPlaybackSink::QueryParams q7;  q7.channel_id  = 7;
    IPlaybackSink::QueryParams q12; q12.channel_id = 12;
    EXPECT_EQ(sink.query(q7)["events"].size(),  0u);
    ASSERT_EQ(sink.query(q12)["events"].size(), 1u);
    EXPECT_EQ(sink.query(q12)["events"][0].value("clip_path", std::string{}),
              "/news.mp4");
}

TEST_F(SqlitePlaybackSinkTest, PurgeUnknownChannelReturnsZeros) {
    SqlitePlaybackSink sink;
    IPlaybackSink::PurgeParams p;
    p.channel_id = 999;
    const auto j = sink.purge(p);
    EXPECT_EQ(j.value("deleted_rows",  -1), 0);
    EXPECT_EQ(j.value("removed_files", -1), 0);
}

TEST_F(SqlitePlaybackSinkTest, ExistingV1DbReusedWithoutBumping) {
    // v1 file already exists → no migration, write succeeds.
    fs::create_directories(ch7_ / "playback");
    const auto path = (ch7_ / "playback" / "db-2026-W19.db").string();
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=1;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    SqlitePlaybackSink sink;
    sink.registerChannel(7, ch7_);
    const auto t = day_ns(2026, 5, 4);
    sink.log(makeEv(7, t));
    waitDrained(sink, t);

    IPlaybackSink::QueryParams p; p.channel_id = 7;
    EXPECT_EQ(sink.query(p)["events"].size(), 1u);
}
