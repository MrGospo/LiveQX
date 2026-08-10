// Phase E of stage 2.10 — fix/fix2.md.
//
// End-to-end ContentSync wiring: ShareScanner → CacheManager → Timeline.
// Uses ClipFactory (FFmpeg) so this test lives in the itests target.
//
// We drive ContentSync deterministically via tick() instead of start()/stop()
// so scenarios stay fast and free of sleeps.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "content/ContentSync.h"
#include "core/FramePool.h"
#include "core/Timeline.h"
#include "metrics/ChannelMetrics.h"

namespace fs = std::filesystem;

namespace {

class ContentSyncIT : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_  = fs::temp_directory_path() / ("cs_it_" + std::to_string(stamp));
        share_ = root_ / "share";
        cache_ = root_ / "cache";
        fs::create_directories(share_);

        decode_pool_ = std::make_shared<FramePool>(4, kW, kH);
        metrics_     = std::make_shared<ChannelMetrics>();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // tests/test_media is reachable because the itests target sets
    // WORKING_DIRECTORY=${CMAKE_SOURCE_DIR}.
    fs::path copyToShare(const std::string& filename, const std::string& as = {}) {
        const fs::path src = fs::path("test_media") / filename;
        const fs::path dst = share_ / (as.empty() ? filename : as);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        // Backdate so the next two scans see identical mtime → ShareScanner
        // promotes it to stable on the second pass.
        fs::last_write_time(dst, fs::file_time_type::clock::now()
                                 - std::chrono::seconds(2));
        return dst;
    }

    ContentSync::Config makeCfg() {
        ContentSync::Config c;
        c.share_dir                    = share_;
        c.cache_dir                    = cache_;
        c.max_file_size_bytes          = 0;
        c.scan_interval                = std::chrono::milliseconds(50);
        c.video_width                  = kW;
        c.video_height                 = kH;
        c.default_photo_duration_sec   = 5.0;
        c.default_transition           = {TransitionType::HardCut,
                                          TransitionMode::HardCut, 0.0};
        return c;
    }

    ContentSync::Config makePassthroughCfg() {
        auto c = makeCfg();
        c.cache_dir.clear();   // → use_cache=false
        return c;
    }

    std::unique_ptr<ContentSync> makeSync(Timeline& tl) {
        return std::make_unique<ContentSync>(
            makeCfg(), tl, graveyard_,
            [this] { return active_idx_.load(); },
            decode_pool_, metrics_, "ch_test");
    }

    std::unique_ptr<ContentSync> makePassthroughSync(Timeline& tl) {
        return std::make_unique<ContentSync>(
            makePassthroughCfg(), tl, graveyard_,
            [this] { return active_idx_.load(); },
            decode_pool_, metrics_, "ch_test");
    }

    static constexpr int kW = 320;
    static constexpr int kH = 240;

    fs::path root_, share_, cache_;
    std::shared_ptr<FramePool>      decode_pool_;
    std::shared_ptr<ChannelMetrics> metrics_;
    std::atomic<int>                active_idx_{-1};
    ClipGraveyard                   graveyard_;
};

}  // namespace

TEST_F(ContentSyncIT, EmptyShareEmptyTimeline) {
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();          // 2 scans, nothing to add
    EXPECT_EQ(tl.getPlaylistSize(), 0);
    EXPECT_GT(metrics_->last_share_ok_ns.load(), 0);
}

TEST_F(ContentSyncIT, MissingShareIncrementsUnreachable) {
    fs::remove_all(share_);          // delete share dir
    Timeline tl;
    auto cs = makeSync(tl);
    EXPECT_FALSE(cs->tick());
    EXPECT_FALSE(cs->tick());
    EXPECT_GE(metrics_->share_unreachable_count.load(), 2u);
    EXPECT_EQ(metrics_->last_share_ok_ns.load(), 0);
    EXPECT_EQ(tl.getPlaylistSize(), 0);
}

TEST_F(ContentSyncIT, TwoFilesIngestedAndAppendedInOrder) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");

    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick();                       // first sighting (not stable)
    EXPECT_EQ(tl.getPlaylistSize(), 0);
    cs->tick();                       // second sighting → both added
    EXPECT_EQ(tl.getPlaylistSize(), 2);

    auto snap = tl.snapshot();
    EXPECT_EQ(fs::path(snap->cache_paths[0]).filename(), "photo1.png");
    EXPECT_EQ(fs::path(snap->cache_paths[1]).filename(), "photo2.png");
    EXPECT_EQ(metrics_->cache_files_count.load(), 2u);
    EXPECT_GT(metrics_->cache_size_bytes.load(),  0u);
}

