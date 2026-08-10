#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "metrics/ProfileSampler.h"

using nlohmann::json;
using R = ChannelManager::Result;

namespace {

json minimalCfg(int id = 0) {
    const int port = 19500 + (id > 0 ? id : 0);
    json c{
        {"name", "test"},
        {"resolution", "320x240"},
        {"fps", 25},
        {"bitrate", 1'000'000},
        {"preset", "ultrafast"},
        {"output", {{"port", port}, {"latency_ms", 200}}},
    };
    if (id != 0) c["id"] = id;
    return c;
}

}  // namespace

TEST(ChannelManager, CreateAndList) {
    ChannelManager mgr;
    int id = 0;
    EXPECT_EQ(mgr.create(minimalCfg(7), &id), R::Ok);
    EXPECT_EQ(id, 7);
    EXPECT_EQ(mgr.size(), 1u);

    auto list = mgr.listJson();
    ASSERT_TRUE(list.is_array());
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["id"], 7);
    EXPECT_EQ(list[0]["state"], "stopped");
}

TEST(ChannelManager, CreateRejectsDuplicate) {
    ChannelManager mgr;
    EXPECT_EQ(mgr.create(minimalCfg(7)), R::Ok);
    EXPECT_EQ(mgr.create(minimalCfg(7)), R::AlreadyExists);
}

TEST(ChannelManager, AutoIncrementId) {
    ChannelManager mgr;
    int a = 0, b = 0, c = 0;
    EXPECT_EQ(mgr.create(minimalCfg(), &a), R::Ok);
    EXPECT_EQ(mgr.create(minimalCfg(), &b), R::Ok);
    EXPECT_EQ(mgr.create(minimalCfg(5), &c), R::Ok);
    int d = 0;
    EXPECT_EQ(mgr.create(minimalCfg(), &d), R::Ok);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(c, 5);
    EXPECT_EQ(d, 6);  // max(1,2,5)+1
}

TEST(ChannelManager, RemoveExistingAndMissing) {
    ChannelManager mgr;
    int id = 0;
    mgr.create(minimalCfg(7), &id);
    EXPECT_EQ(mgr.remove(7), R::Ok);
    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_EQ(mgr.remove(7), R::NotFound);
}

TEST(ChannelManager, NextNotFound) {
    ChannelManager mgr;
    EXPECT_EQ(mgr.next(99), R::NotFound);
}

TEST(ChannelManager, StopOnStoppedReturnsAlreadyStopped) {
    ChannelManager mgr;
    mgr.create(minimalCfg(1));
    EXPECT_EQ(mgr.stop(1), R::AlreadyStopped);
}

TEST(ChannelManager, ProfilerForUnknownIdIsNull) {
    ChannelManager mgr;
    EXPECT_EQ(mgr.profilerFor(99), nullptr);
}

TEST(ChannelManager, ProfilerForStoppedChannelIsNull) {
    ChannelManager mgr;
    mgr.create(minimalCfg(1));
    // No play() — RenderLoop has not been built yet, so no profiler.
    EXPECT_EQ(mgr.profilerFor(1), nullptr);
}

TEST(ChannelManager, SetProfileSamplerNullDetachesCleanly) {
    ChannelManager mgr;
    liveqx::profiler::ProfileSampler sampler(std::chrono::milliseconds(1000));
    mgr.create(minimalCfg(1));
    // Stopped channels have no profiler, so setProfileSampler must not blow up.
    mgr.setProfileSampler(&sampler);
    mgr.setProfileSampler(nullptr);
    EXPECT_EQ(sampler.registeredCount(), 0u);
}

TEST(ChannelManager, RemoveDoesNotTouchSamplerWhenStopped) {
    ChannelManager mgr;
    liveqx::profiler::ProfileSampler sampler(std::chrono::milliseconds(1000));
    mgr.setProfileSampler(&sampler);
    mgr.create(minimalCfg(1));
    EXPECT_EQ(mgr.remove(1), R::Ok);
    EXPECT_EQ(sampler.registeredCount(), 0u);
}

