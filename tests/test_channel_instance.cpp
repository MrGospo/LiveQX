#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/ChannelInstance.h"
#include "core/ScheduleEntry.h"
#include "core/Scheduler.h"
#include "core/Timeline.h"
#include "metrics/ChannelHealth.h"
#include "utils/Log.h"

using nlohmann::json;

namespace {

json minimalCfg(int id = 42) {
    return json{
        {"id",   id},
        {"name", "test"},
        {"resolution", "320x240"},
        {"fps", 25},
        {"bitrate", 1'000'000},
        {"preset", "ultrafast"},
        {"default_photo_duration", 5.0},
        {"output", {{"port", 19000}, {"latency_ms", 200}}},
    };
}

}  // namespace

TEST(ChannelInstance, BuildSetsLongLivedState) {
    auto ch = ChannelInstance::build(minimalCfg(7));
    ASSERT_TRUE(ch);
    EXPECT_EQ(ch->id(), 7);
    EXPECT_EQ(ch->name(), "test");
    EXPECT_FALSE(ch->isRunning());
    EXPECT_TRUE(ch->metrics());
    EXPECT_TRUE(ch->health());
}

TEST(ChannelInstance, StatusInStoppedState) {
    auto ch = ChannelInstance::build(minimalCfg());
    const auto s = ch->status();
    EXPECT_EQ(s["id"], 42);
    EXPECT_EQ(s["state"], "stopped");
    EXPECT_EQ(s["resolution"], "320x240");
    EXPECT_EQ(s["fps_target"], 25);
    EXPECT_EQ(s["bitrate"], 1'000'000);
    EXPECT_EQ(s["output"]["port"], 19000);
    EXPECT_EQ(s["srt_connected"], false);
    EXPECT_EQ(s["current_clip_index"], -1);  // empty playlist
}

TEST(ChannelInstance, SkipToNextOnEmptyIsNoOp) {
    auto ch = ChannelInstance::build(minimalCfg());
    ch->skipToNext();   // must not crash
    EXPECT_EQ(ch->status()["current_clip_index"], -1);
}

TEST(ChannelInstance, UpdateConfigAcceptsAllowedFields) {
    auto ch = ChannelInstance::build(minimalCfg());
    json patch = {
        {"bitrate", 2'500'000},
        {"preset",  "fast"},
    };
    EXPECT_TRUE(ch->updateConfig(patch));
    const auto s = ch->status();
    EXPECT_EQ(s["bitrate"], 2'500'000);
    EXPECT_EQ(s["preset"],  "fast");
    EXPECT_EQ(s["name"],    "test");
}

TEST(ChannelInstance, StatusExposesPreloadSecAndTimezoneDefaults) {
    auto ch = ChannelInstance::build(minimalCfg());
    const auto s = ch->status();
    // Defaults: preload_sec=4.0. Per fix33 C, missing channel_timezone means
    // "inherit server TZ" — channel_timezone is null and inherits_server_tz=true.
    // effective_timezone falls back to UTC when no server TZ provider is wired.
    ASSERT_TRUE(s.contains("preload_sec"));
    EXPECT_DOUBLE_EQ(s["preload_sec"].get<double>(), 4.0);
    ASSERT_TRUE(s.contains("channel_timezone"));
    EXPECT_TRUE(s["channel_timezone"].is_null());
    ASSERT_TRUE(s.contains("inherits_server_tz"));
    EXPECT_TRUE(s["inherits_server_tz"].get<bool>());
    ASSERT_TRUE(s.contains("effective_timezone"));
    EXPECT_EQ(s["effective_timezone"], "UTC");
}

TEST(ChannelInstance, StatusReflectsConfiguredPreloadSecAndTimezone) {
    json cfg = minimalCfg();
    cfg["preload_sec"] = 8.5;
    cfg["channel_timezone"] = "Europe/Moscow";
    auto ch = ChannelInstance::build(cfg);
    const auto s = ch->status();
    EXPECT_DOUBLE_EQ(s["preload_sec"].get<double>(), 8.5);
    EXPECT_EQ(s["channel_timezone"], "Europe/Moscow");
    EXPECT_FALSE(s["inherits_server_tz"].get<bool>());
    EXPECT_EQ(s["effective_timezone"], "Europe/Moscow");
}

TEST(ChannelInstance, UpdateConfigPatchesPreloadSecAndTimezone) {
    auto ch = ChannelInstance::build(minimalCfg());
    json patch = {
        {"preload_sec", 12.0},
        {"channel_timezone", "Asia/Tokyo"},
    };
    EXPECT_TRUE(ch->updateConfig(patch));
    const auto s = ch->status();
    // status() reads from cfg_, so the patched values surface immediately
    // (effective in-memory preload_sec_ freezes at buildRuntime — apply on play()).
    EXPECT_DOUBLE_EQ(s["preload_sec"].get<double>(), 12.0);
    EXPECT_EQ(s["channel_timezone"], "Asia/Tokyo");
    EXPECT_FALSE(s["inherits_server_tz"].get<bool>());
    EXPECT_EQ(s["effective_timezone"], "Asia/Tokyo");
}

// fix33 C — toggling channel_timezone back to null restores inherit mode.
TEST(ChannelInstance, PatchChannelTimezoneToNullRestoresInherit) {
    json cfg = minimalCfg();
    cfg["channel_timezone"] = "Asia/Tokyo";
    auto ch = ChannelInstance::build(cfg);
    EXPECT_FALSE(ch->inheritsServerTimezone());

    ch->setServerTimezoneGetter([]() { return std::string{"Europe/Berlin"}; });
    // explicit override → still false, effective stays at Tokyo
    EXPECT_FALSE(ch->inheritsServerTimezone());
    EXPECT_EQ(ch->effectiveTimezone(), "Asia/Tokyo");

    json patch = {{"channel_timezone", nullptr}};
    EXPECT_TRUE(ch->updateConfig(patch));
    EXPECT_TRUE(ch->inheritsServerTimezone());
    EXPECT_EQ(ch->effectiveTimezone(), "Europe/Berlin");

    const auto s = ch->status();
    EXPECT_TRUE(s["channel_timezone"].is_null());
    EXPECT_TRUE(s["inherits_server_tz"].get<bool>());
    EXPECT_EQ(s["effective_timezone"], "Europe/Berlin");
}

