#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "output/HlsOrphanSweep.h"
#include "output/HlsOutputCfg.h"

namespace fs = std::filesystem;
using namespace liveqx::hls;

namespace {

// RAII: every test creates an isolated tmpdir so concurrent ctest workers
// can't see each other's segment files.
struct TmpDir {
    fs::path path;
    TmpDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto cand = base / ("hls_sweep_test_" + std::to_string(::getpid())
                                       + "_" + std::to_string(i));
            std::error_code ec;
            if (fs::create_directory(cand, ec)) {
                path = cand;
                return;
            }
        }
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touchFile(const fs::path& p, const std::string& body = "x") {
    std::ofstream(p) << body;
}

void backdateFile(const fs::path& p, std::chrono::seconds age) {
    const auto ftime = fs::file_time_type::clock::now() - age;
    fs::last_write_time(p, ftime);
}

HlsCfg makeCfg(const fs::path& dir,
               int duration   = 6,
               int playlist   = 5,
               int delete_sec = 60,
               std::string pattern = "seg_%05d.ts") {
    HlsCfg c;
    c.output_dir                = dir.string();
    c.segment_duration_sec      = duration;
    c.playlist_size             = playlist;
    c.delete_segments_after_sec = delete_sec;
    c.playlist_filename         = "stream.m3u8";
    c.segment_filename_pattern  = std::move(pattern);
    return c;
}

} // namespace

// ─── splitPattern ────────────────────────────────────────────────────────────

TEST(HlsSplitPattern, SimpleZeroPadded) {
    auto p = splitPattern("seg_%05d.ts");
    EXPECT_EQ(p.prefix, "seg_");
    EXPECT_EQ(p.suffix, ".ts");
}

TEST(HlsSplitPattern, BarePercentD) {
    auto p = splitPattern("chunk_%d.ts");
    EXPECT_EQ(p.prefix, "chunk_");
    EXPECT_EQ(p.suffix, ".ts");
}

TEST(HlsSplitPattern, PercentEscapeIsLiteral) {
    // %% must NOT be treated as a placeholder; the real one comes after.
    auto p = splitPattern("%%seg_%05d.ts");
    EXPECT_EQ(p.prefix, "%%seg_");
    EXPECT_EQ(p.suffix, ".ts");
}

TEST(HlsSplitPattern, NoPlaceholderReturnsEmptySuffix) {
    auto p = splitPattern("static.ts");
    EXPECT_EQ(p.prefix, "static.ts");
    EXPECT_EQ(p.suffix, "");
}

// ─── matchesSegmentName ──────────────────────────────────────────────────────

TEST(HlsMatchesSegmentName, MatchesZeroPaddedNumeric) {
    PatternParts p{"seg_", ".ts"};
    EXPECT_TRUE (matchesSegmentName("seg_00042.ts", p));
    EXPECT_TRUE (matchesSegmentName("seg_1.ts",     p));    // shorter than %05d width OK
    EXPECT_TRUE (matchesSegmentName("seg_999999.ts", p));   // wider than %05d OK
}

TEST(HlsMatchesSegmentName, RejectsNonDigitMiddle) {
    PatternParts p{"seg_", ".ts"};
    EXPECT_FALSE(matchesSegmentName("seg_abc.ts",   p));
    EXPECT_FALSE(matchesSegmentName("seg_42x.ts",   p));
    EXPECT_FALSE(matchesSegmentName("seg_.ts",      p));    // empty middle
}

TEST(HlsMatchesSegmentName, RejectsWrongPrefixOrSuffix) {
    PatternParts p{"seg_", ".ts"};
    EXPECT_FALSE(matchesSegmentName("xseg_42.ts",   p));
    EXPECT_FALSE(matchesSegmentName("seg_42.tsx",   p));
    EXPECT_FALSE(matchesSegmentName("stream.m3u8",  p));
}

// ─── sweepOrphans ────────────────────────────────────────────────────────────