TEST(ChannelManager, UpdateConfigPath) {
    ChannelManager mgr;
    mgr.create(minimalCfg(1));
    EXPECT_EQ(mgr.updateConfig(1, json{{"bitrate", 2'000'000}}), R::Ok);
    EXPECT_EQ(mgr.updateConfig(1, json{{"resolution", "640x480"}}), R::BadPatch);
    EXPECT_EQ(mgr.updateConfig(99, json{{"bitrate", 1}}), R::NotFound);

    EXPECT_EQ(mgr.statusJson(1)["bitrate"], 2'000'000);
}

TEST(ChannelManager, HealthAggregation) {
    ChannelManager mgr;
    mgr.create(minimalCfg(1));
    mgr.create(minimalCfg(2));
    auto h = mgr.healthJson();
    ASSERT_EQ(h["channels"].size(), 2u);
    EXPECT_EQ(h["overall"], "running");  // fresh ChannelHealth defaults
}

TEST(ChannelManager, StatusForMissingIsNull) {
    ChannelManager mgr;
    EXPECT_TRUE(mgr.statusJson(42).is_null());
}

// fix13 c8 — live-status passthrough.
TEST(ChannelManager, LiveStatusUnknownIdIsNull) {
    ChannelManager mgr;
    EXPECT_TRUE(mgr.liveStatusJson(42).is_null());
}

TEST(ChannelManager, LiveStatusKnownChannelIsArray) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfg(11)), R::Ok);
    auto j = mgr.liveStatusJson(11);
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

// ── fix7: loadFromRoot ────────────────────────────────────────────────────────

#include <filesystem>
#include <fstream>
#include "core/ChannelInstance.h"

namespace {
std::filesystem::path makeRoot(const std::string& tag) {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path() /
        ("ch_mgr_load_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void writeChannelDir(const std::filesystem::path& root,
                     int id, const std::string& name,
                     const std::string& contents) {
    namespace fs = std::filesystem;
    const auto dir = root / ("ch" + std::to_string(id) + "-" + name);
    fs::create_directories(dir);
    std::ofstream f(dir / "config.json");
    f << contents;
}
}  // namespace

TEST(ChannelManagerLoad, EmptyRootLoadsZero) {
    auto root = makeRoot("empty");
    ChannelManager mgr(nullptr, root);
    EXPECT_EQ(mgr.loadFromRoot(), 0u);
    EXPECT_EQ(mgr.size(), 0u);
    std::filesystem::remove_all(root);
}

TEST(ChannelManagerLoad, ValidChannelsBoot) {
    auto root = makeRoot("valid");
    auto cfgA = json{{"id", 1}, {"name", "Alpha"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19510}, {"latency_ms", 200}}}};
    auto cfgB = json{{"id", 2}, {"name", "Beta"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19511}, {"latency_ms", 200}}}};
    writeChannelDir(root, 1, "Alpha", cfgA.dump(2));
    writeChannelDir(root, 2, "Beta",  cfgB.dump(2));

    ChannelManager mgr(nullptr, root);
    EXPECT_EQ(mgr.loadFromRoot(), 2u);
    EXPECT_EQ(mgr.size(), 2u);
    EXPECT_EQ(mgr.statusJson(1)["name"], "Alpha");
    EXPECT_EQ(mgr.statusJson(2)["name"], "Beta");
    EXPECT_EQ(mgr.statusJson(1)["state"], "stopped");

    std::filesystem::remove_all(root);
}

TEST(ChannelManagerLoad, BrokenConfigSkipped) {
    auto root = makeRoot("broken");
    writeChannelDir(root, 1, "Bad",  "{ this is not json }");
    auto cfgB = json{{"id", 2}, {"name", "Good"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19520}, {"latency_ms", 200}}}};
    writeChannelDir(root, 2, "Good", cfgB.dump(2));

    ChannelManager mgr(nullptr, root);
    EXPECT_EQ(mgr.loadFromRoot(), 1u);   // broken skipped, good loaded
    EXPECT_TRUE(mgr.statusJson(1).is_null());
    EXPECT_FALSE(mgr.statusJson(2).is_null());

    std::filesystem::remove_all(root);
}