// fix33 C — applyServerTimezoneChange propagates new server TZ to inherit channels.
TEST(ChannelInstance, ApplyServerTimezoneChangeHotSwapsForInheritChannel) {
    auto ch = ChannelInstance::build(minimalCfg());
    EXPECT_TRUE(ch->inheritsServerTimezone());

    std::string srv = "UTC";
    ch->setServerTimezoneGetter([&srv]() { return srv; });
    EXPECT_EQ(ch->effectiveTimezone(), "UTC");

    srv = "America/Los_Angeles";
    ch->applyServerTimezoneChange();
    EXPECT_EQ(ch->effectiveTimezone(), "America/Los_Angeles");
}

// ─── Playlist API (Stage 3.2) ────────────────────────────────────────────────

namespace {
using PR = ChannelInstance::PlaylistResult;

json item(const std::string& path, double dur = 0.0) {
    json j = {{"path", path}};
    if (dur > 0) j["duration"] = dur;
    return j;
}
}  // namespace

TEST(ChannelInstancePlaylist, EmptyOnFreshBuild) {
    auto ch = ChannelInstance::build(minimalCfg());
    auto pl = ch->playlistJson();
    EXPECT_TRUE(pl.is_array());
    EXPECT_EQ(pl.size(), 0u);
}

TEST(ChannelInstancePlaylist, ReplaceWithImagesPopulatesPlaylist) {
    auto ch = ChannelInstance::build(minimalCfg());
    json items = json::array({
        item("test_media/photo1.png", 3.0),
        item("test_media/photo2.png", 2.0),
    });
    EXPECT_EQ(ch->replacePlaylist(items), PR::Ok);

    auto pl = ch->playlistJson();
    ASSERT_EQ(pl.size(), 2u);
    EXPECT_EQ(pl[0]["path"], "test_media/photo1.png");
    EXPECT_EQ(pl[1]["path"], "test_media/photo2.png");
    EXPECT_FALSE(pl[0]["pending_remove"]);
}

TEST(ChannelInstancePlaylist, ReplaceRejectsNonArray) {
    auto ch = ChannelInstance::build(minimalCfg());
    EXPECT_EQ(ch->replacePlaylist(json::object()), PR::BadJson);
    EXPECT_EQ(ch->replacePlaylist(json("oops")), PR::BadJson);
}

TEST(ChannelInstancePlaylist, ReplaceWithBadPathFails) {
    auto ch = ChannelInstance::build(minimalCfg());
    json items = json::array({ item("/nonexistent/file.png", 3.0) });
    EXPECT_EQ(ch->replacePlaylist(items), PR::ItemBuildFailed);
    EXPECT_EQ(ch->playlistJson().size(), 0u);  // no partial state
}

TEST(ChannelInstancePlaylist, AppendExtendsAndReportsFirstIdx) {
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({ item("test_media/photo1.png", 2.0) })),
              PR::Ok);

    int first = -1;
    EXPECT_EQ(ch->appendPlaylist(json::array({ item("test_media/photo2.png", 2.0) }),
                                 &first),
              PR::Ok);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(ch->playlistJson().size(), 2u);
}

TEST(ChannelInstancePlaylist, RemoveAtNonAnchoredIsPhysical) {
    // setPlaylist anchors the cursor on clips[0]. Removing a different idx
    // is a physical removal regardless of run state.
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({
                  item("test_media/photo1.png", 2.0),
                  item("test_media/photo2.png", 2.0),
                  item("test_media/photo1.png", 2.0),
              })), PR::Ok);

    bool was_active = true;
    EXPECT_EQ(ch->removeAt(1, &was_active), PR::Ok);
    EXPECT_FALSE(was_active);

    auto pl = ch->playlistJson();
    ASSERT_EQ(pl.size(), 2u);
    EXPECT_EQ(pl[0]["path"], "test_media/photo1.png");
    EXPECT_EQ(pl[1]["path"], "test_media/photo1.png");
}

TEST(ChannelInstancePlaylist, RemoveAtAnchoredMarksPending) {
    // Removing the cursor-anchored entry (index 0 right after setPlaylist)
    // is deferred to slot wrap; pending_remove flag is exposed via JSON.
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({
                  item("test_media/photo1.png", 2.0),
                  item("test_media/photo2.png", 2.0),
              })), PR::Ok);

    bool was_active = false;
    EXPECT_EQ(ch->removeAt(0, &was_active), PR::Ok);
    EXPECT_TRUE(was_active);

    auto pl = ch->playlistJson();
    ASSERT_EQ(pl.size(), 2u);
    EXPECT_TRUE(pl[0]["pending_remove"]);
    EXPECT_FALSE(pl[1]["pending_remove"]);
}

TEST(ChannelInstancePlaylist, RemoveAtOutOfRange) {
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({ item("test_media/photo1.png", 2.0) })),
              PR::Ok);
    EXPECT_EQ(ch->removeAt(99), PR::IndexOutOfRange);
    EXPECT_EQ(ch->removeAt(-1), PR::IndexOutOfRange);
}

TEST(ChannelInstancePlaylist, ClearEmptiesPlaylist) {
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({
                  item("test_media/photo1.png", 2.0),
                  item("test_media/photo2.png", 2.0),
              })), PR::Ok);
    EXPECT_EQ(ch->clearPlaylist(), PR::Ok);
    EXPECT_EQ(ch->playlistJson().size(), 0u);
}

TEST(ChannelInstancePlaylist, NotifyDeletedRemovesByPath) {
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({
                  item("test_media/photo1.png", 2.0),
                  item("test_media/photo2.png", 2.0),
              })), PR::Ok);

    EXPECT_EQ(ch->notifyDeleted("test_media/photo2.png"), PR::Ok);
    auto pl = ch->playlistJson();
    ASSERT_EQ(pl.size(), 1u);
    EXPECT_EQ(pl[0]["path"], "test_media/photo1.png");
}