TEST_F(ContentSyncIT, RemoveDeferredUntilNotActive) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");

    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();          // both added
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    // Pretend RenderLoop is currently playing clip 0 (photo1.png).
    active_idx_.store(0);

    fs::remove(share_ / "photo1.png");
    cs->tick();                      // mark for removal + reap; active is preserved

    EXPECT_EQ(tl.getPlaylistSize(), 2);  // not yet evicted
    auto snap = tl.snapshot();
    EXPECT_TRUE (snap->pending_remove[0]);
    EXPECT_FALSE(snap->pending_remove[1]);
    EXPECT_EQ(metrics_->pending_deletes.load(), 1u);
    EXPECT_TRUE(fs::exists(cache_ / "photo1.png"));   // still in cache

    // Now switch active to clip 1 — next tick reaps photo1.
    active_idx_.store(1);
    cs->tick();
    EXPECT_EQ(tl.getPlaylistSize(), 1);
    EXPECT_FALSE(fs::exists(cache_ / "photo1.png"));
    EXPECT_EQ(metrics_->cache_files_count.load(), 1u);
}

TEST_F(ContentSyncIT, AddDuringRuntimeAppendsToEnd) {
    copyToShare("photo1.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 1);

    // Pretend playback in progress on clip 0.
    active_idx_.store(0);

    copyToShare("photo2.png", "late.png");
    cs->tick();                      // first sighting
    EXPECT_EQ(tl.getPlaylistSize(), 1);
    cs->tick();                      // promote → append
    EXPECT_EQ(tl.getPlaylistSize(), 2);

    auto snap = tl.snapshot();
    EXPECT_EQ(fs::path(snap->cache_paths[1]).filename(), "late.png");
}

TEST_F(ContentSyncIT, OversizedFileSkipped) {
    // Tight per-file limit (100 bytes). photo1.png is bigger than that.
    Timeline tl;
    auto cfg = makeCfg();
    cfg.max_file_size_bytes = 100;
    ContentSync cs(cfg, tl, graveyard_, [this]{ return active_idx_.load(); },
                   decode_pool_, metrics_, "ch_test");

    copyToShare("photo1.png");
    cs.tick(); cs.tick();
    EXPECT_EQ(tl.getPlaylistSize(), 0);
    EXPECT_GE(metrics_->oversized_skipped.load(), 1u);
}

TEST_F(ContentSyncIT, RestoreFromDiskSkipsRecopy) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    {
        Timeline tl;
        auto cs = makeSync(tl);
        cs->tick(); cs->tick();
        ASSERT_EQ(tl.getPlaylistSize(), 2);
    }
    ASSERT_TRUE(fs::exists(cache_ / "photo1.png"));
    ASSERT_TRUE(fs::exists(cache_ / "photo2.png"));

    // Fresh ContentSync — discovers cache_ entries lexicographically.
    Timeline tl2;
    auto cs2 = makeSync(tl2);
    const auto restored = cs2->restoreFromDisk();
    EXPECT_EQ(restored, 2u);
    EXPECT_EQ(tl2.getPlaylistSize(), 2);
    auto snap = tl2.snapshot();
    EXPECT_EQ(fs::path(snap->cache_paths[0]).filename(), "photo1.png");
    EXPECT_EQ(fs::path(snap->cache_paths[1]).filename(), "photo2.png");
}

TEST_F(ContentSyncIT, RemoveAllDrainsToEmpty) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    // No active clip — every pending entry can be reaped immediately.
    active_idx_.store(-1);
    fs::remove(share_ / "photo1.png");
    fs::remove(share_ / "photo2.png");
    cs->tick();
    EXPECT_EQ(tl.getPlaylistSize(), 0);
    EXPECT_FALSE(fs::exists(cache_ / "photo1.png"));
    EXPECT_FALSE(fs::exists(cache_ / "photo2.png"));
}

// ─── Passthrough mode (local folder, no cache) — Stage 3.3 ───────────────────

TEST_F(ContentSyncIT, PassthroughModeNoCacheCopy) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");

    Timeline tl;
    auto cs = makePassthroughSync(tl);
    EXPECT_FALSE(cs->usesCache());

    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    // Timeline must reference the source paths directly.
    auto snap = tl.snapshot();
    EXPECT_EQ(snap->cache_paths[0], (share_ / "photo1.png").string());
    EXPECT_EQ(snap->cache_paths[1], (share_ / "photo2.png").string());

    // Cache dir was never created.
    EXPECT_FALSE(fs::exists(cache_));
    EXPECT_EQ(metrics_->cache_files_count.load(), 0u);
    EXPECT_EQ(metrics_->cache_size_bytes.load(),  0u);
}

