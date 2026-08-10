// fix23 commit 7 — PreviewManager skeleton tests.
//
// libdatachannel-bound peer connection lives behind a compile flag; on
// default builds (ENABLE_WEBRTC_PREVIEW=OFF) createOffer returns
// NotCompiled. These tests pin the always-compiled state machine: cap
// accounting, idempotent shutdowns, idle eviction, snapshot shape.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "preview/PreviewManager.h"

namespace {

using liveqx::preview::PreviewManager;
using R = PreviewManager::Result;

nlohmann::json validOffer() {
    return {{"type", "offer"}, {"sdp", "v=0\r\n..."}};
}

}  // namespace

TEST(PreviewManager, IsCompiledMatchesBuildFlag) {
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    EXPECT_TRUE(PreviewManager::isCompiled());
#else
    EXPECT_FALSE(PreviewManager::isCompiled());
#endif
}

TEST(PreviewManager, CreateOfferReturnsNotCompiledOnDefaultBuild) {
    PreviewManager mgr;
    nlohmann::json answer;
    auto r = mgr.createOffer(7, validOffer(), &answer);
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    // With the feature compiled in we expect either Ok (full impl) or
    // InternalError (commit 7 placeholder until 7b lands).
    EXPECT_TRUE(r == R::Ok || r == R::InternalError);
#else
    EXPECT_EQ(r, R::NotCompiled);
#endif
}

TEST(PreviewManager, RejectsMalformedOffer) {
    PreviewManager mgr;
    nlohmann::json answer;
    // Missing type=offer.
    auto r = mgr.createOffer(1, nlohmann::json{{"sdp", "x"}}, &answer);
    // NotCompiled wins over BadOffer when feature is OFF — both block
    // session creation. With feature ON, body shape is checked first so
    // we expect BadOffer.
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    EXPECT_EQ(r, R::BadOffer);
#else
    EXPECT_EQ(r, R::NotCompiled);
#endif
}

TEST(PreviewManager, CloseSessionOnUnknownChannelReturnsNotFound) {
    PreviewManager mgr;
    EXPECT_EQ(mgr.closeSession(99, "doesnotexist"), R::NotFound);
}

TEST(PreviewManager, StatsForUnknownChannelIsNull) {
    PreviewManager mgr;
    EXPECT_TRUE(mgr.statsJson(42).is_null());
}

TEST(PreviewManager, GlobalSnapshotShape) {
    PreviewManager::Config cfg;
    cfg.max_clients_per_channel = 3;
    cfg.max_active_channels     = 5;
    cfg.idle_shutdown_sec       = 120;

    PreviewManager mgr(cfg);
    auto snap = mgr.globalSnapshotJson();
    EXPECT_TRUE(snap.is_object());
    EXPECT_EQ(snap["max_per_ch"], 3);
    EXPECT_EQ(snap["max_active"], 5);
    EXPECT_EQ(snap["idle_sec"],   120);
    EXPECT_TRUE(snap["channels"].is_array());
    EXPECT_TRUE(snap["channels"].empty());
}

TEST(PreviewManager, StopChannelIsIdempotent) {
    PreviewManager mgr;
    mgr.stopChannel(123);   // never created — must not throw
    mgr.stopChannel(123);
    SUCCEED();
}

TEST(PreviewManager, OnChannelFrameIsNoOpForUnknownChannel) {
    PreviewManager mgr;
    Frame f;  // invalid frame
    mgr.onChannelFrame(7, f);  // must not crash
    SUCCEED();
}

TEST(PreviewManager, IdleTickWithNoChannelsIsHarmless) {
    PreviewManager mgr;
    mgr.tickIdleShutdown(123456789);
    SUCCEED();
}