TEST(ChannelInstancePlaylist, NotifyDeletedUnknownPath) {
    auto ch = ChannelInstance::build(minimalCfg());
    ASSERT_EQ(ch->replacePlaylist(json::array({ item("test_media/photo1.png", 2.0) })),
              PR::Ok);
    EXPECT_EQ(ch->notifyDeleted("not/in/playlist.png"), PR::NotFound);
    EXPECT_EQ(ch->notifyDeleted(""), PR::BadJson);
}

TEST(ChannelInstancePlaylist, ManagedByContentSyncRefusesMutations) {
    auto cfg = minimalCfg();
    cfg["content_source"] = {
        {"share_path", "/tmp/some-share"},
        {"cache_path", "/tmp/some-cache"},
    };
    auto ch = ChannelInstance::build(cfg);
    json items = json::array({ item("test_media/photo1.png", 2.0) });

    EXPECT_EQ(ch->replacePlaylist(items),  PR::ManagedByContentSync);
    EXPECT_EQ(ch->appendPlaylist(items),   PR::ManagedByContentSync);
    EXPECT_EQ(ch->removeAt(0),             PR::ManagedByContentSync);
    EXPECT_EQ(ch->clearPlaylist(),         PR::ManagedByContentSync);

    // GET still works.
    EXPECT_TRUE(ch->playlistJson().is_array());
}

TEST(ChannelInstance, UpdateConfigRejectsResolutionChange) {
    auto ch = ChannelInstance::build(minimalCfg());
    EXPECT_FALSE(ch->updateConfig(json{{"resolution", "640x480"}}));
    EXPECT_FALSE(ch->updateConfig(json{{"fps", 30}}));
    EXPECT_FALSE(ch->updateConfig(json{{"output", {{"port", 19001}}}}));
    // Original values preserved.
    const auto s = ch->status();
    EXPECT_EQ(s["resolution"], "320x240");
    EXPECT_EQ(s["fps_target"], 25);
    EXPECT_EQ(s["output"]["port"], 19000);
}

TEST(ChannelInstanceLogging, BuildRoutesLogsToPerChannelFile) {
    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path() /
        ("ch_log_it_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Log::init(tmp.string(), "trace");

    auto cfg = minimalCfg(99);
    cfg["name"] = "LogIT";
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);

    auto lg = spdlog::get("ch99");
    ASSERT_TRUE(lg) << "channel logger not registered";
    lg->flush();

    const auto expected = tmp / "ch99-LogIT.log";
    ASSERT_TRUE(fs::exists(expected)) << "missing " << expected;

    std::ifstream f(expected);
    std::stringstream ss; ss << f.rdbuf();
    const auto body = ss.str();
    // buildLongLived emits "playlist is empty" via the channel logger.
    EXPECT_NE(body.find("playlist is empty"), std::string::npos);
    EXPECT_NE(body.find("[ch99:LogIT]"), std::string::npos);

    // Main log must NOT receive the channel-tagged line.
    const auto main_body = [&] {
        std::ifstream mf(tmp / "liveqx.log");
        std::stringstream s; s << mf.rdbuf(); return s.str();
    }();
    EXPECT_EQ(main_body.find("playlist is empty"), std::string::npos);

    spdlog::shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(ChannelInstanceLogging, BuildWithChannelDirRoutesLogsToChannelLog) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_dir_it_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    Log::init((root / "global_logs").string(), "trace");

    auto cfg = minimalCfg(77);
    cfg["name"] = "DirIT";
    const auto channel_dir = root / "ch77-DirIT";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    EXPECT_EQ(ch->channelDir(), channel_dir);

    EXPECT_TRUE(fs::is_directory(channel_dir / "logs"));
    EXPECT_TRUE(fs::is_directory(channel_dir / "cache"));

    auto lg = spdlog::get("ch77");
    ASSERT_TRUE(lg);
    lg->flush();

    const auto expected = channel_dir / "logs" / "channel.log";
    ASSERT_TRUE(fs::exists(expected)) << "missing " << expected;

    std::ifstream f(expected);
    std::stringstream ss; ss << f.rdbuf();
    EXPECT_NE(ss.str().find("[ch77:DirIT]"), std::string::npos);

    // Legacy per-channel file MUST NOT appear when channel_dir is set.
    EXPECT_FALSE(fs::exists(root / "global_logs" / "ch77-DirIT.log"));

    spdlog::shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ChannelInstancePersist, UpdateConfigWritesAtomicConfigJson) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_persist_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(55);
    cfg["name"] = "Persist";
    const auto channel_dir = root / "ch55-Persist";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);

    const auto cfg_path = channel_dir / "config.json";
    // Build alone does NOT write config.json; only updateConfig persists.
    EXPECT_FALSE(fs::exists(cfg_path));

    ASSERT_TRUE(ch->updateConfig(json{{"bitrate", 7'000'000}}));
    ASSERT_TRUE(fs::exists(cfg_path));

    std::ifstream f(cfg_path);
    json on_disk = json::parse(f);
    EXPECT_EQ(on_disk.value("bitrate", 0), 7'000'000);
    EXPECT_EQ(on_disk.value("name", std::string{}), "Persist");
    // tmp file must be cleaned up (not left behind by rename).
    EXPECT_FALSE(fs::exists(channel_dir / "config.json.tmp"));

    // Second patch overwrites — round-trip again.
    ASSERT_TRUE(ch->updateConfig(json{{"preset", std::string("fast")}}));
    on_disk = json::parse(std::ifstream(cfg_path));
    EXPECT_EQ(on_disk.value("bitrate", 0), 7'000'000);
    EXPECT_EQ(on_disk.value("preset", std::string{}), "fast");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ChannelInstancePersist, LegacyModeDoesNotWriteConfigJson) {
    auto cfg = minimalCfg(56);
    auto ch = ChannelInstance::build(cfg);  // empty channel_dir
    ASSERT_TRUE(ch);
    EXPECT_TRUE(ch->channelDir().empty());
    // updateConfig still succeeds — persistence is just skipped silently.
    EXPECT_TRUE(ch->updateConfig(json{{"bitrate", 5'000'000}}));
}

