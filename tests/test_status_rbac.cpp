// fix32 B1 — RBAC filter for /api/status and /api/metrics.
//
// Verifies ChannelScope semantics and that MetricsCollector::renderStatus
// / renderPrometheus filter their per-channel and gateway sections when a
// non-admin scope is supplied.
//
// Pattern:
//   anonymous (scope == nullptr)              → full view
//   admin / operator (show_all == true)       → full view
//   viewer with grants                        → only granted channels
//   viewer without grants                     → channels: []
//
// Cross-channel summaries (channels_total, channels_running, gateways_*)
// are recomputed from the visible subset so a viewer never learns that
// "10 channels exist" when they only have access to two.

#include <algorithm>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/MetricsCollector.h"
#include "api/RbacScope.h"
#include "auth/AuthTypes.h"
#include "metrics/ProcessMetrics.h"

using nlohmann::json;
using R = ChannelManager::Result;
using Role = liveqx::auth::Role;
using ChannelPermission = liveqx::auth::ChannelPermission;

namespace {

json minimalCfg(int id) {
    return json{
        {"id",          id},
        {"name",        "ch" + std::to_string(id)},
        {"resolution", "320x240"},
        {"fps",         25},
        {"bitrate",     1'000'000},
        {"preset",      "ultrafast"},
        {"output",     {{"port", 19500 + id}, {"latency_ms", 200}}},
    };
}

liveqx::auth::RequestContext ctxWithRole(Role r) {
    liveqx::auth::RequestContext ctx;
    ctx.role = r;
    return ctx;
}

liveqx::auth::RequestContext viewerCtx(
    std::initializer_list<std::int64_t> grants) {
    auto ctx = ctxWithRole(Role::Viewer);
    for (auto cid : grants) {
        ctx.channel_grants.push_back(
            {cid, ChannelPermission::View});
    }
    return ctx;
}

}  // namespace

// ── ChannelScope semantics ──────────────────────────────────────────────────

TEST(ChannelScope, AdminScopeShowsAll) {
    auto scope = liveqx::api::makeChannelScope(ctxWithRole(Role::Admin));
    EXPECT_TRUE(scope.show_all);
    EXPECT_TRUE(scope.allows(1));
    EXPECT_TRUE(scope.allows(999));
    EXPECT_TRUE(scope.allowsCrossChannel());
}

TEST(ChannelScope, OperatorScopeShowsAll) {
    auto scope = liveqx::api::makeChannelScope(ctxWithRole(Role::Operator));
    EXPECT_TRUE(scope.show_all);
    EXPECT_TRUE(scope.allows(42));
    EXPECT_TRUE(scope.allowsCrossChannel());
}

TEST(ChannelScope, ViewerWithGrantsAllowsOnlyGranted) {
    auto scope = liveqx::api::makeChannelScope(viewerCtx({3, 1, 7}));
    EXPECT_FALSE(scope.show_all);
    EXPECT_TRUE(scope.allows(1));
    EXPECT_TRUE(scope.allows(3));
    EXPECT_TRUE(scope.allows(7));
    EXPECT_FALSE(scope.allows(2));
    EXPECT_FALSE(scope.allows(0));
    EXPECT_FALSE(scope.allowsCrossChannel());
    // Sorted + deduped for binary_search.
    ASSERT_EQ(scope.allowed_channels.size(), 3u);
    EXPECT_TRUE(std::is_sorted(scope.allowed_channels.begin(),
                               scope.allowed_channels.end()));
}

TEST(ChannelScope, ViewerWithoutGrantsSeesNothing) {
    auto scope = liveqx::api::makeChannelScope(viewerCtx({}));
    EXPECT_FALSE(scope.show_all);
    EXPECT_TRUE(scope.allowed_channels.empty());
    EXPECT_FALSE(scope.allows(1));
    EXPECT_FALSE(scope.allowsCrossChannel());
}

TEST(ChannelScope, ViewerGrantsAreDeduped) {
    liveqx::auth::RequestContext ctx;
    ctx.role = Role::Viewer;
    ctx.channel_grants.push_back({5, ChannelPermission::View});
    ctx.channel_grants.push_back({5, ChannelPermission::Operate});  // dup id
    ctx.channel_grants.push_back({2, ChannelPermission::View});
    auto scope = liveqx::api::makeChannelScope(ctx);
    EXPECT_EQ(scope.allowed_channels.size(), 2u);
    EXPECT_TRUE(scope.allows(2));
    EXPECT_TRUE(scope.allows(5));
}

// ── renderStatus filtering ──────────────────────────────────────────────────