TEST(ChannelManagerLoad, FolderWithoutConfigJsonSkipped) {
    auto root = makeRoot("noconfig");
    std::filesystem::create_directories(root / "ch9-Lonely");

    ChannelManager mgr(nullptr, root);
    EXPECT_EQ(mgr.loadFromRoot(), 0u);

    std::filesystem::remove_all(root);
}

TEST(ChannelManagerCRUD, CreateMaterialisesConfigJson) {
    auto root = makeRoot("crud_create");
    ChannelManager mgr(nullptr, root);

    auto cfg = json{{"id", 11}, {"name", "Created"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19530}, {"latency_ms", 200}}}};
    int id = 0;
    EXPECT_EQ(mgr.create(cfg, &id), R::Ok);
    EXPECT_EQ(id, 11);

    const auto cfg_path = root / "ch11-Created" / "config.json";
    ASSERT_TRUE(std::filesystem::exists(cfg_path));
    auto on_disk = json::parse(std::ifstream(cfg_path));
    EXPECT_EQ(on_disk["id"], 11);
    EXPECT_EQ(on_disk["name"], "Created");

    std::filesystem::remove_all(root);
}

TEST(ChannelManagerCRUD, RemoveDeletesChannelDir) {
    auto root = makeRoot("crud_remove");
    ChannelManager mgr(nullptr, root);
    auto cfg = json{{"id", 12}, {"name", "Doomed"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19531}, {"latency_ms", 200}}}};
    int id = 0;
    ASSERT_EQ(mgr.create(cfg, &id), R::Ok);
    const auto dir = root / "ch12-Doomed";
    ASSERT_TRUE(std::filesystem::is_directory(dir));

    EXPECT_EQ(mgr.remove(12), R::Ok);
    EXPECT_FALSE(std::filesystem::exists(dir));
    EXPECT_TRUE(std::filesystem::is_directory(root));   // root preserved

    std::filesystem::remove_all(root);
}

TEST(ChannelManagerCRUD, LegacyModeNoDirSideEffects) {
    ChannelManager mgr;   // empty channel_root
    auto cfg = json{{"id", 13}, {"name", "Legacy"},
        {"resolution", "320x240"}, {"fps", 25},
        {"bitrate", 1'000'000}, {"preset", "ultrafast"},
        {"output", {{"port", 19532}, {"latency_ms", 200}}}};
    EXPECT_EQ(mgr.create(cfg), R::Ok);
    EXPECT_EQ(mgr.remove(13), R::Ok);  // no FS to touch — must still succeed
}

// ─── Playback log endpoints (fix8 step 9) ───────────────────────────────────

TEST(ChannelManagerPlaybackLog, UnknownIdReturnsNull) {
    ChannelManager mgr;
    EXPECT_TRUE(mgr.playbackLogStatusJson(999).is_null());
    EXPECT_TRUE(mgr.queryPlaybackLog(999, std::nullopt, std::nullopt,
                                     std::nullopt, 100, 0).is_null());
}

TEST(ChannelManagerPlaybackLog, ChannelWithoutPlaybackLogConfigReportsNoneOnPlay) {
    auto root = makeRoot("pblog_none");
    ChannelManager mgr(nullptr, root);
    int id = 0;
    auto cfg = minimalCfg(21);
    ASSERT_EQ(mgr.create(cfg, &id), R::Ok);

    // Sink is resolved on play(); without a play() the sink may still be null.
    ASSERT_EQ(mgr.play(21), R::Ok);
    auto st = mgr.playbackLogStatusJson(21);
    ASSERT_FALSE(st.is_null());
    EXPECT_EQ(st["sink_type"], "none");

    auto q = mgr.queryPlaybackLog(21, std::nullopt, std::nullopt,
                                  std::nullopt, 100, 0);
    ASSERT_FALSE(q.is_null());
    EXPECT_TRUE(q["events"].is_array());
    EXPECT_EQ(q["events"].size(), 0u);

    mgr.stop(21);
    std::filesystem::remove_all(root);
}