// fix17: per-channel state.db is created during build() when the
// channel has a directory; legacy mode (empty channel_dir) skips it.
TEST(ChannelInstanceStateDb, BuildOpensStateDb) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_state_db_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(91);
    cfg["name"] = "StateDb";
    const auto channel_dir = root / "ch91-StateDb";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    EXPECT_TRUE(fs::exists(channel_dir / "state.db"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ChannelInstanceStateDb, LegacyModeSkipsStateDb) {
    auto cfg = minimalCfg(92);
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    // No directory → no state.db on disk anywhere; just verify the channel
    // is functional without persistence wired.
    EXPECT_TRUE(ch->channelDir().empty());
}

// fix17 — pause() persists paused=true; subsequent build() honours it via
// wasPausedAtLastSave(). Mirrors what main.cpp's bootstrap consults to
// skip auto-play of operator-stopped channels across a process restart.
TEST(ChannelInstanceStateDb, PauseFlagSurvivesRebuild) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_state_pause_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(93);
    cfg["name"] = "Pause";
    const auto channel_dir = root / "ch93-Pause";

    {
        auto ch = ChannelInstance::build(cfg, channel_dir);
        ASSERT_TRUE(ch);
        EXPECT_FALSE(ch->wasPausedAtLastSave());
        ch->pause();   // marks paused_intent_=true and flushes to disk
    }

    // Second build re-reads state.db without any other intervening process
    // activity. paused must come back as true.
    {
        auto ch = ChannelInstance::build(cfg, channel_dir);
        ASSERT_TRUE(ch);
        EXPECT_TRUE(ch->wasPausedAtLastSave());
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

// fix17 — a corrupt state.db must not block channel build(). We
// pre-write garbage bytes into the file; build() detects the
// SQLITE_NOTADB error path, renames the file with a `.corrupt-<ns>`
// suffix and starts fresh. The channel still builds normally.
TEST(ChannelInstanceStateDb, CorruptDbGetsRenamedAndChannelStillBuilds) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_state_corrupt_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(94);
    cfg["name"] = "Corrupt";
    const auto channel_dir = root / "ch94-Corrupt";
    fs::create_directories(channel_dir);

    // Intentionally write non-SQLite bytes — sqlite3_open succeeds (the
    // header is checked lazily), the first prepare/exec returns
    // SQLITE_NOTADB, and openAndPrepare renames the file.
    {
        std::ofstream f(channel_dir / "state.db", std::ios::binary);
        f << "this is not a sqlite database";
    }

    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    // Original file replaced. A new file may or may not exist yet — the
    // saver only writes on first scheduleSave — but the corrupt copy
    // must have been moved aside.
    bool found_corrupt_copy = false;
    for (const auto& e : fs::directory_iterator(channel_dir)) {
        const auto name = e.path().filename().string();
        if (name.rfind("state.db.corrupt-", 0) == 0) {
            found_corrupt_copy = true;
            break;
        }
    }
    EXPECT_TRUE(found_corrupt_copy);
    EXPECT_FALSE(ch->wasPausedAtLastSave());   // empty snapshot → paused stays false

    std::error_code ec;
    fs::remove_all(root, ec);
}


// ─── Schedule hot-reload (fix9 step 5) ───────────────────────────────────────

namespace {

json validScheduleEntry(const std::string& id,
                        const std::string& start = "10:00",
                        const std::string& end   = "11:00") {
    return json{
        {"id", id},
        {"playlist", json::array({"a.mp4"})},
        {"recurrence", {
            {"kind", "daily"},
            {"start_time", start},
            {"end_time",   end},
        }},
    };
}

}  // namespace

TEST(ChannelInstanceScheduleHotReload, AcceptsValidScheduleOnPatch) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_sched_hr_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(60);
    cfg["name"] = "SchedHR";
    auto ch = ChannelInstance::build(cfg, root / "ch60-SchedHR");
    ASSERT_TRUE(ch);

    json patch = {{"schedule", json::array({validScheduleEntry("morning")})}};
    EXPECT_TRUE(ch->updateConfig(patch));

    // Persisted on disk so a restart sees the same schedule.
    std::ifstream f(root / "ch60-SchedHR" / "config.json");
    json on_disk = json::parse(f);
    ASSERT_TRUE(on_disk.contains("schedule"));
    ASSERT_TRUE(on_disk["schedule"].is_array());
    ASSERT_EQ(on_disk["schedule"].size(), 1u);
    EXPECT_EQ(on_disk["schedule"][0].value("id", std::string{}), "morning");

    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ChannelInstanceScheduleHotReload, RejectsBadScheduleAndPreservesOldOne) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_sched_bad_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(61);
    cfg["name"]     = "SchedBad";
    cfg["schedule"] = json::array({validScheduleEntry("orig")});
    auto ch = ChannelInstance::build(cfg, root / "ch61-SchedBad");
    ASSERT_TRUE(ch);

    // Establish a baseline on-disk config (build alone does not persist).
    ASSERT_TRUE(ch->updateConfig(json{{"bitrate", cfg["bitrate"].get<int>()}}));

    // Bad: missing required "kind" inside recurrence.
    json bad_entry = {
        {"id", "broken"},
        {"playlist", json::array({"x.mp4"})},
        {"recurrence", json::object()},
    };
    json bad_patch = {{"schedule", json::array({bad_entry})}};
    EXPECT_FALSE(ch->updateConfig(bad_patch));

    // On-disk schedule must still be the original — no half-applied state.
    std::ifstream f(root / "ch61-SchedBad" / "config.json");
    json on_disk = json::parse(f);
    ASSERT_TRUE(on_disk.contains("schedule"));
    ASSERT_EQ(on_disk["schedule"].size(), 1u);
    EXPECT_EQ(on_disk["schedule"][0].value("id", std::string{}), "orig");

    // Patch with non-array schedule is also rejected.
    EXPECT_FALSE(ch->updateConfig(json{{"schedule", "not-an-array"}}));

    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ChannelInstanceScheduleHotReload, NullSchedulePatchClearsEntries) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_sched_null_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(62);
    cfg["name"]     = "SchedNull";
    cfg["schedule"] = json::array({validScheduleEntry("a"), validScheduleEntry("b", "12:00", "13:00")});
    auto ch = ChannelInstance::build(cfg, root / "ch62-SchedNull");
    ASSERT_TRUE(ch);

    // RFC 7396 merge-patch convention: schedule=null deletes the key.
    EXPECT_TRUE(ch->updateConfig(json{{"schedule", nullptr}}));

    std::ifstream f(root / "ch62-SchedNull" / "config.json");
    json on_disk = json::parse(f);
    EXPECT_FALSE(on_disk.contains("schedule"));

    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ChannelInstanceScheduleHotReload, ReloadOnChannelWithoutInitialSchedule) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_sched_lazy_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(63);
    cfg["name"] = "SchedLazy";
    // No "schedule" in cfg — channel is built without entries.
    auto ch = ChannelInstance::build(cfg, root / "ch63-SchedLazy");
    ASSERT_TRUE(ch);

    json patch = {{"schedule", json::array({validScheduleEntry("late_added")})}};
    EXPECT_TRUE(ch->updateConfig(patch));

    std::ifstream f(root / "ch63-SchedLazy" / "config.json");
    json on_disk = json::parse(f);
    ASSERT_TRUE(on_disk.contains("schedule"));
    ASSERT_EQ(on_disk["schedule"].size(), 1u);
    EXPECT_EQ(on_disk["schedule"][0].value("id", std::string{}), "late_added");

    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ChannelInstance, UpdateConfigRefusesNameChange) {
    auto cfg = minimalCfg();
    cfg["name"] = "Original";
    auto ch = ChannelInstance::build(cfg);

    EXPECT_FALSE(ch->updateConfig(json{{"name", "Renamed"}}));
    // Mixed patch — even with valid fields, presence of name rejects whole patch.
    EXPECT_FALSE(ch->updateConfig(json{{"name", "X"}, {"bitrate", 2'000'000}}));

    const auto s = ch->status();
    EXPECT_EQ(s["name"], "Original");
}

