// fix26 c8 — unit tests for ClipCorrupt scenario.
//
// Drives the scenario directly against temporary files in a per-test
// directory. No ChannelManager is needed because clip_corrupt only
// touches files on disk — the FallbackClip plumbing it triggers is
// tested elsewhere.
//
// Intervals are tightened to milliseconds so each test runs in a few
// ms and remains deterministic.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "stress/ClipCorrupt.h"
#include "stress/Scenario.h"

namespace fs = std::filesystem;
using nlohmann::json;
using liveqx::stress::ClipCorrupt;
using liveqx::stress::ClipCorruptIntervals;
using liveqx::stress::makeScenario;
using liveqx::stress::ScenarioContext;
using liveqx::stress::ScenarioEvent;

namespace {

fs::path uniqueTempDir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("clip_corrupt_test_" + tag + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(&tag)));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

fs::path makeFile(const fs::path& dir, const std::string& name,
                  const std::vector<std::uint8_t>& bytes) {
    auto p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return p;
}

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

ScenarioContext makeCtx(std::int64_t started_ms, std::uint64_t seed) {
    ScenarioContext ctx;
    ctx.mgr            = nullptr;       // clip_corrupt does not need it
    ctx.run_started_ms = started_ms;
    ctx.rng.seed(seed);
    return ctx;
}

}  // namespace

TEST(ClipCorrupt, KillTruncatesFileToZeroBytes) {
    auto dir  = uniqueTempDir("kill_truncate");
    std::vector<std::uint8_t> payload(1024, 0xAB);
    auto file = makeFile(dir, "clip.bin", payload);

    ClipCorruptIntervals iv{1, 1, 100000, 100000};   // kill at 1ms, restore far away
    ClipCorrupt sc({file}, iv);
    auto ctx = makeCtx(/*started_ms=*/0, /*seed=*/1);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, /*now_ms=*/2, /*elapsed_ms=*/2, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_TRUE(evs[0].ok) << evs[0].detail;
    EXPECT_NE(evs[0].detail.find("corrupted"), std::string::npos);
    EXPECT_EQ(fs::file_size(file), 0u);

    fs::remove_all(dir);
}

TEST(ClipCorrupt, RestoreReturnsOriginalBytes) {
    auto dir  = uniqueTempDir("restore_bytes");
    std::vector<std::uint8_t> payload;
    payload.reserve(2048);
    for (int i = 0; i < 2048; ++i) payload.push_back(static_cast<std::uint8_t>(i));
    auto file = makeFile(dir, "clip.bin", payload);

    ClipCorruptIntervals iv{1, 1, 5, 5};
    ClipCorrupt sc({file}, iv);
    auto ctx = makeCtx(0, 2);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);                 // kill
    ASSERT_EQ(fs::file_size(file), 0u);

    sc.tick(ctx, 100, 100, evs);             // > restore_at -> restore
    ASSERT_GE(evs.size(), 2u);
    bool saw_restore = false;
    for (const auto& e : evs) {
        if (e.detail.find("restored") != std::string::npos) saw_restore = true;
    }
    EXPECT_TRUE(saw_restore);

    auto restored = readFile(file);
    EXPECT_EQ(restored, payload);

    fs::remove_all(dir);
}

TEST(ClipCorrupt, OnFinishRestoresPendingKill) {
    auto dir  = uniqueTempDir("on_finish");
    std::vector<std::uint8_t> payload(64, 0x77);
    auto file = makeFile(dir, "clip.bin", payload);

    ClipCorruptIntervals iv{1, 1, /*restore far away*/100000, 100000};
    ClipCorrupt sc({file}, iv);
    auto ctx = makeCtx(0, 3);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);                 // kill
    ASSERT_EQ(fs::file_size(file), 0u);

    sc.onFinish(ctx, evs);                   // must restore
    auto restored = readFile(file);
    EXPECT_EQ(restored, payload);

    fs::remove_all(dir);
}

TEST(ClipCorrupt, MissingPathProducesErrorEvent) {
    auto dir = uniqueTempDir("missing_path");
    fs::path bogus = dir / "does_not_exist.bin";

    ClipCorruptIntervals iv{1, 1, 10, 10};
    ClipCorrupt sc({bogus}, iv);
    auto ctx = makeCtx(0, 4);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_FALSE(evs[0].ok);
    EXPECT_NE(evs[0].detail.find("does not exist"), std::string::npos);

    fs::remove_all(dir);
}

TEST(ClipCorrupt, EmptyCandidatesNoOp) {
    ClipCorruptIntervals iv{1, 1, 5, 5};
    ClipCorrupt sc({}, iv);
    auto ctx = makeCtx(0, 5);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 100, 100, evs);
    // No paths -> nothing happens, no events emitted, no crash.
    EXPECT_TRUE(evs.empty());
    sc.onFinish(ctx, evs);
    EXPECT_TRUE(evs.empty());
}

TEST(ClipCorrupt, ZeroByteFileRoundTripsCleanly) {
    auto dir  = uniqueTempDir("empty_file");
    auto file = makeFile(dir, "clip.bin", {});
    ASSERT_EQ(fs::file_size(file), 0u);

    ClipCorruptIntervals iv{1, 1, 5, 5};
    ClipCorrupt sc({file}, iv);
    auto ctx = makeCtx(0, 6);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);          // kill (file already 0 bytes)
    EXPECT_EQ(fs::file_size(file), 0u);
    sc.tick(ctx, 100, 100, evs);      // restore — backup was empty, write must not throw
    EXPECT_EQ(fs::file_size(file), 0u);

    fs::remove_all(dir);
}

TEST(ScenarioFactory, ClipCorruptRequiresPaths) {
    // Empty options -> nullptr with warning.
    auto null1 = makeScenario("clip_corrupt", {});
    EXPECT_EQ(null1, nullptr);

    // Empty paths array -> nullptr with warning.
    auto null2 = makeScenario("clip_corrupt", json{{"paths", json::array()}});
    EXPECT_EQ(null2, nullptr);

    // Valid options -> instance.
    auto good = makeScenario("clip_corrupt",
                             json{{"paths", {"/tmp/some_clip.mp4"}}});
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->name(), "clip_corrupt");
}
