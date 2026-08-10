#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "clips/IClip.h"
#include "core/Timeline.h"

namespace {

// fix13 c5 — Timeline::setPrepareCallback firing rules.
//
// The callback is the hook ChannelInstance uses to drive a LiveClip's
// warm-up window: Timeline notifies "the upcoming clip just entered
// its warm-up zone, here's how long until the cut", and the channel
// kicks off the slow open (RTMP handshake / multicast IGMP join) on a
// thread pool. We pin firing semantics here without needing a real
// LiveClip — a tiny FakeClip with a configurable warmUpSec() is
// enough to drive every branch.

class FakeClip : public IClip {
public:
    explicit FakeClip(double dur, double warm_up = 0.0)
        : dur_(dur), warm_up_(warm_up) {}
    Frame      getFrame() override                      { return {}; }
    AudioFrame getAudio(int) override                   { return {}; }
    double     getDuration() const override             { return dur_; }
    bool       hasAudio() const override                { return false; }
    bool       isPrepared() const override              { return true; }
    void       prepare() override                       {}
    void       release() override                       {}
    Frame      getTailFrame() override                  { return {}; }
    AudioFrame getTailAudio(int) override               { return {}; }
    void       reset() override                         {}
    double     warmUpSec() const noexcept override      { return warm_up_; }
private:
    double dur_;
    double warm_up_;
};

TransitionConfig hardCut() {
    return { TransitionType::HardCut, TransitionMode::HardCut, 0.0 };
}

struct PrepareLog {
    struct Entry { IClip* clip; double seconds_until_start; };
    std::vector<Entry> entries;

    Timeline::PrepareCallback callback() {
        return [this](std::shared_ptr<IClip> next, double secs) {
            entries.push_back({next.get(), secs});
        };
    }
};

void setSimplePlaylist(Timeline& t,
                       std::vector<std::unique_ptr<IClip>> clips) {
    std::vector<TransitionConfig> trs(clips.size(), hardCut());
    t.setPlaylist(std::move(clips), std::move(trs));
}

// ── Default behavior: no warm-up, no callback ──────────────────────────

TEST(TimelinePrepare, NoCallbackByDefault) {
    Timeline t;
    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::make_unique<FakeClip>(10.0));
    clips.push_back(std::make_unique<FakeClip>(10.0));
    setSimplePlaylist(t, std::move(clips));
    t.advance(9.5);   // doesn't crash, no observers
    SUCCEED();
}

TEST(TimelinePrepare, FileClipWithZeroWarmUpNeverFires) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::make_unique<FakeClip>(10.0));
    clips.push_back(std::make_unique<FakeClip>(10.0));   // warm_up=0
    setSimplePlaylist(t, std::move(clips));
    t.advance(9.99);
    EXPECT_TRUE(log.entries.empty());
}

// ── Firing rules ───────────────────────────────────────────────────────

TEST(TimelinePrepare, FiresWhenRemainingDropsBelowWarmUp) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    auto first  = std::make_unique<FakeClip>(10.0);
    auto second = std::make_unique<FakeClip>(60.0, /*warm_up=*/3.0);
    IClip* second_raw = second.get();

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(first));
    clips.push_back(std::move(second));
    setSimplePlaylist(t, std::move(clips));

    // 6.5s in, remaining = 3.5s — outside warm-up window
    t.advance(6.5);
    EXPECT_TRUE(log.entries.empty());

    // Step to 7.5s in, remaining = 2.5s ≤ 3s warm-up
    t.advance(1.0);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].clip, second_raw);
    EXPECT_NEAR(log.entries[0].seconds_until_start, 2.5, 1e-9);
}

TEST(TimelinePrepare, FiresOnceUntilSlotCrosses) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::make_unique<FakeClip>(10.0));
    clips.push_back(std::make_unique<FakeClip>(20.0, /*warm_up=*/5.0));
    setSimplePlaylist(t, std::move(clips));

    // Drive deep inside the warm window with multiple advance() ticks
    t.advance(6.0);  // remaining=4 → fires
    t.advance(1.0);  // remaining=3 → must NOT fire again
    t.advance(1.0);  // remaining=2 → must NOT fire again
    t.advance(1.0);  // remaining=1 → must NOT fire again
    EXPECT_EQ(log.entries.size(), 1u);
}