// ─── Output routing (fix10 step 5) ───────────────────────────────────────────

namespace ChannelInstanceOutputRouting {

TEST(ChannelInstanceOutputRouting, LegacyConfigDefaultsToSrt) {
    // minimalCfg() has output.{port,latency_ms} with no "type" — must keep
    // working unchanged (this is what every fix7-era channel looks like).
    auto ch = ChannelInstance::build(minimalCfg());
    const auto s = ch->status();
    EXPECT_EQ(s["output"]["port"], 19000);
}

TEST(ChannelInstanceOutputRouting, AcceptsExplicitSrtType) {
    auto cfg = minimalCfg();
    cfg["output"] = {{"type", "srt"}, {"port", 19001}, {"latency_ms", 150}};
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    EXPECT_EQ(s["output"]["port"], 19001);
}

TEST(ChannelInstanceOutputRouting, AcceptsMulticastConfig) {
    auto cfg = minimalCfg();
    cfg["output"] = {
        {"type",    "multicast"},
        {"address", "239.0.0.1"},
        {"port",    6000},
        {"ttl",     8},
    };
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    EXPECT_EQ(s["output"]["type"],    "multicast");
    EXPECT_EQ(s["output"]["address"], "239.0.0.1");
    EXPECT_EQ(s["output"]["port"],    6000);
}

TEST(ChannelInstanceOutputRouting, FallsBackOnBadMulticast) {
    // Missing required address field — channel must still build (operator
    // gets a logged error) and fall back to SRT defaults.
    auto cfg = minimalCfg();
    cfg["output"] = {{"type", "multicast"}, {"port", 6000}};
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    // Output won't be a multicast driver — type field in cfg_ stays
    // "multicast" but instantiation will use SRT. Acceptable: operator must
    // fix config or downgrade explicitly.
}

TEST(ChannelInstanceOutputRouting, RejectsHotSwapOfOutputType) {
    auto ch = ChannelInstance::build(minimalCfg());
    EXPECT_FALSE(ch->updateConfig(json{{"output", {{"type", "multicast"}}}}));
    EXPECT_FALSE(ch->updateConfig(json{{"output", {{"address", "239.0.0.5"}}}}));
}

TEST(ChannelInstanceOutputRouting, AcceptsRtmpConfig) {
    auto cfg = minimalCfg();
    cfg["output"] = {
        {"type",                  "rtmp"},
        {"url",                   "rtmp://a.rtmp.example.com/live/key123"},
        {"reconnect_initial_ms",  500},
    };
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    EXPECT_EQ(s["output"]["type"], "rtmp");
}

TEST(ChannelInstanceOutputRouting, FallsBackOnBadRtmp) {
    // Missing required url — channel must still build (logged error) and
    // not crash. We don't assert on driver type here because the build
    // doesn't run play(); the routing decision is logged only.
    auto cfg = minimalCfg();
    cfg["output"] = {{"type", "rtmp"}};
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
}

TEST(ChannelInstanceOutputRouting, RejectsHotSwapOfRtmpUrl) {
    auto ch = ChannelInstance::build(minimalCfg());
    // url change is also a transport mutation — the new key/host needs a
    // fresh handshake which collides with the live encoder pipeline.
    EXPECT_FALSE(ch->updateConfig(
        json{{"output", {{"url", "rtmp://other.example/live/k"}}}}));
}

} // namespace ChannelInstanceOutputRouting

// ─── Multi-output (fix12 c3) ─────────────────────────────────────────────────

