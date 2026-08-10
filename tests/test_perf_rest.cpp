// fix26 commit 5 — REST integration tests for /api/channels/{id}/perf/*.
//
// Endpoints exercised:
//   GET  /api/channels/{id}/perf
//   POST /api/channels/{id}/perf/start  (?mode=...&reset=true | JSON body)
//   POST /api/channels/{id}/perf/stop
//
// Coverage focuses on REST contract (status codes, error shape) and the
// two ChannelManager states reachable without a running RenderLoop:
//   - unknown id              ⇒ 404
//   - existing, not playing   ⇒ 200 mode=off (GET) / 409 AlreadyStopped (POST)
//
// Running-channel paths are covered by ChannelProfiler/ProfileSampler unit
// tests; spinning up a real RenderLoop here would just duplicate those.

#include <atomic>
#include <chrono>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kPerfPortBase = 19700;

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kPerfPortBase + (static_cast<int>(getpid()) % 600) * 5;
    return base + n.fetch_add(1);
}

json minimalCfg(int id) {
    return json{
        {"id", id},
        {"name", "perf-rest-test"},
        {"resolution", "320x240"},
        {"fps", 25},
        {"bitrate", 1'000'000},
        {"preset", "ultrafast"},
        {"output", {{"port", 49500}, {"latency_ms", 200}}},
    };
}

struct PerfFixture {
    ChannelManager manager;
    ControlApi     api;
    int            port;

    explicit PerfFixture(int p)
        : manager(nullptr),
          api(p, manager),
          port(p) {
        api.start();
        waitListening(port);
    }

    ~PerfFixture() { api.stop(); }

    static void waitListening(int port) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        FAIL() << "ControlApi never started on port " << port;
    }

    httplib::Client client() {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(5, 0);
        return c;
    }
};

}  // namespace

TEST(PerfRest, GetUnknownChannelReturns404) {
    PerfFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/channels/9999/perf");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404) << r->body;
}

TEST(PerfRest, GetStoppedChannelReturnsModeOff) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(7)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Get("/api/channels/7/perf");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    EXPECT_EQ(body["mode"],       "off");
    EXPECT_EQ(body["running"],    false);
    EXPECT_EQ(body["channel_id"], 7);
}

TEST(PerfRest, PostStartUnknownChannelReturns404) {
    PerfFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/channels/9999/perf/start?mode=sampling", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404) << r->body;
}

TEST(PerfRest, PostStopUnknownChannelReturns404) {
    PerfFixture f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/channels/9999/perf/stop", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404) << r->body;
}

TEST(PerfRest, PostStartWithoutModeReturns400) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(11)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Post("/api/channels/11/perf/start", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
    auto body = json::parse(r->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST(PerfRest, PostStartWithGarbageModeReturns400) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(12)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Post("/api/channels/12/perf/start?mode=lolnope", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
}

TEST(PerfRest, PostStartWithModeOffReturns400) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(13)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Post("/api/channels/13/perf/start?mode=off", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
}

TEST(PerfRest, PostStartOnStoppedChannelReturns409) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(14)), ChannelManager::Result::Ok);
    auto cli = f.client();
    // Channel exists but is not running ⇒ no profiler ⇒ AlreadyStopped (409).
    auto r = cli.Post("/api/channels/14/perf/start?mode=sampling", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409) << r->body;
}

TEST(PerfRest, PostStopOnStoppedChannelReturns409) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(15)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Post("/api/channels/15/perf/stop", "", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409) << r->body;
}

TEST(PerfRest, PostStartReadsModeFromJsonBody) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(16)), ChannelManager::Result::Ok);
    auto cli = f.client();
    // Body provides mode; channel is not running so we still get 409, but
    // the body parser must reach profilerStart (i.e. not bail on 400).
    auto r = cli.Post("/api/channels/16/perf/start",
                      json({{"mode", "instrumentation"}}).dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409) << r->body;
}

TEST(PerfRest, PostStartWithMalformedJsonReturns400) {
    PerfFixture f(nextPort());
    ASSERT_EQ(f.manager.create(minimalCfg(17)), ChannelManager::Result::Ok);
    auto cli = f.client();
    auto r = cli.Post("/api/channels/17/perf/start",
                      "{this is not json", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
}
