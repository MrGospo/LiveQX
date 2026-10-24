// The cap on long-lived responses (event, log and host-metrics streams).
//
// A stream holds a worker thread of the HTTP server for as long as it is open. Found by loading the
// server: with about a dozen streams open, every other request — the health check and the login
// among them — waited for a worker for ever. One browser tab opens up to four streams.
//
// What these tests protect:
//   * the limiter counts, gives places back, and judges the user and the server separately;
//   * with every stream the server allows open, an ordinary request is still answered at once;
//   * a stream over the cap is refused with 429 and a Retry-After, never left waiting;
//   * a place freed by a closed stream can be taken again.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "api/StreamLimiter.h"
#include "events/EventBus.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;
using liveqx::api::StreamLimiter;

constexpr int kCapPort = 18097;

TEST(StreamLimiter, CountsPlacesGivesThemBackAndKeepsUsersApart) {
    StreamLimiter lim(4, 2);
    auto a1 = lim.acquire(1);
    auto a2 = lim.acquire(1);
    EXPECT_TRUE(a1 && a2);
    EXPECT_FALSE(lim.acquire(1)) << "a user is held to their own cap";
    auto b1 = lim.acquire(2);
    auto b2 = lim.acquire(2);
    EXPECT_TRUE(b1 && b2) << "another user is not affected by the first";
    EXPECT_EQ(lim.active(), 4);
    EXPECT_FALSE(lim.acquire(3)) << "the server's total is held too, whoever asks";

    a1.reset();
    EXPECT_EQ(lim.active(), 3);
    auto c1 = lim.acquire(3);
    EXPECT_TRUE(c1) << "a place given back is a place again";
    EXPECT_FALSE(lim.acquire(1)) << "the server is full again, though user 1 holds only one place";
    a2.reset();
    EXPECT_TRUE(lim.acquire(1)) << "and when user 1 gives theirs back there is room for them";
}

TEST(StreamLimiter, APlaceMayOutliveTheLimiterAndTheCapMayBeChangedWhileStreamsAreOpen) {
    std::shared_ptr<void> held;
    {
        StreamLimiter lim(3, 3);
        held = lim.acquire(7);
        ASSERT_TRUE(held);
        lim.configure(1, 1);                       // lower than what is open: nothing is cut
        EXPECT_FALSE(lim.acquire(8)) << "only new ones are judged";
        EXPECT_EQ(lim.active(), 1);
    }
    held.reset();                                  // the limiter is gone; releasing must not touch freed memory
    SUCCEED();
}

// An open stream as a browser tab holds it: the response is read until the test closes the socket.
struct OpenStream {
    std::shared_ptr<httplib::Client> cli;
    std::thread                      thread;
    std::atomic<int>                 status{0};
    std::string                      retry_after;

    OpenStream(int port, const std::string& path) {
        cli = std::make_shared<httplib::Client>("127.0.0.1", port);
        cli->set_read_timeout(60, 0);
        thread = std::thread([this, path] {
            cli->Get(path.c_str(), httplib::Headers{{"Accept", "text/event-stream"}},
                     [this](const httplib::Response& r) {
                         retry_after = r.get_header_value("Retry-After");
                         status = r.status;
                         return true;
                     },
                     [](const char*, size_t) { return true; });
            if (status == 0) status = -1;           // the connection failed before an answer
        });
    }
    bool answered(std::chrono::milliseconds limit = 3000ms) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (status != 0) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
    void close() {
        cli->stop();
        if (thread.joinable()) thread.join();
    }
    ~OpenStream() { close(); }
};

struct CapFixture {
    ChannelManager                  manager;
    liveqx::events::EventBus        bus;
    std::unique_ptr<ControlApi>     api;

    CapFixture(int port, ControlApi::Limits limits) : manager(nullptr, {}) {
        api = std::make_unique<ControlApi>(port, manager, nullptr, LivezOptions{}, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, &bus);
        api->setLimits(limits);
        api->start();
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started listening on port " << port;
    }
    ~CapFixture() {
        // The streams wake on an event and notice that their client has gone.
        bus.publish(liveqx::events::EventType::PlayersBrokerStatus, -1, json::object());
        api->stop();
    }
};

std::pair<int, std::chrono::milliseconds> healthz(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(3, 0);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Get("/healthz");
    return {r ? r->status : -1, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)};
}

}  // namespace

TEST(StreamCap, WithEveryAllowedStreamOpenAnOrdinaryRequestIsStillAnsweredAtOnce) {
    // 16 workers, 8 streams (the cap is kept at least 8 below the pool): the rest must stay free.
    CapFixture f(kCapPort, ControlApi::Limits{16, 8, 8});
    std::vector<std::unique_ptr<OpenStream>> streams;
    for (int i = 0; i < 8; ++i) streams.push_back(std::make_unique<OpenStream>(kCapPort, "/api/events/stream"));
    for (auto& s : streams) {
        ASSERT_TRUE(s->answered());
        EXPECT_EQ(s->status, 200);
    }
    for (int i = 0; i < 5; ++i) {
        const auto [code, took] = healthz(kCapPort);
        EXPECT_EQ(code, 200);
        EXPECT_LT(took.count(), 500) << "an ordinary request did not wait for a worker";
    }
}

TEST(StreamCap, AStreamOverTheCapIsRefusedAtOnceAndAFreedPlaceIsTakenAgain) {
    CapFixture f(kCapPort + 1, ControlApi::Limits{16, 8, 2});     // two per user; no RBAC here, so one user for all
    OpenStream first(kCapPort + 1, "/api/events/stream");
    OpenStream second(kCapPort + 1, "/api/events/stream");
    ASSERT_TRUE(first.answered());
    ASSERT_TRUE(second.answered());
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(second.status, 200);

    OpenStream third(kCapPort + 1, "/api/events/stream");
    ASSERT_TRUE(third.answered(1000ms)) << "a refused stream must answer, not wait";
    EXPECT_EQ(third.status, 429);
    EXPECT_EQ(third.retry_after, "5");
    EXPECT_EQ(healthz(kCapPort + 1).first, 200);

    // The host-metrics stream draws on the same allowance.
    OpenStream metrics(kCapPort + 1, "/api/system/host_metrics/stream");
    ASSERT_TRUE(metrics.answered(1000ms));
    EXPECT_EQ(metrics.status, 429) << "all kinds of stream share the user's two places";

    first.close();
    f.bus.publish(liveqx::events::EventType::PlayersBrokerStatus, -1, json::object());   // the server notices the closed socket
    bool taken = false;
    for (int i = 0; i < 40 && !taken; ++i) {
        OpenStream again(kCapPort + 1, "/api/events/stream");
        ASSERT_TRUE(again.answered());
        taken = again.status == 200;
        if (!taken) std::this_thread::sleep_for(100ms);
    }
    EXPECT_TRUE(taken) << "the place of the closed stream was not given back";
}