namespace ChannelInstanceMultiOutput {

TEST(ChannelInstanceMultiOutput, MigratesLegacyOutputToOutputsArray) {
    // Legacy single-output cfg: build must rewrite cfg_ in-memory so
    // outputs[] is the authoritative shape (id auto-assigned to "default").
    auto cfg = minimalCfg();
    cfg["output"] = {{"type", "srt"}, {"port", 19010}, {"latency_ms", 250}};
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    ASSERT_TRUE(s["outputs"].is_array());
    ASSERT_EQ(s["outputs"].size(), 1u);
    EXPECT_EQ(s["outputs"][0]["id"],   "default");
    EXPECT_EQ(s["outputs"][0]["port"], 19010);
    // Single-output legacy mirror still populated for backward compat.
    EXPECT_EQ(s["output"]["port"], 19010);
}

TEST(ChannelInstanceMultiOutput, AcceptsExplicitOutputsArray) {
    auto cfg = minimalCfg();
    cfg.erase("output");
    cfg["outputs"] = json::array({
        json{ {"id", "srt-headend"}, {"type", "srt"},
              {"port", 19020}, {"latency_ms", 200} },
        json{ {"id", "mc-cable"}, {"type", "multicast"},
              {"address", "239.0.0.7"}, {"port", 6010}, {"ttl", 4} },
    });
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    ASSERT_TRUE(s["outputs"].is_array());
    ASSERT_EQ(s["outputs"].size(), 2u);
    EXPECT_EQ(s["outputs"][0]["id"], "srt-headend");
    EXPECT_EQ(s["outputs"][1]["id"], "mc-cable");
    // Multi-output: legacy mirror is intentionally absent.
    EXPECT_FALSE(s.contains("output"));
}

TEST(ChannelInstanceMultiOutput, SkipsDuplicateIdsAndKeepsRest) {
    auto cfg = minimalCfg();
    cfg.erase("output");
    cfg["outputs"] = json::array({
        json{ {"id", "a"}, {"type", "srt"}, {"port", 19030} },
        json{ {"id", "a"}, {"type", "srt"}, {"port", 19031} },  // duplicate id
        json{ {"id", "b"}, {"type", "srt"}, {"port", 19032} },
    });
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    ASSERT_EQ(s["outputs"].size(), 2u);
    EXPECT_EQ(s["outputs"][0]["id"], "a");
    EXPECT_EQ(s["outputs"][1]["id"], "b");
    EXPECT_EQ(s["outputs"][0]["port"], 19030);
}

TEST(ChannelInstanceMultiOutput, SkipsEntryWithMissingId) {
    auto cfg = minimalCfg();
    cfg.erase("output");
    cfg["outputs"] = json::array({
        json{ {"type", "srt"}, {"port", 19040} },             // no id
        json{ {"id", "kept"}, {"type", "srt"}, {"port", 19041} },
    });
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    const auto s = ch->status();
    ASSERT_EQ(s["outputs"].size(), 1u);
    EXPECT_EQ(s["outputs"][0]["id"], "kept");
}

TEST(ChannelInstanceMultiOutput, RejectsOutputsPatchEntirely) {
    auto cfg = minimalCfg();
    cfg.erase("output");
    cfg["outputs"] = json::array({
        json{ {"id", "main"}, {"type", "srt"}, {"port", 19050} },
    });
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    // Bulk outputs[] rewrites must go through dedicated REST endpoints
    // (POST/DELETE/PATCH /channels/{id}/outputs/{id}), not generic PATCH.
    EXPECT_FALSE(ch->updateConfig(json{{"outputs", json::array()}}));
    EXPECT_FALSE(ch->updateConfig(json{
        {"outputs", json::array({json{{"id","new"},{"type","srt"},{"port",19051}}})}
    }));
}

} // namespace ChannelInstanceMultiOutput

// ─── Hot-add output (fix12 c4) ──────────────────────────────────────────────

namespace ChannelInstanceAddOutput {

using OR = ChannelInstance::OutputResult;

TEST(ChannelInstanceAddOutput, AddSrtOutputWhileStopped) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    ASSERT_EQ(ch->status()["outputs"].size(), 1u);

    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "srt-extra"}, {"type", "srt"},
        {"port", 19101}, {"latency_ms", 200}
    }));
    const auto outs = ch->outputsJson();
    ASSERT_EQ(outs.size(), 2u);
    EXPECT_EQ(outs[1]["id"],   "srt-extra");
    EXPECT_EQ(outs[1]["port"], 19101);
}

TEST(ChannelInstanceAddOutput, AddMulticastOutputWhileStopped) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "mc"}, {"type", "multicast"},
        {"address", "239.0.1.2"}, {"port", 6100}, {"ttl", 4}
    }));
    EXPECT_EQ(ch->outputsJson().size(), 2u);
}

TEST(ChannelInstanceAddOutput, RejectsDuplicateId) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    // After build, outputs[0].id == "default" (from migration).
    EXPECT_EQ(OR::DuplicateId, ch->addOutput(json{
        {"id", "default"}, {"type", "srt"}, {"port", 19102}
    }));
}

TEST(ChannelInstanceAddOutput, RejectsMissingId) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::BadJson, ch->addOutput(json{
        {"type", "srt"}, {"port", 19103}
    }));
}

TEST(ChannelInstanceAddOutput, RejectsUnknownType) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::BadJson, ch->addOutput(json{
        {"id", "x"}, {"type", "carrier-pigeon"}
    }));
}

TEST(ChannelInstanceAddOutput, RejectsBadMulticastConfig) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    // Missing address — multicast parser must throw, mapped to BadJson.
    EXPECT_EQ(OR::BadJson, ch->addOutput(json{
        {"id", "mc-bad"}, {"type", "multicast"}, {"port", 6101}
    }));
}

TEST(ChannelInstanceAddOutput, RejectsNonObjectBody) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::BadJson, ch->addOutput(json::array()));
    EXPECT_EQ(OR::BadJson, ch->addOutput(json("string")));
}

TEST(ChannelInstanceAddOutput, AddPersistsConfigJson) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_addout_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(77);
    cfg["name"] = "AddOut";
    const auto channel_dir = root / "ch77-AddOut";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "yt"}, {"type", "srt"}, {"port", 19200}
    }));

    const auto cfg_path = channel_dir / "config.json";
    ASSERT_TRUE(fs::exists(cfg_path));
    json on_disk = json::parse(std::ifstream(cfg_path));
    ASSERT_TRUE(on_disk.contains("outputs"));
    ASSERT_TRUE(on_disk["outputs"].is_array());
    EXPECT_EQ(on_disk["outputs"].size(), 2u);
    EXPECT_EQ(on_disk["outputs"][1]["id"],   "yt");
    EXPECT_EQ(on_disk["outputs"][1]["port"], 19200);
    // Legacy "output" field has been migrated away on persist.
    EXPECT_FALSE(on_disk.contains("output"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

} // namespace ChannelInstanceAddOutput

namespace ChannelInstanceRemoveOutput {

using OR = ChannelInstance::OutputResult;

TEST(ChannelInstanceRemoveOutput, RemoveExistingWhileStopped) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "extra"}, {"type", "srt"}, {"port", 19111}
    }));
    ASSERT_EQ(ch->outputsJson().size(), 2u);

    ASSERT_EQ(OR::Ok, ch->removeOutput("extra"));
    const auto outs = ch->outputsJson();
    ASSERT_EQ(outs.size(), 1u);
    EXPECT_EQ(outs[0]["id"], "default");
}