TEST(TimelinePrepare, RefiresAfterSlotRollForNewUpcomingClip) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    auto a = std::make_unique<FakeClip>(10.0, /*warm_up=*/3.0);
    auto b = std::make_unique<FakeClip>(10.0, /*warm_up=*/3.0);
    IClip* a_raw = a.get();
    IClip* b_raw = b.get();
    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::move(a));
    clips.push_back(std::move(b));
    setSimplePlaylist(t, std::move(clips));

    // First fire: cursor on A (10s), upcoming = B
    t.advance(8.0);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].clip, b_raw);

    // Cross slot: now on B, upcoming = A (looped)
    t.advance(2.5);     // pos=10.5 → rolls to B, slot_pos=0.5
    // remaining_in_B = 9.5 (>3) — no fire yet
    EXPECT_EQ(log.entries.size(), 1u);

    t.advance(7.0);     // pos in B = 7.5, remaining = 2.5 ≤ 3 → fire A
    ASSERT_EQ(log.entries.size(), 2u);
    EXPECT_EQ(log.entries[1].clip, a_raw);
}

TEST(TimelinePrepare, FiresImmediatelyWhenWarmUpExceedsSlotLength) {
    // Risk #6 in the fix13 plan: hot-add a live-clip whose warm_up is
    // larger than the time remaining before its scheduled start. The
    // callback must fire on the first qualifying advance(), not wait
    // out a window that never opens.
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::make_unique<FakeClip>(2.0));         // short
    clips.push_back(std::make_unique<FakeClip>(60.0, 10.0));  // huge warm-up
    setSimplePlaylist(t, std::move(clips));

    // First tick already lands inside the warm window (remaining=2 ≤ 10)
    t.advance(0.0);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_NEAR(log.entries[0].seconds_until_start, 2.0, 1e-9);
}

TEST(TimelinePrepare, SetPlaylistResetsToken) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());

    {
        std::vector<std::unique_ptr<IClip>> clips;
        clips.push_back(std::make_unique<FakeClip>(10.0));
        clips.push_back(std::make_unique<FakeClip>(60.0, 3.0));
        setSimplePlaylist(t, std::move(clips));
        t.advance(8.0);   // fire #1 — old playlist
    }
    ASSERT_EQ(log.entries.size(), 1u);

    {
        std::vector<std::unique_ptr<IClip>> clips;
        clips.push_back(std::make_unique<FakeClip>(10.0));
        clips.push_back(std::make_unique<FakeClip>(60.0, 3.0));
        setSimplePlaylist(t, std::move(clips));   // identities replaced
        t.advance(8.0);   // fire #2 — new playlist's upcoming
    }
    EXPECT_EQ(log.entries.size(), 2u);
    EXPECT_NE(log.entries[0].clip, log.entries[1].clip);
}

TEST(TimelinePrepare, EmptyPlaylistDoesNotFire) {
    Timeline t;
    PrepareLog log;
    t.setPrepareCallback(log.callback());
    t.advance(1.0);
    EXPECT_TRUE(log.entries.empty());
}

TEST(TimelinePrepare, CallbackMayBeReplacedAtRuntime) {
    Timeline t;
    PrepareLog logA;
    t.setPrepareCallback(logA.callback());

    std::vector<std::unique_ptr<IClip>> clips;
    clips.push_back(std::make_unique<FakeClip>(10.0));
    clips.push_back(std::make_unique<FakeClip>(20.0, 3.0));
    setSimplePlaylist(t, std::move(clips));
    t.advance(8.0);   // fires into logA
    ASSERT_EQ(logA.entries.size(), 1u);

    // Swap callback while the upcoming clip is still pending — same
    // token, no re-fire on logB. Must wait for the next slot crossing.
    PrepareLog logB;
    t.setPrepareCallback(logB.callback());
    t.advance(0.5);
    EXPECT_TRUE(logB.entries.empty());
}

} // namespace
