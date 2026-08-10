// Phase B of stage 2.10 — fix/fix2.md.
//
// Validates that Timeline supports concurrent reads + RCU-style mutation:
//   - snapshot() returns a stable view across mutations
//   - appendClip extends the playlist without breaking older snapshots
//   - markForRemoval is observable in the next snapshot
//   - reapRemovable preserves the active clip
//   - concurrent stress: reads remain consistent under writes (no crashes,
//     no torn snapshots)

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/Timeline.h"

namespace {

class StubClip : public IClip {
public:
    explicit StubClip(double dur) : dur_(dur) {}
    Frame      getFrame()       override { return {}; }
    AudioFrame getAudio(int)    override { return {}; }
    Frame      getTailFrame()   override { return {}; }
    AudioFrame getTailAudio(int)override { return {}; }
    double     getDuration() const override { return dur_; }
    bool       hasAudio()    const override { return false; }
    bool       isPrepared()  const override { return true; }
    void       prepare()           override {}
    void       release()           override {}
    void       reset()             override {}
private:
    double dur_;
};

TransitionConfig hardCut() {
    return {TransitionType::HardCut, TransitionMode::HardCut, 0.0};
}

void seed(Timeline& tl, int n, double dur = 5.0) {
    std::vector<std::unique_ptr<IClip>> clips;
    std::vector<TransitionConfig>       trs;
    std::vector<std::string>            paths;
    for (int i = 0; i < n; ++i) {
        clips.push_back(std::make_unique<StubClip>(dur));
        trs.push_back(hardCut());
        paths.push_back("/cache/clip" + std::to_string(i) + ".mp4");
    }
    tl.setPlaylist(std::move(clips), std::move(trs), std::move(paths));
}

}  // namespace

TEST(TimelineHotMutation, SnapshotReturnsStableView) {
    Timeline tl;
    seed(tl, 3);

    auto snap_before = tl.snapshot();
    EXPECT_EQ(snap_before->clips.size(), 3u);

    tl.appendClip(std::make_shared<StubClip>(5.0), hardCut(), "/cache/added.mp4");

    EXPECT_EQ(snap_before->clips.size(), 3u);  // old snapshot still valid
    EXPECT_EQ(tl.snapshot()->clips.size(), 4u);
}

TEST(TimelineHotMutation, AppendClipExtendsPlaylist) {
    Timeline tl;
    seed(tl, 2);
    EXPECT_EQ(tl.getPlaylistSize(), 2);

    tl.appendClip(std::make_shared<StubClip>(7.0), hardCut(), "/cache/new.mp4");
    EXPECT_EQ(tl.getPlaylistSize(), 3);

    auto snap = tl.snapshot();
    EXPECT_EQ(snap->cache_paths[2], "/cache/new.mp4");
    EXPECT_FALSE(snap->pending_remove[2]);
    EXPECT_EQ(snap->clips[2]->getDuration(), 7.0);
}

TEST(TimelineHotMutation, MarkForRemovalSetsPendingFlag) {
    Timeline tl;
    seed(tl, 3);

    EXPECT_TRUE(tl.markForRemoval("/cache/clip1.mp4"));
    auto snap = tl.snapshot();
    EXPECT_FALSE(snap->pending_remove[0]);
    EXPECT_TRUE (snap->pending_remove[1]);
    EXPECT_FALSE(snap->pending_remove[2]);
    EXPECT_EQ(snap->clips.size(), 3u);  // not yet removed
}

TEST(TimelineHotMutation, MarkForRemovalUnknownPathReturnsFalse) {
    Timeline tl;
    seed(tl, 2);
    EXPECT_FALSE(tl.markForRemoval("/cache/nonexistent.mp4"));
    EXPECT_FALSE(tl.markForRemoval(""));  // empty path rejected
}

TEST(TimelineHotMutation, MarkForRemovalIdempotent) {
    Timeline tl;
    seed(tl, 2);
    EXPECT_TRUE (tl.markForRemoval("/cache/clip0.mp4"));
    EXPECT_FALSE(tl.markForRemoval("/cache/clip0.mp4"));  // already marked
}