TEST(ChannelInstanceRemoveOutput, RemoveLastEntryYieldsEmptyArray) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->removeOutput("default"));
    EXPECT_TRUE(ch->outputsJson().empty());
}

TEST(ChannelInstanceRemoveOutput, RemoveUnknownIdReturnsNotFound) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::NotFound, ch->removeOutput("ghost"));
}

TEST(ChannelInstanceRemoveOutput, RemoveEmptyIdReturnsNotFound) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::NotFound, ch->removeOutput(""));
}

TEST(ChannelInstanceRemoveOutput, RemovePersistsConfigJson) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_rmout_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(78);
    cfg["name"] = "RmOut";
    const auto channel_dir = root / "ch78-RmOut";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "extra"}, {"type", "srt"}, {"port", 19222}
    }));
    ASSERT_EQ(OR::Ok, ch->removeOutput("extra"));

    const auto cfg_path = channel_dir / "config.json";
    ASSERT_TRUE(fs::exists(cfg_path));
    json on_disk = json::parse(std::ifstream(cfg_path));
    ASSERT_TRUE(on_disk["outputs"].is_array());
    EXPECT_EQ(on_disk["outputs"].size(), 1u);
    EXPECT_EQ(on_disk["outputs"][0]["id"], "default");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ChannelInstanceRemoveOutput, RemoveAfterAddIsRoundTrip) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "tmp"}, {"type", "multicast"},
        {"address", "239.1.2.3"}, {"port", 6200}, {"ttl", 2}
    }));
    ASSERT_EQ(OR::Ok, ch->removeOutput("tmp"));
    // Adding again with same id must succeed — uniqueness window cleared.
    EXPECT_EQ(OR::Ok, ch->addOutput(json{
        {"id", "tmp"}, {"type", "multicast"},
        {"address", "239.1.2.3"}, {"port", 6201}, {"ttl", 2}
    }));
}

} // namespace ChannelInstanceRemoveOutput

namespace ChannelInstancePatchOutput {

using OR = ChannelInstance::OutputResult;

TEST(ChannelInstancePatchOutput, PatchSrtPortWhileStopped) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    // After build, outputs[0] is "default" (legacy migration).
    ASSERT_EQ(OR::Ok, ch->patchOutput("default", json{
        {"type", "srt"}, {"port", 19301}, {"latency_ms", 250}
    }));
    const auto outs = ch->outputsJson();
    ASSERT_EQ(outs.size(), 1u);
    EXPECT_EQ(outs[0]["id"],         "default");
    EXPECT_EQ(outs[0]["type"],       "srt");
    EXPECT_EQ(outs[0]["port"],       19301);
    EXPECT_EQ(outs[0]["latency_ms"], 250);
}

TEST(ChannelInstancePatchOutput, PatchAcceptsBodyIdMatchingUrl) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::Ok, ch->patchOutput("default", json{
        {"id", "default"}, {"type", "srt"}, {"port", 19302}
    }));
}

TEST(ChannelInstancePatchOutput, PatchRejectsBodyIdMismatch) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::BadJson, ch->patchOutput("default", json{
        {"id", "other"}, {"type", "srt"}, {"port", 19303}
    }));
}

TEST(ChannelInstancePatchOutput, PatchUnknownIdReturnsNotFound) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::NotFound, ch->patchOutput("ghost", json{
        {"type", "srt"}, {"port", 19304}
    }));
}

TEST(ChannelInstancePatchOutput, PatchEmptyIdReturnsNotFound) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::NotFound, ch->patchOutput("", json{
        {"type", "srt"}, {"port", 19305}
    }));
}

TEST(ChannelInstancePatchOutput, PatchNonObjectBodyIsBadJson) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_EQ(OR::BadJson, ch->patchOutput("default", json::array()));
}

TEST(ChannelInstancePatchOutput, PatchChangesType) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_EQ(OR::Ok, ch->patchOutput("default", json{
        {"type", "multicast"},
        {"address", "239.0.5.6"}, {"port", 6300}, {"ttl", 3}
    }));
    const auto outs = ch->outputsJson();
    ASSERT_EQ(outs.size(), 1u);
    EXPECT_EQ(outs[0]["type"],    "multicast");
    EXPECT_EQ(outs[0]["address"], "239.0.5.6");
}

TEST(ChannelInstancePatchOutput, PatchWithBadCfgLeavesChannelWithoutOutput) {
    // Patch validates the new body via the same parser as POST. A bad
    // body fails with BadJson AFTER the old entry is already removed —
    // by design the channel is left without that output (caller must
    // re-POST to recover).
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_EQ(OR::BadJson, ch->patchOutput("default", json{
        // missing "type"
        {"port", 19306}
    }));
    EXPECT_TRUE(ch->outputsJson().empty());
}

TEST(ChannelInstancePatchOutput, PatchPersistsConfigJson) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ch_patchout_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    auto cfg = minimalCfg(79);
    cfg["name"] = "PatchOut";
    const auto channel_dir = root / "ch79-PatchOut";
    auto ch = ChannelInstance::build(cfg, channel_dir);
    ASSERT_TRUE(ch);
    ASSERT_EQ(OR::Ok, ch->patchOutput("default", json{
        {"type", "srt"}, {"port", 19400}, {"latency_ms", 333}
    }));

    const auto cfg_path = channel_dir / "config.json";
    ASSERT_TRUE(fs::exists(cfg_path));
    json on_disk = json::parse(std::ifstream(cfg_path));
    ASSERT_TRUE(on_disk["outputs"].is_array());
    ASSERT_EQ(on_disk["outputs"].size(), 1u);
    EXPECT_EQ(on_disk["outputs"][0]["id"],         "default");
    EXPECT_EQ(on_disk["outputs"][0]["port"],       19400);
    EXPECT_EQ(on_disk["outputs"][0]["latency_ms"], 333);

    std::error_code ec;
    fs::remove_all(root, ec);
}

} // namespace ChannelInstancePatchOutput

