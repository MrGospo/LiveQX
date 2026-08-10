// Unit tests for CacheManager (Phase A of stage 2.10 — fix/fix2.md).
//
// Covers:
//   - cache_dir auto-create
//   - ingest of a valid image (atomic copy + rename)
//   - ingest oversized → counter, no file in cache
//   - ingest unsupported extension → ignored
//   - ingest invalid video → file removed, counter
//   - evict removes file and entry
//   - restoreFromDisk re-discovers entries without re-copying
//   - restoreFromDisk drops leftover *.tmp
//   - restoreFromDisk drops invalid cached video

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "content/CacheManager.h"

namespace fs = std::filesystem;

namespace {

class CacheManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_  = fs::temp_directory_path() / ("cm_test_" + std::to_string(stamp));
        share_ = root_ / "share";
        cache_ = root_ / "cache";
        fs::create_directories(share_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // Copy bytes from the project test_media to our tmp share dir.
    fs::path copyFromTestMedia(const std::string& filename) {
        // Tests run with WORKING_DIRECTORY=${CMAKE_SOURCE_DIR}, so test_media/* is reachable.
        const fs::path src = fs::path("test_media") / filename;
        const fs::path dst = share_ / filename;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        return dst;
    }

    fs::path makeJunkFile(const std::string& filename, std::size_t bytes) {
        const fs::path p = share_ / filename;
        std::ofstream o(p, std::ios::binary);
        std::string blob(bytes, 'X');
        o.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        return p;
    }

    fs::path root_, share_, cache_;
};

}  // namespace

TEST_F(CacheManagerTest, CreatesCacheDir) {
    CacheManager cm({cache_, 0});
    EXPECT_TRUE(fs::exists(cache_));
    EXPECT_TRUE(fs::is_directory(cache_));
}

TEST_F(CacheManagerTest, IngestImageCopiesAndRecords) {
    CacheManager cm({cache_, 0});
    const auto src = copyFromTestMedia("photo1.png");

    auto e = cm.ingest(src);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->filename, "photo1.png");
    EXPECT_TRUE(fs::exists(e->cache_path));
    EXPECT_GT(e->size_bytes, 0u);
    EXPECT_EQ(cm.listCached().size(), 1u);
    EXPECT_FALSE(fs::exists(cache_ / "photo1.png.tmp"));
}

TEST_F(CacheManagerTest, OversizedFileIsSkipped) {
    CacheManager cm({cache_, /*max_file_size=*/100});  // 100 bytes
    const auto src = makeJunkFile("big.png", 10'000);

    auto e = cm.ingest(src);
    EXPECT_FALSE(e.has_value());
    EXPECT_EQ(cm.oversizedSkipped(), 1u);
    EXPECT_TRUE(cm.listCached().empty());
    EXPECT_FALSE(fs::exists(cache_ / "big.png"));
}

TEST_F(CacheManagerTest, UnsupportedExtensionIgnored) {
    CacheManager cm({cache_, 0});
    const auto src = makeJunkFile("readme.txt", 16);

    auto e = cm.ingest(src);
    EXPECT_FALSE(e.has_value());
    EXPECT_TRUE(cm.listCached().empty());
}

TEST_F(CacheManagerTest, InvalidVideoIsRejectedAndRemoved) {
    CacheManager cm({cache_, 0});
    // 64 bytes of garbage with .mp4 extension — MediaValidator must reject.
    const auto src = makeJunkFile("broken.mp4", 64);

    auto e = cm.ingest(src);
    EXPECT_FALSE(e.has_value());
    EXPECT_GE(cm.copyErrors(), 1u);
    EXPECT_FALSE(fs::exists(cache_ / "broken.mp4"));
}

TEST_F(CacheManagerTest, EvictRemovesFileAndEntry) {
    CacheManager cm({cache_, 0});
    const auto src = copyFromTestMedia("photo1.png");
    ASSERT_TRUE(cm.ingest(src).has_value());

    EXPECT_TRUE(cm.evict("photo1.png"));
    EXPECT_FALSE(fs::exists(cache_ / "photo1.png"));
    EXPECT_TRUE(cm.listCached().empty());
    EXPECT_FALSE(cm.evict("photo1.png"));  // idempotent
}

TEST_F(CacheManagerTest, RestoreFromDiskRediscovers) {
    {
        CacheManager cm({cache_, 0});
        cm.ingest(copyFromTestMedia("photo1.png"));
        cm.ingest(copyFromTestMedia("photo2.png"));
    }
    // New manager — must find both without re-copying.
    CacheManager cm2({cache_, 0});
    EXPECT_EQ(cm2.restoreFromDisk(), 2u);
    EXPECT_EQ(cm2.listCached().size(), 2u);
    EXPECT_TRUE(cm2.listCached().count("photo1.png"));
    EXPECT_TRUE(cm2.listCached().count("photo2.png"));
}

TEST_F(CacheManagerTest, RestoreDropsStaleTmp) {
    fs::create_directories(cache_);
    {
        std::ofstream o(cache_ / "halfwritten.png.tmp", std::ios::binary);
        o << "garbage";
    }
    CacheManager cm({cache_, 0});
    EXPECT_EQ(cm.restoreFromDisk(), 0u);
    EXPECT_FALSE(fs::exists(cache_ / "halfwritten.png.tmp"));
}

TEST_F(CacheManagerTest, RestoreDropsInvalidCachedVideo) {
    fs::create_directories(cache_);
    {
        std::ofstream o(cache_ / "broken.mp4", std::ios::binary);
        std::string blob(128, 'X');
        o.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }
    CacheManager cm({cache_, 0});
    EXPECT_EQ(cm.restoreFromDisk(), 0u);
    EXPECT_FALSE(fs::exists(cache_ / "broken.mp4"));
}

TEST_F(CacheManagerTest, CacheSizeBytesAggregates) {
    CacheManager cm({cache_, 0});
    cm.ingest(copyFromTestMedia("photo1.png"));
    cm.ingest(copyFromTestMedia("photo2.png"));
    EXPECT_GT(cm.cacheSizeBytes(), 0u);
}
