// fix33 A3 — NtpSource unit tests (FakeSntpClient).
//
// Реальный UDP/123 в unit-тестах не тыкаем. Все network-paths замокировано
// через FakeSntpClient: контролируем, что offset_ms_ обновляется по
// результату sntp_->query, callback зовётся, retry'и идут по списку
// серверов, и dtor корректно гасит worker.

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "auth/NtpSource.h"
#include "auth/SntpClient.h"
#include "auth/TimeConfig.h"

namespace sa = liveqx::auth;

namespace {

class FakeSntpClient : public sa::ISntpClient {
public:
    struct Call {
        std::string host;
        int         port;
    };

    std::optional<sa::SntpResult> query(
        std::string_view host,
        int              port,
        std::chrono::milliseconds /*timeout*/) override {
        std::lock_guard lk(mu_);
        calls.push_back({std::string(host), port});

        // server_to_offset_ms: host → плановый offset; nullopt → fail.
        auto it = scripted.find(std::string(host));
        if (it == scripted.end() || !it->second.has_value()) {
            return std::nullopt;
        }
        sa::SntpResult r{};
        r.offset_ms      = *it->second;
        r.round_trip_ms  = 10;
        r.server_unix_ms = 1'700'000'000'000;
        return r;
    }

    void script(std::string host, std::optional<std::int64_t> off) {
        std::lock_guard lk(mu_);
        scripted[std::move(host)] = off;
    }

    std::size_t callCount() const {
        std::lock_guard lk(mu_);
        return calls.size();
    }

    std::vector<Call> snapshot() const {
        std::lock_guard lk(mu_);
        return calls;
    }

private:
    mutable std::mutex mu_;
    std::vector<Call> calls;
    std::map<std::string, std::optional<std::int64_t>> scripted;
};

}  // namespace

TEST(NtpSource, WarmUpSyncAppliesOffsetImmediately) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("a.ntp", 4'242);

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"a.ntp"};
    s.poll_interval_s = 3600;

    std::atomic<int> cb_count{0};
    std::atomic<std::int64_t> cb_offset{0};
    sa::NtpSource src(s, fake,
        [&](std::int64_t off, std::int64_t /*at*/) {
            cb_offset.store(off);
            cb_count.fetch_add(1);
        });

    EXPECT_EQ(src.sourceName(), "ntp");
    EXPECT_EQ(src.offsetMs(),   4'242);
    EXPECT_EQ(cb_count.load(),  1);
    EXPECT_EQ(cb_offset.load(), 4'242);
}

TEST(NtpSource, FallsThroughServersUntilFirstSuccess) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("dead.one",   std::nullopt);
    fake->script("dead.two",   std::nullopt);
    fake->script("alive.three", -1'234);

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"dead.one", "dead.two", "alive.three"};
    s.poll_interval_s = 3600;

    sa::NtpSource src(s, fake);
    EXPECT_EQ(src.offsetMs(), -1'234);
    EXPECT_EQ(fake->callCount(), 3u);

    const auto calls = fake->snapshot();
    EXPECT_EQ(calls[0].host, "dead.one");
    EXPECT_EQ(calls[1].host, "dead.two");
    EXPECT_EQ(calls[2].host, "alive.three");
}

TEST(NtpSource, AllServersFailKeepsPreviousOffset) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("dead.one", std::nullopt);

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"dead.one"};
    s.poll_interval_s = 3600;
    s.last_offset_ms  = 999;  // ранее закэшированный.

    std::atomic<int> cb_count{0};
    sa::NtpSource src(s, fake,
        [&](std::int64_t, std::int64_t) { cb_count.fetch_add(1); });

    EXPECT_EQ(src.offsetMs(), 999);  // previous из last_offset_ms.
    EXPECT_EQ(cb_count.load(), 0);   // callback не зовётся при failed sync.
}

TEST(NtpSource, DisabledDoesNotSyncOrSpawnWorker) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("a.ntp", 1);

    sa::NtpSettings s;
    s.enabled         = false;
    s.servers         = {"a.ntp"};
    s.last_offset_ms  = 77;
    s.poll_interval_s = 3600;

    std::atomic<int> cb_count{0};
    sa::NtpSource src(s, fake,
        [&](std::int64_t, std::int64_t) { cb_count.fetch_add(1); });

    EXPECT_EQ(src.offsetMs(), 77);   // last_offset_ms применился.
    EXPECT_EQ(fake->callCount(), 0u);
    EXPECT_EQ(cb_count.load(), 0);
}

TEST(NtpSource, NoServersIsNoOp) {
    auto fake = std::make_shared<FakeSntpClient>();
    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {};
    s.poll_interval_s = 3600;

    sa::NtpSource src(s, fake);
    EXPECT_EQ(src.offsetMs(), 0);
    EXPECT_EQ(fake->callCount(), 0u);
}

TEST(NtpSource, ParsesHostPortSpec) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("h1", 100);
    fake->script("h2", 200);

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"h2:1234"};  // host:port вариант.
    s.poll_interval_s = 3600;

    sa::NtpSource src(s, fake);
    EXPECT_EQ(src.offsetMs(), 200);

    const auto calls = fake->snapshot();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].host, "h2");
    EXPECT_EQ(calls[0].port, 1234);
}

TEST(NtpSource, NowAppliesOffsetOnTopOfSystemClock) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("a.ntp", 60'000);  // +1m

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"a.ntp"};
    s.poll_interval_s = 3600;

    sa::NtpSource src(s, fake);
    const auto sys_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto got_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            src.now().time_since_epoch())
                            .count();
    EXPECT_NEAR(got_ms - sys_ms, 60'000, 50);
}

TEST(NtpSource, SyncNowOnDemand) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("a.ntp", 7);

    sa::NtpSettings s;
    s.enabled         = false;  // даже выключенный — syncNow работает.
    s.servers         = {"a.ntp"};
    s.poll_interval_s = 3600;

    sa::NtpSource src(s, fake);
    EXPECT_EQ(src.offsetMs(), 0);

    EXPECT_TRUE(src.syncNow());
    EXPECT_EQ(src.offsetMs(), 7);
}

TEST(NtpSource, DtorStopsWorkerCleanly) {
    auto fake = std::make_shared<FakeSntpClient>();
    fake->script("a.ntp", 1);

    sa::NtpSettings s;
    s.enabled         = true;
    s.servers         = {"a.ntp"};
    s.poll_interval_s = 3600;  // workered будет долго спать.

    {
        sa::NtpSource src(s, fake);
        // dtor должен быстро вернуться через cv_.notify_all().
    }
    // Если бы dtor висел — тест таймаутнул бы.
    SUCCEED();
}