TEST(ChannelManagerPlaybackLog, FileSinkResolvedOnPlay) {
    auto root = makeRoot("pblog_file");
    ChannelManager mgr(nullptr, root);
    auto cfg = minimalCfg(22);
    cfg["playback_log"] = json{{"sink", "file"}};
    int id = 0;
    ASSERT_EQ(mgr.create(cfg, &id), R::Ok);
    ASSERT_EQ(mgr.play(22), R::Ok);

    auto st = mgr.playbackLogStatusJson(22);
    ASSERT_FALSE(st.is_null());
    EXPECT_EQ(st["sink_type"], "file");
    EXPECT_TRUE(st.contains("queue_depth"));
    EXPECT_TRUE(st.contains("dropped_count"));

    mgr.stop(22);
    std::filesystem::remove_all(root);
}

// ─── Schedule REST proxies (fix9 step 6) ─────────────────────────────────────

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

TEST(ChannelManagerSchedule, UnknownIdReturnsNull) {
    ChannelManager mgr;
    EXPECT_TRUE(mgr.scheduleJson(999).is_null());
    EXPECT_TRUE(mgr.scheduleActiveJson(999).is_null());
    EXPECT_EQ(mgr.replaceSchedule(999, json::array()), R::NotFound);
}

TEST(ChannelManagerSchedule, EmptyByDefault) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfg(31)), R::Ok);
    auto j = mgr.scheduleJson(31);
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);

    auto a = mgr.scheduleActiveJson(31);
    ASSERT_FALSE(a.is_null());
    EXPECT_EQ(a["mode"], "regular");
    EXPECT_TRUE(a["entry_id"].is_null());
    EXPECT_TRUE(a["window_end_ns"].is_null());
}

TEST(ChannelManagerSchedule, PutReplacesEntries) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfg(32)), R::Ok);

    json items = json::array({
        validScheduleEntry("morning"),
        validScheduleEntry("afternoon", "13:00", "14:00"),
    });
    EXPECT_EQ(mgr.replaceSchedule(32, items), R::Ok);

    auto j = mgr.scheduleJson(32);
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 2u);

    // PUT with empty array clears the schedule.
    EXPECT_EQ(mgr.replaceSchedule(32, json::array()), R::Ok);
    EXPECT_EQ(mgr.scheduleJson(32).size(), 0u);
}

TEST(ChannelManagerSchedule, UpcomingShape) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfg(34)), R::Ok);

    // Default: empty schedule → empty upcoming array.
    auto u0 = mgr.scheduleUpcomingJson(34, 3600);
    ASSERT_TRUE(u0.is_array());
    EXPECT_EQ(u0.size(), 0u);

    // Replace with one daily entry. We don't know wall-clock relative to
    // "10:00", so we ask for a 24h horizon — guaranteed to include the next
    // activation. The exact time fields are validated in test_scheduler.cpp;
    // here we only check the shape.
    EXPECT_EQ(mgr.replaceSchedule(34, json::array({validScheduleEntry("d")})), R::Ok);
    auto u1 = mgr.scheduleUpcomingJson(34, 24 * 3600);
    ASSERT_TRUE(u1.is_array());
    ASSERT_EQ(u1.size(), 1u);
    EXPECT_EQ(u1[0]["entry_id"], "d");
    EXPECT_TRUE(u1[0]["starts_at"].is_number_integer());
    EXPECT_TRUE(u1[0]["ends_at"].is_number_integer());

    // within_sec=0 → empty (clamp behaviour).
    auto u2 = mgr.scheduleUpcomingJson(34, 0);
    ASSERT_TRUE(u2.is_array());
    EXPECT_EQ(u2.size(), 0u);
}

TEST(ChannelManagerSchedule, UpcomingUnknownIdNull) {
    ChannelManager mgr;
    EXPECT_TRUE(mgr.scheduleUpcomingJson(999, 3600).is_null());
}

TEST(ChannelManagerSchedule, PutBadScheduleIsBadPatch) {
    ChannelManager mgr;
    ASSERT_EQ(mgr.create(minimalCfg(33)), R::Ok);
    ASSERT_EQ(mgr.replaceSchedule(33, json::array({validScheduleEntry("ok")})), R::Ok);

    // Missing recurrence.kind — parser rejects → BadPatch
    json bad_entry = {
        {"id", "x"},
        {"playlist", json::array({"x.mp4"})},
        {"recurrence", json::object()},
    };
    EXPECT_EQ(mgr.replaceSchedule(33, json::array({bad_entry})), R::BadPatch);

    // Pre-existing schedule preserved.
    EXPECT_EQ(mgr.scheduleJson(33).size(), 1u);
}