TEST(TimelineHotMutation, ReapRemovesPendingButPreservesActive) {
    Timeline tl;
    seed(tl, 4);
    tl.markForRemoval("/cache/clip0.mp4");
    tl.markForRemoval("/cache/clip2.mp4");

    // clip0 is "playing" — must NOT be evicted even though pending.
    auto evicted = tl.reapRemovable("/cache/clip0.mp4");
    ASSERT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], "/cache/clip2.mp4");

    auto snap = tl.snapshot();
    EXPECT_EQ(snap->clips.size(), 3u);
    EXPECT_EQ(snap->cache_paths[0], "/cache/clip0.mp4");
    EXPECT_TRUE(snap->pending_remove[0]);  // still pending — will reap later
}

TEST(TimelineHotMutation, ReapWithNoPendingIsNoOp) {
    Timeline tl;
    seed(tl, 3);
    auto before = tl.snapshot();
    auto evicted = tl.reapRemovable("");
    EXPECT_TRUE(evicted.empty());
    // No mutation = same shared_ptr.
    EXPECT_EQ(tl.snapshot().get(), before.get());
}

TEST(TimelineHotMutation, ReapPreservesActivePendingUntilReapPendingActive) {
    // reapRemovable always preserves the active entry; the active pending
    // is dropped explicitly via reapPendingActive() after the fallback
    // crossfade completes, and that eviction is drained on the next
    // reapRemovable().
    Timeline tl;
    seed(tl, 1, /*dur=*/5.0);
    tl.markForRemoval("/cache/clip0.mp4");

    // Active is preserved — playlist stays intact.
    auto evicted1 = tl.reapRemovable("/cache/clip0.mp4");
    EXPECT_TRUE(evicted1.empty());
    EXPECT_EQ(tl.getPlaylistSize(), 1);

    // Preloader-driven drop after the final cycle.
    EXPECT_TRUE(tl.reapPendingActive());
    EXPECT_EQ(tl.getPlaylistSize(), 0);

    // Eviction surfaces on the next reapRemovable so ContentSync can clean
    // the cache disk.
    auto evicted2 = tl.reapRemovable("");
    EXPECT_EQ(evicted2.size(), 1u);
    EXPECT_EQ(evicted2[0], "/cache/clip0.mp4");
}

TEST(TimelineHotMutation, ReapAllWhenActiveEmpty) {
    Timeline tl;
    seed(tl, 3);
    tl.markForRemoval("/cache/clip0.mp4");
    tl.markForRemoval("/cache/clip1.mp4");
    tl.markForRemoval("/cache/clip2.mp4");

    auto evicted = tl.reapRemovable("");
    EXPECT_EQ(evicted.size(), 3u);
    EXPECT_EQ(tl.getPlaylistSize(), 0);
}

TEST(TimelineHotMutation, ConcurrentReadersAndWritersDoNotTear) {
    Timeline tl;
    seed(tl, 4);

    std::atomic<bool> stop{false};
    std::atomic<int>  reads{0};

    // 4 readers continuously snapshot + iterate.
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = tl.snapshot();
                // Touch all parallel arrays — would crash on torn snapshot.
                for (size_t i = 0; i < snap->clips.size(); ++i) {
                    (void)snap->clips[i]->getDuration();
                    (void)snap->cache_paths[i].size();
                    (void)snap->pending_remove[i];
                }
                ++reads;
            }
        });
    }

    // Single writer: append + mark + reap, repeatedly.
    std::thread writer([&] {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            const auto path = "/cache/hot" + std::to_string(i) + ".mp4";
            tl.appendClip(std::make_shared<StubClip>(2.0), hardCut(), path);
            tl.markForRemoval(path);
            tl.reapRemovable("");
        }
    });

    writer.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    EXPECT_GT(reads.load(), 0);
    EXPECT_GE(tl.getPlaylistSize(), 4);  // original 4 still there
}

TEST(TimelineHotMutation, GetStateUsesLatestSnapshot) {
    Timeline tl;
    seed(tl, 2, /*dur=*/4.0);   // 2 clips × 4s = 8s total

    auto st0 = tl.getState(0.5);
    ASSERT_NE(st0.clipA, nullptr);
    auto* original_first = st0.clipA;

    // Append a third clip — totals/indexing change in a fresh snapshot.
    tl.appendClip(std::make_shared<StubClip>(4.0), hardCut(), "/cache/extra.mp4");
    EXPECT_EQ(tl.getPlaylistSize(), 3);

    // At t=0.5 in the new snapshot, still on clip 0 (first 4s).
    auto st1 = tl.getState(0.5);
    EXPECT_EQ(st1.clipA, original_first);
}