namespace ChannelInstanceOutputStatus {

TEST(ChannelInstanceOutputStatus, StoppedChannelReturnsNull) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    EXPECT_TRUE(ch->outputStatusJson("default").is_null());
}

TEST(ChannelInstanceOutputStatus, UnknownIdReturnsNull) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    EXPECT_TRUE(ch->outputStatusJson("ghost").is_null());
}

// fix13 c8 — REST live-status endpoint
TEST(ChannelInstanceLiveStatus, EmptyWhenNoLiveClips) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    ASSERT_TRUE(ch);
    auto j = ch->liveStatusJson();
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

TEST(ChannelInstanceLiveStatus, EmptyForFileOnlyPlaylist) {
    auto cfg = minimalCfg();
    auto ch = ChannelInstance::build(cfg);
    json items = json::array({
        item("test_media/photo1.png", 3.0),
        item("test_media/photo2.png", 2.0),
    });
    ASSERT_EQ(ch->replacePlaylist(items), ChannelInstance::PlaylistResult::Ok);
    auto j = ch->liveStatusJson();
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

// ─── fix13 c9 — ChannelInstance live-clip integration ───────────────────────

// Helper: build a syntactically-valid multicast live entry. The address is
// a non-routable doc-block IP so prepare() never actually joins anything; the
// LiveClip stays in Idle until its scheduled-start window arrives, which is
// after the test ends.
namespace {
json liveMulticastItem(const std::string& id,
                       const std::string& addr = "233.252.0.10",
                       int port = 49500) {
    return json{
        {"type", "live"},
        {"id",   id},
        {"input", {
            {"type",     "multicast"},
            {"address",  addr},
            {"port",     port},
        }},
        {"duration_sec",     30.0},
        {"warm_up_sec",      5.0},
        {"loss_threshold_ms", 2000.0},
        {"fallback_on_loss", "black"},
    };
}
}  // namespace

TEST(ChannelInstanceLive, ReplacePlaylistWithLiveEntryBuildsLiveClip) {
    auto ch = ChannelInstance::build(minimalCfg());
    json items = json::array({ liveMulticastItem("cam-1") });
    ASSERT_EQ(ch->replacePlaylist(items), PR::Ok);

    auto j = ch->liveStatusJson();
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["id"], "cam-1");
    EXPECT_EQ(j[0]["state"], "Idle");
    EXPECT_EQ(j[0]["playlist_index"], 0);
    EXPECT_EQ(j[0]["duration_ns"], 30'000'000'000ULL);
    EXPECT_EQ(j[0]["warm_up_ns"],   5'000'000'000ULL);
}

TEST(ChannelInstanceLive, MixedFileAndLivePlaylistKeepsBothInOrder) {
    auto ch = ChannelInstance::build(minimalCfg());
    json items = json::array({
        item("test_media/photo1.png", 3.0),
        liveMulticastItem("cam-2", "233.252.0.11", 49510),
        item("test_media/photo2.png", 2.0),
    });
    ASSERT_EQ(ch->replacePlaylist(items), PR::Ok);

    // playlistJson surfaces all three.
    auto pl = ch->playlistJson();
    ASSERT_EQ(pl.size(), 3u);

    auto live = ch->liveStatusJson();
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0]["id"], "cam-2");
    EXPECT_EQ(live[0]["playlist_index"], 1);
}

TEST(ChannelInstanceLive, ReplacePlaylistRejectsUnknownLiveType) {
    auto ch = ChannelInstance::build(minimalCfg());
    json bad = liveMulticastItem("cam-bad");
    bad["input"]["type"] = "ftp";  // unknown transport
    json items = json::array({ bad });
    EXPECT_EQ(ch->replacePlaylist(items), PR::ItemBuildFailed);
    EXPECT_TRUE(ch->liveStatusJson().empty());
}

TEST(ChannelInstanceLive, ReplacePlaylistRejectsLiveWithoutId) {
    auto ch = ChannelInstance::build(minimalCfg());
    json bad = liveMulticastItem("placeholder");
    bad.erase("id");
    json items = json::array({ bad });
    EXPECT_EQ(ch->replacePlaylist(items), PR::ItemBuildFailed);
}

TEST(ChannelInstanceLive, AppendPlaylistAcceptsLiveEntry) {
    auto ch = ChannelInstance::build(minimalCfg());
    json initial = json::array({ item("test_media/photo1.png", 3.0) });
    ASSERT_EQ(ch->replacePlaylist(initial), PR::Ok);

    int first_idx = -1;
    json appended = json::array({ liveMulticastItem("cam-3") });
    ASSERT_EQ(ch->appendPlaylist(appended, &first_idx), PR::Ok);
    EXPECT_EQ(first_idx, 1);

    auto live = ch->liveStatusJson();
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0]["id"], "cam-3");
    EXPECT_EQ(live[0]["playlist_index"], 1);
}

TEST(ChannelInstanceLive, ReplacePlaylistTwiceSwapsLiveClip) {
    auto ch = ChannelInstance::build(minimalCfg());
    json first  = json::array({ liveMulticastItem("cam-A") });
    json second = json::array({ liveMulticastItem("cam-B", "233.252.0.20", 49520) });

    ASSERT_EQ(ch->replacePlaylist(first), PR::Ok);
    EXPECT_EQ(ch->liveStatusJson()[0]["id"], "cam-A");

    ASSERT_EQ(ch->replacePlaylist(second), PR::Ok);
    auto live = ch->liveStatusJson();
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0]["id"], "cam-B");
}

} // namespace ChannelInstanceOutputStatus
