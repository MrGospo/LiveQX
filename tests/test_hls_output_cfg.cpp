#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "output/HlsOutputCfg.h"

using nlohmann::json;
using namespace liveqx::hls;

namespace {

// Helper RAII directory: every cfg test that exercises the writability and
// directory-existence checks needs a real, writable filesystem location.
// Test bodies clean up via the destructor regardless of assertion path.
struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto cand = base / ("hls_cfg_test_" + std::to_string(::getpid())
                                       + "_" + std::to_string(i));
            std::error_code ec;
            if (std::filesystem::create_directory(cand, ec)) {
                path = cand;
                return;
            }
        }
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// ─── happy path ──────────────────────────────────────────────────────────────

TEST(HlsOutputCfg, MinimalValidJson) {
    TmpDir d;
    auto j = json{{"output_dir", d.path.string()}};
    auto c = parseOutputCfg(j);
    EXPECT_EQ(c.output_dir,                d.path.string());
    EXPECT_EQ(c.segment_duration_sec,      6);
    EXPECT_EQ(c.playlist_size,             5);
    EXPECT_EQ(c.delete_segments_after_sec, 60);
    EXPECT_EQ(c.playlist_filename,         "stream.m3u8");
    EXPECT_EQ(c.segment_filename_pattern,  "seg_%05d.ts");
}

TEST(HlsOutputCfg, AllKnobsTuned) {
    TmpDir d;
    json j = {
        {"output_dir",                 d.path.string()},
        {"segment_duration_sec",       4},
        {"playlist_size",              10},
        {"delete_segments_after_sec",  120},
        {"playlist_filename",          "live.m3u8"},
        {"segment_filename_pattern",   "chunk_%d.ts"},
    };
    auto c = parseOutputCfg(j);
    EXPECT_EQ(c.segment_duration_sec,      4);
    EXPECT_EQ(c.playlist_size,             10);
    EXPECT_EQ(c.delete_segments_after_sec, 120);
    EXPECT_EQ(c.playlist_filename,         "live.m3u8");
    EXPECT_EQ(c.segment_filename_pattern,  "chunk_%d.ts");
}

// ─── output_dir validation ───────────────────────────────────────────────────

TEST(HlsOutputCfg, RejectsNonObject) {
    EXPECT_THROW(parseOutputCfg(json::array()), std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsMissingOutputDir) {
    EXPECT_THROW(parseOutputCfg(json{{"segment_duration_sec", 6}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsEmptyOutputDir) {
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", ""}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsRelativeOutputDir) {
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", "var/hls"}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsNonExistentOutputDir) {
    EXPECT_THROW(
        parseOutputCfg(json{{"output_dir", "/this/path/does/not/exist/anywhere"}}),
        std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsOutputDirThatIsAFile) {
    TmpDir d;
    const auto file = d.path / "not_a_dir";
    std::ofstream(file) << "x";
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", file.string()}}),
                 std::invalid_argument);
}

// ─── range validation ────────────────────────────────────────────────────────

TEST(HlsOutputCfg, RejectsSegmentDurationBelowRange) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_duration_sec", 1}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsSegmentDurationAboveRange) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_duration_sec", 31}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsPlaylistSizeTooSmall) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"playlist_size", 2}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsDeleteWindowSmallerThanCoverage) {
    // delete_segments_after_sec must cover at least one full sliding window.
    // 6 × 5 = 30, so 29 must fail.
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_duration_sec", 6},
                                     {"playlist_size", 5},
                                     {"delete_segments_after_sec", 29}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, AcceptsDeleteWindowExactlyEqualToCoverage) {
    TmpDir d;
    auto c = parseOutputCfg(json{{"output_dir", d.path.string()},
                                 {"segment_duration_sec", 6},
                                 {"playlist_size", 5},
                                 {"delete_segments_after_sec", 30}});
    EXPECT_EQ(c.delete_segments_after_sec, 30);
}

// ─── name validation ─────────────────────────────────────────────────────────

TEST(HlsOutputCfg, RejectsPlaylistFilenameWithSlash) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"playlist_filename", "sub/stream.m3u8"}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsEmptyPlaylistFilename) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"playlist_filename", ""}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsPatternWithoutPlaceholder) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_filename_pattern", "seg.ts"}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsPatternWithMultiplePlaceholders) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_filename_pattern",
                                      "seg_%d_%05d.ts"}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, RejectsPatternWithSlash) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_filename_pattern",
                                      "sub/seg_%05d.ts"}}),
                 std::invalid_argument);
}

TEST(HlsOutputCfg, AcceptsPercentLiteralPlusOnePlaceholder) {
    // "%%" is the FFmpeg literal-percent escape and must NOT count as a
    // placeholder. So "%%seg_%05d.ts" carries exactly one placeholder.
    TmpDir d;
    auto c = parseOutputCfg(json{{"output_dir", d.path.string()},
                                 {"segment_filename_pattern", "%%seg_%05d.ts"}});
    EXPECT_EQ(c.segment_filename_pattern, "%%seg_%05d.ts");
}

TEST(HlsOutputCfg, RejectsBadFieldTypes) {
    TmpDir d;
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"segment_duration_sec", "6"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"playlist_size", "5"}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"output_dir", d.path.string()},
                                     {"playlist_filename", 42}}),
                 std::invalid_argument);
}

} // namespace