// ─── Cursor (stateful API) hot-mutation tests ────────────────────────────────
// The fmod-based legacy API can teleport the active clip when the playlist
// total length changes. The cursor must NOT — it anchors on clip identity.

TEST(TimelineHotMutationCursor, AppendDoesNotJumpActiveClip) {
    Timeline tl;
    seed(tl, 1, /*dur=*/10.0);
    auto* original = tl.snapshot()->clips[0].get();

    tl.advance(3.0);  // 3s into clip 0
    EXPECT_EQ(tl.peek().clipA, original);
    EXPECT_NEAR(tl.getRemainingTime(), 7.0, 1e-9);

    // Append — total goes from 10 → 20. Legacy fmod would relocate.
    tl.appendClip(std::make_shared<StubClip>(10.0), hardCut(), "/cache/added.mp4");

    // Cursor stays on the same clip with the same intra-slot position.
    EXPECT_EQ(tl.peek().clipA, original);
    EXPECT_EQ(tl.getActiveIndex(), 0);
    EXPECT_NEAR(tl.getRemainingTime(), 7.0, 1e-9);
}

TEST(TimelineHotMutationCursor, ReapOfNonActivePreservesCursor) {
    Timeline tl;
    seed(tl, 3, /*dur=*/10.0);
    auto snap0 = tl.snapshot();
    auto* clip1 = snap0->clips[1].get();

    // Roll the cursor onto clip 1 (slot 0 is 10s long with HardCut).
    tl.advance(12.0);
    EXPECT_EQ(tl.peek().clipA, clip1);
    EXPECT_EQ(tl.getActiveIndex(), 1);

    // Mark + reap clip 0 (not active). Indices shift: clip1 becomes index 0.
    EXPECT_TRUE(tl.markForRemoval("/cache/clip0.mp4"));
    auto evicted = tl.reapRemovable("/cache/clip1.mp4");
    EXPECT_EQ(evicted.size(), 1u);

    // Cursor still on clip1 — same pointer, same intra-slot position.
    EXPECT_EQ(tl.peek().clipA, clip1);
    EXPECT_EQ(tl.getActiveIndex(), 0);   // index shifted, identity didn't
    EXPECT_NEAR(tl.getRemainingTime(), 8.0, 1e-9);
}

TEST(TimelineHotMutationCursor, ReapOfActiveHealsToHead) {
    Timeline tl;
    seed(tl, 3, /*dur=*/10.0);
    auto snap0 = tl.snapshot();
    auto* clip0 = snap0->clips[0].get();
    auto* clip1 = snap0->clips[1].get();

    tl.advance(12.0);  // cursor on clip 1
    EXPECT_EQ(tl.peek().clipA, clip1);

    // Reap the active clip (clip1). Use override → effective_active="" so it
    // gets dropped. Cursor heals to the new clips[0] = clip0 (still present).
    tl.markForRemoval("/cache/clip1.mp4");
    tl.reapRemovable("");

    // Peek: soft-heal returns clip0 (new head), cursor not committed yet.
    EXPECT_EQ(tl.peek().clipA, clip0);
    // advance() commits the heal: pos resets to 0.
    tl.advance(1.0);
    EXPECT_EQ(tl.peek().clipA, clip0);
    EXPECT_EQ(tl.getActiveIndex(), 0);
    EXPECT_NEAR(tl.getRemainingTime(), 9.0, 1e-9);
}

TEST(TimelineHotMutationCursor, DrainAllToEmptyFallsBackOnPeek) {
    Timeline tl;
    auto fb = std::make_unique<StubClip>(1.0);
    auto* fb_raw = fb.get();
    tl.setFallback(std::move(fb));

    seed(tl, 2, /*dur=*/5.0);
    tl.advance(2.0);
    ASSERT_NE(tl.peek().clipA, fb_raw);

    // Drain everything: non-active first, then active via reapPendingActive.
    tl.markForRemoval("/cache/clip0.mp4");
    tl.markForRemoval("/cache/clip1.mp4");
    tl.reapRemovable("/cache/clip0.mp4");
    tl.reapPendingActive();
    EXPECT_EQ(tl.getPlaylistSize(), 0);

    // Empty playlist → fallback.
    EXPECT_EQ(tl.peek().clipA, fb_raw);
    EXPECT_EQ(tl.getActiveIndex(), -1);
    EXPECT_DOUBLE_EQ(tl.getRemainingTime(), 0.0);
}

