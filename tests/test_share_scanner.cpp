// Phase C of stage 2.10 — fix/fix2.md.
//
// ShareScanner is single-threaded and synchronous: each scan() returns a
// diff vs internal state. Tests use a tmp directory as a mock share.

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "content/ShareScanner.h"

namespace fs = std::filesystem;

namespace {

class ShareScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        share_ = fs::temp_directory_path() / ("ss_test_" + std::to_string(stamp));
        fs::create_directories(share_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(share_, ec);
    }

    fs::path makeFile(const std::string& name, std::size_t bytes = 32) {
        const fs::path p = share_ / name;
        std::ofstream o(p, std::ios::binary);
        std::string blob(bytes, 'X');
        o.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        return p;
    }

    // Stabilization needs two equal scans; bump mtime forward to make the
    // "unchanged" comparison meaningful even if filesystem mtime resolution
    // is coarse.
    void touchOlder(const fs::path& p) {
        auto t = fs::last_write_time(p) - std::chrono::seconds(2);
        fs::last_write_time(p, t);
    }

    fs::path share_;
};

}  // namespace

TEST_F(ShareScannerTest, MissingDirReportsUnreachable) {
    ShareScanner ss(share_ / "does_not_exist");
    auto d = ss.scan();
    EXPECT_TRUE(d.share_unreachable);
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
}

TEST_F(ShareScannerTest, EmptyDirNoDiff) {
    ShareScanner ss(share_);
    auto d = ss.scan();
    EXPECT_FALSE(d.share_unreachable);
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
}

TEST_F(ShareScannerTest, NewFileNeedsTwoScansToBeReported) {
    ShareScanner ss(share_);
    makeFile("a.png");
    touchOlder(share_ / "a.png");

    // First scan: see it but not yet stable.
    auto d1 = ss.scan();
    EXPECT_TRUE(d1.added.empty());
    EXPECT_EQ(ss.stableCount(), 0u);

    // Second scan: same mtime+size → emit Added.
    auto d2 = ss.scan();
    ASSERT_EQ(d2.added.size(), 1u);
    EXPECT_EQ(d2.added[0].filename(), "a.png");
    EXPECT_EQ(ss.stableCount(), 1u);

    // Third scan: stable, no new event.
    auto d3 = ss.scan();
    EXPECT_TRUE(d3.added.empty());
}

TEST_F(ShareScannerTest, RemovedFileReportedOnce) {
    ShareScanner ss(share_);
    makeFile("a.png");
    touchOlder(share_ / "a.png");
    ss.scan(); ss.scan();           // promote a.png to stable
    ASSERT_EQ(ss.stableCount(), 1u);

    fs::remove(share_ / "a.png");
    auto d = ss.scan();
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0], "a.png");

    auto d2 = ss.scan();
    EXPECT_TRUE(d2.removed.empty());
}

TEST_F(ShareScannerTest, UnstableFileNotExposedThenRemovedSilently) {
    ShareScanner ss(share_);
    makeFile("growing.mp4", 64);
    ss.scan();                       // first sighting (not stable)

    // Removed before stabilizing — must NOT appear in `removed`.
    fs::remove(share_ / "growing.mp4");
    auto d = ss.scan();
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
}

TEST_F(ShareScannerTest, MtimeChangeKeepsFileUnstable) {
    ShareScanner ss(share_);
    makeFile("x.png", 32);
    touchOlder(share_ / "x.png");
    ss.scan();                       // first sighting

    // Modify before second scan: still treated as in-flight.
    {
        std::ofstream o(share_ / "x.png", std::ios::binary | std::ios::app);
        o << "more";
    }
    auto d2 = ss.scan();
    EXPECT_TRUE(d2.added.empty());
    EXPECT_EQ(ss.stableCount(), 0u);

    // Now stable across two scans.
    touchOlder(share_ / "x.png");
    ss.scan();
    auto d4 = ss.scan();
    ASSERT_EQ(d4.added.size(), 1u);
    EXPECT_EQ(d4.added[0].filename(), "x.png");
}

TEST_F(ShareScannerTest, IgnoresUnknownExtensions) {
    ShareScanner ss(share_);
    makeFile("readme.txt");
    makeFile("notes.md");
    touchOlder(share_ / "readme.txt");
    touchOlder(share_ / "notes.md");

    ss.scan();
    auto d = ss.scan();
    EXPECT_TRUE(d.added.empty());
    EXPECT_EQ(ss.stableCount(), 0u);
}

TEST_F(ShareScannerTest, MultipleFilesBatched) {
    ShareScanner ss(share_);
    makeFile("a.png");
    makeFile("b.mp4", 128);
    makeFile("c.jpg");
    touchOlder(share_ / "a.png");
    touchOlder(share_ / "b.mp4");
    touchOlder(share_ / "c.jpg");

    ss.scan();
    auto d = ss.scan();
    EXPECT_EQ(d.added.size(), 3u);
    EXPECT_EQ(ss.stableCount(), 3u);
}