TEST(ChannelManagerPlaybackLog, PatchPlaybackLogWhileRunningRejected) {
    auto root = makeRoot("pblog_patch");
    ChannelManager mgr(nullptr, root);
    auto cfg = minimalCfg(23);
    cfg["playback_log"] = json{{"sink", "none"}};
    int id = 0;
    ASSERT_EQ(mgr.create(cfg, &id), R::Ok);
    ASSERT_EQ(mgr.play(23), R::Ok);

    // While running, switching sink type would require restarting the writer
    // thread mid-event — must be refused.
    json patch = {{"playback_log", {{"sink", "file"}}}};
    EXPECT_EQ(mgr.updateConfig(23, patch), R::BadPatch);

    // After stop the same patch is accepted (rebuild on next play).
    mgr.stop(23);
    EXPECT_EQ(mgr.updateConfig(23, patch), R::Ok);

    std::filesystem::remove_all(root);
}

// ─── fix21: sd_notify status formatter ────────────────────────────────────────

TEST(ChannelManagerStatusFormatter, AllRunning) {
    ChannelManager::StatusSnapshot s{};
    s.channels = 2; s.running = 2;
    s.outputs_ok = 4; s.outputs_total = 4;
    EXPECT_EQ(ChannelManager::formatStatusLine(s), "ch=2 run=2 outputs=4/4");
}

TEST(ChannelManagerStatusFormatter, OmitsZeroDegradedAndFailed) {
    ChannelManager::StatusSnapshot s{};
    s.channels = 1; s.running = 1;
    s.outputs_ok = 1; s.outputs_total = 1;
    EXPECT_EQ(ChannelManager::formatStatusLine(s), "ch=1 run=1 outputs=1/1");
}

TEST(ChannelManagerStatusFormatter, IncludesDegraded) {
    ChannelManager::StatusSnapshot s{};
    s.channels = 3; s.running = 2; s.degraded = 1;
    s.outputs_ok = 3; s.outputs_total = 4;
    EXPECT_EQ(ChannelManager::formatStatusLine(s),
              "ch=3 run=2 deg=1 outputs=3/4");
}

TEST(ChannelManagerStatusFormatter, IncludesFailed) {
    ChannelManager::StatusSnapshot s{};
    s.channels = 2; s.running = 1; s.failed = 1;
    s.outputs_ok = 1; s.outputs_total = 2;
    EXPECT_EQ(ChannelManager::formatStatusLine(s),
              "ch=2 run=1 fail=1 outputs=1/2");
}

TEST(ChannelManagerStatusFormatter, IncludesBothDegradedAndFailed) {
    ChannelManager::StatusSnapshot s{};
    s.channels = 4; s.running = 1; s.degraded = 2; s.failed = 1;
    s.outputs_ok = 2; s.outputs_total = 5;
    EXPECT_EQ(ChannelManager::formatStatusLine(s),
              "ch=4 run=1 deg=2 fail=1 outputs=2/5");
}

TEST(ChannelManagerStatusFormatter, EmptyManagerSnapshot) {
    ChannelManager mgr;
    const auto s = mgr.snapshotForStatus();
    EXPECT_EQ(s.channels, 0);
    EXPECT_EQ(s.running, 0);
    EXPECT_EQ(s.outputs_total, 0);
    EXPECT_EQ(ChannelManager::formatStatusLine(s), "ch=0 run=0 outputs=0/0");
}

TEST(ChannelManagerStatusFormatter, CountsCreatedChannels) {
    ChannelManager mgr;
    int id = 0;
    ASSERT_EQ(mgr.create(minimalCfg(11), &id), R::Ok);
    ASSERT_EQ(mgr.create(minimalCfg(12), &id), R::Ok);
    const auto s = mgr.snapshotForStatus();
    EXPECT_EQ(s.channels, 2);
    EXPECT_EQ(s.running + s.degraded + s.failed, 2);
}