namespace {

struct StatusFixture {
    ChannelManager   mgr{nullptr, {}};
    ProcessMetrics   pm{/*start_unix=*/1234567890};
    BuildInfo        build{"0.32.0", "test", "2026-05-09T00:00:00Z", "Release"};
    MetricsCollector mc{mgr, pm, build};

    StatusFixture() {
        EXPECT_EQ(mgr.create(minimalCfg(1)), R::Ok);
        EXPECT_EQ(mgr.create(minimalCfg(2)), R::Ok);
        EXPECT_EQ(mgr.create(minimalCfg(3)), R::Ok);
    }
};

std::vector<int> idsIn(const json& body) {
    std::vector<int> ids;
    for (const auto& c : body["channels"]) ids.push_back(c["id"].get<int>());
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

TEST(StatusRbac, AnonymousScopeSeesAllChannels) {
    StatusFixture f;
    auto body = f.mc.renderStatus(/*scope=*/nullptr);
    EXPECT_EQ(idsIn(body), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(body["summary"]["channels_total"], 3);
}

TEST(StatusRbac, AdminScopeSeesAllChannels) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(ctxWithRole(Role::Admin));
    auto body = f.mc.renderStatus(&scope);
    EXPECT_EQ(idsIn(body), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(body["summary"]["channels_total"], 3);
}

TEST(StatusRbac, OperatorScopeSeesAllChannels) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(ctxWithRole(Role::Operator));
    auto body = f.mc.renderStatus(&scope);
    EXPECT_EQ(idsIn(body), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(body["summary"]["channels_total"], 3);
}

TEST(StatusRbac, ViewerWithGrantSeesOnlyGrantedChannel) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(viewerCtx({2}));
    auto body = f.mc.renderStatus(&scope);
    EXPECT_EQ(idsIn(body), (std::vector<int>{2}));
    EXPECT_EQ(body["summary"]["channels_total"], 1);
    // Recomputed from the visible subset — must NOT leak the actual total.
    EXPECT_EQ(body["summary"]["channels_running"], 0);
}

TEST(StatusRbac, ViewerWithoutGrantsSeesNoChannels) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(viewerCtx({}));
    auto body = f.mc.renderStatus(&scope);
    ASSERT_TRUE(body["channels"].is_array());
    EXPECT_EQ(body["channels"].size(), 0u);
    EXPECT_EQ(body["summary"]["channels_total"], 0);
}

TEST(StatusRbac, ViewerScopeHidesGatewaysSection) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(viewerCtx({1}));
    auto body = f.mc.renderStatus(&scope);
    ASSERT_TRUE(body["gateways"].is_array());
    EXPECT_EQ(body["gateways"].size(), 0u);
    EXPECT_EQ(body["summary"]["gateways_total"],   0);
    EXPECT_EQ(body["summary"]["gateways_running"], 0);
}

// ── renderPrometheus filtering ──────────────────────────────────────────────

namespace {

bool containsLineWith(const std::string& exposition,
                      const std::string& needle) {
    return exposition.find(needle) != std::string::npos;
}

}  // namespace

TEST(MetricsRbac, AnonymousScopeEmitsAllChannelLines) {
    StatusFixture f;
    const auto body = f.mc.renderPrometheus(/*scope=*/nullptr);
    EXPECT_TRUE(containsLineWith(body, "channel_name=\"ch1\""));
    EXPECT_TRUE(containsLineWith(body, "channel_name=\"ch2\""));
    EXPECT_TRUE(containsLineWith(body, "channel_name=\"ch3\""));
    EXPECT_TRUE(containsLineWith(body, "liveqx_channels_total 3"));
}

TEST(MetricsRbac, ViewerScopeEmitsOnlyGrantedChannel) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(viewerCtx({2}));
    const auto body = f.mc.renderPrometheus(&scope);
    EXPECT_FALSE(containsLineWith(body, "channel_name=\"ch1\""));
    EXPECT_TRUE (containsLineWith(body, "channel_name=\"ch2\""));
    EXPECT_FALSE(containsLineWith(body, "channel_name=\"ch3\""));
    // Cross-channel summary recomputed from filtered subset.
    EXPECT_TRUE(containsLineWith(body, "liveqx_channels_total 1"));
}

TEST(MetricsRbac, ViewerWithoutGrantsEmitsNoChannelLines) {
    StatusFixture f;
    auto scope = liveqx::api::makeChannelScope(viewerCtx({}));
    const auto body = f.mc.renderPrometheus(&scope);
    EXPECT_FALSE(containsLineWith(body, "channel_name=\"ch1\""));
    EXPECT_FALSE(containsLineWith(body, "channel_name=\"ch2\""));
    EXPECT_FALSE(containsLineWith(body, "channel_name=\"ch3\""));
    EXPECT_TRUE (containsLineWith(body, "liveqx_channels_total 0"));
}