TEST(TimelineHotMutationCursor, AdvanceRollsAcrossMultipleSlots) {
    Timeline tl;
    seed(tl, 3, /*dur=*/2.0);
    auto snap = tl.snapshot();

    // Big jump that spans 2 full slots (4s) and lands 0.5s into slot 2.
    tl.advance(4.5);
    EXPECT_EQ(tl.peek().clipA, snap->clips[2].get());
    EXPECT_NEAR(tl.getRemainingTime(), 1.5, 1e-9);
}

TEST(TimelineHotMutationCursor, SkipToNextAdvancesCursor) {
    Timeline tl;
    seed(tl, 3, /*dur=*/5.0);
    auto snap = tl.snapshot();

    tl.advance(2.0);
    EXPECT_EQ(tl.getActiveIndex(), 0);

    tl.skipToNext();
    EXPECT_EQ(tl.getActiveIndex(), 1);
    EXPECT_EQ(tl.peek().clipA, snap->clips[1].get());
    EXPECT_NEAR(tl.getRemainingTime(), 5.0, 1e-9);

    tl.skipToNext();
    tl.skipToNext();  // wraps 2→0
    EXPECT_EQ(tl.getActiveIndex(), 0);
    EXPECT_EQ(tl.peek().clipA, snap->clips[0].get());
}

TEST(TimelineHotMutationCursor, SkipToNextOnEmptyIsNoOp) {
    Timeline tl;
    tl.skipToNext();  // must not crash
    EXPECT_EQ(tl.getActiveIndex(), -1);
}

// ─── removeAt(idx) — Stage 3.2 ───────────────────────────────────────────────

TEST(TimelineRemoveAt, OutOfRangeReturnsNotFound) {
    Timeline tl;
    seed(tl, 2);
    EXPECT_EQ(tl.removeAt(2), Timeline::RemoveResult::NotFound);
    EXPECT_EQ(tl.removeAt(99), Timeline::RemoveResult::NotFound);
    EXPECT_EQ(tl.getPlaylistSize(), 2);
}

TEST(TimelineRemoveAt, NonActiveRemovedPhysically) {
    Timeline tl;
    seed(tl, 3);
    // Cursor never advanced — no active_clip_, so any idx is "non-active".
    EXPECT_EQ(tl.removeAt(1), Timeline::RemoveResult::Removed);
    EXPECT_EQ(tl.getPlaylistSize(), 2);

    auto snap = tl.snapshot();
    EXPECT_EQ(snap->cache_paths[0], "/cache/clip0.mp4");
    EXPECT_EQ(snap->cache_paths[1], "/cache/clip2.mp4");
}

TEST(TimelineRemoveAt, ActiveMarksPendingWithoutPhysicalRemoval) {
    Timeline tl;
    seed(tl, 3, /*dur=*/5.0);
    tl.advance(0.0);  // commits cursor onto clip[0]
    EXPECT_EQ(tl.getActiveIndex(), 0);

    EXPECT_EQ(tl.removeAt(0), Timeline::RemoveResult::MarkedActive);
    EXPECT_EQ(tl.getPlaylistSize(), 3);
    EXPECT_TRUE(tl.snapshot()->pending_remove[0]);

    // Calling again returns MarkedActive idempotently.
    EXPECT_EQ(tl.removeAt(0), Timeline::RemoveResult::MarkedActive);

    // Preloader-style drain swaps fallback when the active wraps.
    EXPECT_TRUE(tl.reapPendingActive());
    EXPECT_EQ(tl.getPlaylistSize(), 2);
}

TEST(TimelineRemoveAt, NonActiveAfterCursorAdvance) {
    Timeline tl;
    seed(tl, 3, /*dur=*/5.0);
    tl.advance(0.0);  // active = clip0

    // Removing clip2 while clip0 is active — physical removal.
    EXPECT_EQ(tl.removeAt(2), Timeline::RemoveResult::Removed);
    EXPECT_EQ(tl.getPlaylistSize(), 2);
    EXPECT_EQ(tl.getActiveIndex(), 0);  // cursor unaffected
}