TEST(HlsSweepOrphans, RemovesTmpOrphansMatchingPattern) {
    TmpDir d;
    touchFile(d.path / "seg_00001.ts.tmp");
    touchFile(d.path / "seg_00002.ts.tmp");
    touchFile(d.path / "seg_00003.ts");          // not orphan, recent
    auto r = sweepOrphans(makeCfg(d.path));
    EXPECT_EQ(r.tmp_removed,   2);
    EXPECT_EQ(r.stale_removed, 0);
    EXPECT_FALSE(fs::exists(d.path / "seg_00001.ts.tmp"));
    EXPECT_FALSE(fs::exists(d.path / "seg_00002.ts.tmp"));
    EXPECT_TRUE (fs::exists(d.path / "seg_00003.ts"));
}

TEST(HlsSweepOrphans, IgnoresUnrelatedTmpFiles) {
    TmpDir d;
    touchFile(d.path / "seg_00001.ts.tmp");      // matches → swept
    touchFile(d.path / "stream.m3u8.tmp");       // playlist tmp, not segment
    touchFile(d.path / "operator-note.tmp");     // foreign file
    auto r = sweepOrphans(makeCfg(d.path));
    EXPECT_EQ(r.tmp_removed, 1);
    EXPECT_FALSE(fs::exists(d.path / "seg_00001.ts.tmp"));
    EXPECT_TRUE (fs::exists(d.path / "stream.m3u8.tmp"));
    EXPECT_TRUE (fs::exists(d.path / "operator-note.tmp"));
}

TEST(HlsSweepOrphans, RemovesStaleSegmentsOlderThanDeleteWindow) {
    TmpDir d;
    auto cfg = makeCfg(d.path, /*duration=*/6, /*playlist=*/5, /*delete_sec=*/60);

    touchFile(d.path / "seg_00010.ts");
    backdateFile(d.path / "seg_00010.ts", std::chrono::seconds(120));   // very stale

    touchFile(d.path / "seg_00011.ts");                                  // fresh

    auto r = sweepOrphans(cfg);
    EXPECT_EQ(r.stale_removed, 1);
    EXPECT_FALSE(fs::exists(d.path / "seg_00010.ts"));
    EXPECT_TRUE (fs::exists(d.path / "seg_00011.ts"));
}

TEST(HlsSweepOrphans, IgnoresUnrelatedStaleFiles) {
    TmpDir d;
    auto cfg = makeCfg(d.path);

    touchFile(d.path / "operator_log.txt");
    backdateFile(d.path / "operator_log.txt", std::chrono::seconds(99999));

    touchFile(d.path / "stream.m3u8");
    backdateFile(d.path / "stream.m3u8", std::chrono::seconds(99999));

    auto r = sweepOrphans(cfg);
    EXPECT_EQ(r.tmp_removed,   0);
    EXPECT_EQ(r.stale_removed, 0);
    EXPECT_TRUE(fs::exists(d.path / "operator_log.txt"));
    EXPECT_TRUE(fs::exists(d.path / "stream.m3u8"));
}

TEST(HlsSweepOrphans, MissingDirReportsErrorNotCrash) {
    HlsCfg cfg;
    cfg.output_dir               = "/this/path/does/not/exist/anywhere";
    cfg.segment_filename_pattern = "seg_%05d.ts";
    cfg.delete_segments_after_sec = 60;
    cfg.segment_duration_sec      = 6;
    cfg.playlist_size             = 5;
    auto r = sweepOrphans(cfg);
    EXPECT_GT(r.errors, 0);
}

TEST(HlsSweepOrphans, PatternWithoutPlaceholderIsBail) {
    // Defence in depth — HlsCfg validation already requires exactly one
    // placeholder, but a directly-constructed cfg could skip that.
    // sweepOrphans must NOT fall back to globbing the whole directory.
    TmpDir d;
    touchFile(d.path / "anything.ts");
    touchFile(d.path / "anything.ts.tmp");

    HlsCfg cfg = makeCfg(d.path);
    cfg.segment_filename_pattern = "static.ts";       // no %d
    auto r = sweepOrphans(cfg);
    EXPECT_EQ(r.tmp_removed,   0);
    EXPECT_EQ(r.stale_removed, 0);
    EXPECT_TRUE(fs::exists(d.path / "anything.ts"));
    EXPECT_TRUE(fs::exists(d.path / "anything.ts.tmp"));
}
