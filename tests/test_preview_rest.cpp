// fix23 commit 9 — REST surface for the WebRTC preview manager.
//
// We exercise the route handlers through a real httplib stack so the
// regex matchers, body parsing, and PreviewManager.Result→HTTP mapping
// are pinned. RBAC is intentionally not wired here — the route logic is
// what we're testing, and Auth/RBAC integration lives in
// test_rbac_rest.cpp.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "preview/PreviewManager.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;
using PR = liveqx::preview::PreviewManager::Result;

constexpr int kNoPreviewPort   = 18301;
constexpr int kOfferPort       = 18302;
constexpr int kStatsPort       = 18303;
constexpr int kCloseSessPort   = 18304;
constexpr int kSnapshotPort    = 18305;

json minimalCfg(int id) {
    return {
        {"id",         id},
        {"name",       "preview-rest-test"},
        {"resolution", "320x240"},
        {"fps",        25},
        {"bitrate",    1'000'000},
        {"preset",     "ultrafast"},
        {"output",     {{"port", 49600 + id}, {"latency_ms", 200}}},
    };
}

void waitListening(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto r = cli.Get("/healthz");
        if (r && r->status == 200) return;
        std::this_thread::sleep_for(20ms);
    }
    FAIL() << "ControlApi never started on port " << port;
}

// Owns a ChannelManager, a PreviewManager (optional), and ControlApi.
struct PreviewApiFixture {
    ChannelManager                          manager;
    liveqx::preview::PreviewManager preview;
    ControlApi                              api;

    PreviewApiFixture(int port, bool wire_preview)
        : manager(nullptr, {}),
          preview(),
          api(port, manager, /*metrics=*/nullptr, /*livez=*/{},
              /*gateways=*/nullptr, /*auth=*/nullptr,
              /*ldap=*/nullptr, /*smtp=*/nullptr,
              /*rbac=*/nullptr, /*events=*/nullptr,
              wire_preview ? &preview : nullptr) {
        api.start();
        waitListening(port);
    }
    ~PreviewApiFixture() { api.stop(); }
};

}  // namespace

// ── 503 path: PreviewManager not wired ─────────────────────────────────
TEST(PreviewRest, NoPreviewWiredReturns503OnEveryRoute) {
    PreviewApiFixture f(kNoPreviewPort, /*wire_preview=*/false);
    httplib::Client cli("127.0.0.1", kNoPreviewPort);

    for (const char* path : {"/api/channels/1/preview/offer"}) {
        auto r = cli.Post(path, R"({"sdp":"x","type":"offer"})", "application/json");
        ASSERT_TRUE(r) << path;
        EXPECT_EQ(r->status, 503) << path;
        auto body = json::parse(r->body);
        EXPECT_EQ(body["error"], "preview_not_configured");
    }
    {
        auto r = cli.Get("/api/channels/1/preview/stats");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 503);
    }
    {
        auto r = cli.Delete("/api/channels/1/preview/sessions/abc123");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 503);
    }
    {
        auto r = cli.Get("/api/preview");
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 503);
    }
}

// ── Offer path: 404 on unknown channel, body validation, compile gate ──
TEST(PreviewRest, OfferOnUnknownChannelReturns404) {
    PreviewApiFixture f(kOfferPort, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kOfferPort);

    // No channel exists yet.
    auto r = cli.Post("/api/channels/77/preview/offer",
                      R"({"sdp":"v=0","type":"offer"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(PreviewRest, OfferRejectsInvalidJson) {
    PreviewApiFixture f(kOfferPort + 10, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kOfferPort + 10);

    int id = 0;
    ASSERT_EQ(f.manager.create(minimalCfg(11), &id),
              ChannelManager::Result::Ok);

    auto r = cli.Post("/api/channels/11/preview/offer",
                      "not valid json", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_json");
}

TEST(PreviewRest, OfferOnDefaultBuildOrBadOffer) {
    PreviewApiFixture f(kOfferPort + 20, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kOfferPort + 20);

    int id = 0;
    ASSERT_EQ(f.manager.create(minimalCfg(12), &id),
              ChannelManager::Result::Ok);

    // Missing sdp/type.
    auto r = cli.Post("/api/channels/12/preview/offer",
                      R"({})", "application/json");
    ASSERT_TRUE(r);
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "bad_offer");
#else
    // NotCompiled wins regardless of body shape on default builds.
    EXPECT_EQ(r->status, 501);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "preview_not_compiled");
#endif
}

// ── Stats path ─────────────────────────────────────────────────────────
TEST(PreviewRest, StatsOnUnknownChannelReturns404) {
    PreviewApiFixture f(kStatsPort, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kStatsPort);

    auto r = cli.Get("/api/channels/999/preview/stats");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(PreviewRest, StatsForExistingChannelWithoutPreviewReturnsEmptyShape) {
    PreviewApiFixture f(kStatsPort + 10, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kStatsPort + 10);

    int id = 0;
    ASSERT_EQ(f.manager.create(minimalCfg(13), &id),
              ChannelManager::Result::Ok);

    auto r = cli.Get("/api/channels/13/preview/stats");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["channel_id"], 13);
    EXPECT_TRUE(body["encoder"].is_null());
    EXPECT_TRUE(body["sessions"].is_array());
    EXPECT_EQ(body["sessions"].size(), 0u);
}

// ── Close-session path ────────────────────────────────────────────────
TEST(PreviewRest, CloseSessionOnUnknownChannelReturns404) {
    PreviewApiFixture f(kCloseSessPort, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kCloseSessPort);

    auto r = cli.Delete("/api/channels/42/preview/sessions/abc");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(PreviewRest, CloseSessionUnknownSessionForKnownChannelReturns404) {
    PreviewApiFixture f(kCloseSessPort + 10, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kCloseSessPort + 10);

    int id = 0;
    ASSERT_EQ(f.manager.create(minimalCfg(14), &id),
              ChannelManager::Result::Ok);

    auto r = cli.Delete("/api/channels/14/preview/sessions/nosuch");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "not_found");
}

// ── Global snapshot ───────────────────────────────────────────────────
TEST(PreviewRest, GlobalSnapshotEcoesConfigAndIsAdminScoped) {
    PreviewApiFixture f(kSnapshotPort, /*wire_preview=*/true);
    httplib::Client cli("127.0.0.1", kSnapshotPort);

    auto r = cli.Get("/api/preview");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.contains("compiled"));
    EXPECT_TRUE(body.contains("max_per_ch"));
    EXPECT_TRUE(body.contains("max_active"));
    EXPECT_TRUE(body.contains("idle_sec"));
    ASSERT_TRUE(body["channels"].is_array());
    EXPECT_EQ(body["channels"].size(), 0u);
}