TEST_F(ContentSyncIT, PassthroughRemoveAppliesToTimeline) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makePassthroughSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    active_idx_.store(-1);  // nothing playing → reap freely
    fs::remove(share_ / "photo1.png");
    cs->tick();

    EXPECT_EQ(tl.getPlaylistSize(), 1);
    auto snap = tl.snapshot();
    EXPECT_EQ(fs::path(snap->cache_paths[0]).filename(), "photo2.png");
}

TEST_F(ContentSyncIT, PassthroughEmptyToFilesAddsAtRuntime) {
    Timeline tl;
    auto cs = makePassthroughSync(tl);
    cs->tick();
    EXPECT_EQ(tl.getPlaylistSize(), 0);

    copyToShare("photo1.png");
    cs->tick();   // first sighting
    EXPECT_EQ(tl.getPlaylistSize(), 0);
    cs->tick();   // promote → append
    EXPECT_EQ(tl.getPlaylistSize(), 1);
}

// ─── Disconnect / reconnect (cache mode) — Stage 3.3 audit ───────────────────

TEST_F(ContentSyncIT, DisconnectPreservesPlaylistAndCache) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);
    ASSERT_TRUE(fs::exists(cache_ / "photo1.png"));
    ASSERT_TRUE(fs::exists(cache_ / "photo2.png"));

    // Simulate share unmount.
    fs::remove_all(share_);

    // Several unreachable ticks must not touch Timeline or cache.
    EXPECT_FALSE(cs->tick());
    EXPECT_FALSE(cs->tick());
    EXPECT_FALSE(cs->tick());

    EXPECT_EQ(tl.getPlaylistSize(), 2);
    EXPECT_TRUE(fs::exists(cache_ / "photo1.png"));
    EXPECT_TRUE(fs::exists(cache_ / "photo2.png"));
    auto snap = tl.snapshot();
    EXPECT_FALSE(snap->pending_remove[0]);
    EXPECT_FALSE(snap->pending_remove[1]);
}

TEST_F(ContentSyncIT, ReconnectAppliesDiff) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    // Disconnect.
    fs::remove_all(share_);
    EXPECT_FALSE(cs->tick());

    // While offline: photo1 deleted, photo3 added.
    fs::create_directories(share_);
    copyToShare("photo1.png", "photo3.png");   // photo3 only

    // Reconnect — reconcile detects photo1+photo2 missing, photo3 new.
    active_idx_.store(-1);   // free reaping
    cs->tick();              // reconcile + first sighting of photo3
    cs->tick();              // promote photo3

    auto snap = tl.snapshot();
    // photo1 + photo2 reaped, photo3 appended.
    ASSERT_EQ(snap->clips.size(), 1u);
    EXPECT_EQ(fs::path(snap->cache_paths[0]).filename(), "photo3.png");
    EXPECT_TRUE (fs::exists(cache_ / "photo3.png"));
    EXPECT_FALSE(fs::exists(cache_ / "photo1.png"));
    EXPECT_FALSE(fs::exists(cache_ / "photo2.png"));
}

TEST_F(ContentSyncIT, DisconnectDuringActivePlaybackContinues) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);

    // Pretend RenderLoop on clip 0.
    active_idx_.store(0);

    fs::remove_all(share_);
    EXPECT_FALSE(cs->tick());
    EXPECT_FALSE(cs->tick());

    auto snap = tl.snapshot();
    EXPECT_EQ(snap->clips.size(), 2u);
    EXPECT_FALSE(snap->pending_remove[0]);   // active untouched
    EXPECT_FALSE(snap->pending_remove[1]);
}

TEST_F(ContentSyncIT, ReconnectSameFilesNoChurn) {
    copyToShare("photo1.png");
    copyToShare("photo2.png");
    Timeline tl;
    auto cs = makeSync(tl);
    cs->tick(); cs->tick();
    ASSERT_EQ(tl.getPlaylistSize(), 2);
    auto snap_before = tl.snapshot();

    // Disconnect, then reconnect with identical contents.
    fs::remove_all(share_);
    EXPECT_FALSE(cs->tick());
    fs::create_directories(share_);
    copyToShare("photo1.png");
    copyToShare("photo2.png");

    cs->tick();   // reconcile (nothing changed) + first sighting
    cs->tick();   // promote (already in timeline → skip)

    // Same Timeline shape, no pending_remove churn.
    auto snap_after = tl.snapshot();
    EXPECT_EQ(snap_after->clips.size(), 2u);
    EXPECT_EQ(snap_after->cache_paths, snap_before->cache_paths);
    EXPECT_FALSE(snap_after->pending_remove[0]);
    EXPECT_FALSE(snap_after->pending_remove[1]);
    // No re-ingest churn.
    EXPECT_EQ(metrics_->cache_files_count.load(), 2u);
}
