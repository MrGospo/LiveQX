// fix33 A4 — REST integration tests для /api/system/time.
//
// GET  /api/system/time          → config + runtime snapshot
// PUT  /api/system/time          → merge-patch, persist + reconfigure
// POST /api/system/time/test     → SNTP probe (FakeSntpClient в тестах)

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "auth/AuthDb.h"
#include "auth/SntpClient.h"
#include "auth/TimeConfig.h"
#include "auth/TimeConfigRepo.h"
#include "auth/TimeSource.h"

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr int kSysTimePortBase = 19850;

int nextPort() {
    static std::atomic<int> n{0};
    static const int base = kSysTimePortBase + (static_cast<int>(::getpid()) % 700) * 5;
    return base + n.fetch_add(1);
}

class FakeSntpClient : public liveqx::auth::ISntpClient {
public:
    std::optional<liveqx::auth::SntpResult> query(
        std::string_view host, int port,
        std::chrono::milliseconds /*timeout*/) override {
        std::lock_guard lk(mu_);
        calls.push_back({std::string(host), port});
        auto it = scripted.find(std::string(host));
        if (it == scripted.end() || !it->second.has_value()) {
            return std::nullopt;
        }
        liveqx::auth::SntpResult r{};
        r.offset_ms      = *it->second;
        r.round_trip_ms  = 7;
        r.server_unix_ms = 1'700'000'000'000;
        return r;
    }

    void script(std::string host, std::optional<std::int64_t> off) {
        std::lock_guard lk(mu_);
        scripted[std::move(host)] = off;
    }

    struct Call { std::string host; int port; };
    std::vector<Call> snapshot() const {
        std::lock_guard lk(mu_);
        return calls;
    }

private:
    mutable std::mutex mu_;
    std::vector<Call>  calls;
    std::map<std::string, std::optional<std::int64_t>> scripted;
};

struct Fx {
    std::filesystem::path                                  dbpath;
    std::unique_ptr<liveqx::auth::AuthDb>          db;
    std::unique_ptr<liveqx::auth::TimeConfigRepo>  repo;
    std::unique_ptr<liveqx::auth::TimeSourceManager> tsrc;
    std::shared_ptr<FakeSntpClient>                        sntp;
    ChannelManager                                         manager;
    std::unique_ptr<ControlApi>                            api;
    int                                                    port;

    explicit Fx(int p) : manager(nullptr), port(p) {
        dbpath = std::filesystem::temp_directory_path() /
                 ("sys_time_rest_" + std::to_string(p) + ".db");
        std::filesystem::remove(dbpath);

        db   = std::make_unique<liveqx::auth::AuthDb>(dbpath);
        EXPECT_TRUE(db->open());
        repo = std::make_unique<liveqx::auth::TimeConfigRepo>(*db);
        tsrc = std::make_unique<liveqx::auth::TimeSourceManager>();
        sntp = std::make_shared<FakeSntpClient>();
        tsrc->setNtpDependencies(sntp,
            [this](std::int64_t off, std::int64_t at) {
                repo->updateNtpSyncResult(off, at);
            });

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            /*auth=*/nullptr,
            /*ldap_repo=*/nullptr,
            /*smtp_repo=*/nullptr,
            /*rbac=*/nullptr,
            /*events=*/nullptr,
            /*preview=*/nullptr,
            /*stress=*/nullptr,
            /*plugins=*/nullptr,
            /*master_key=*/nullptr,
            /*mounts=*/nullptr,
            /*tls=*/TlsBindings{},
            repo.get(),
            tsrc.get(),
            sntp.get());
        api->start();
        waitListening(port);
    }

    ~Fx() {
        api->stop();
        api.reset();
        tsrc.reset();
        repo.reset();
        sntp.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove(dbpath, ec);
        std::filesystem::remove(dbpath.string() + "-wal", ec);
        std::filesystem::remove(dbpath.string() + "-shm", ec);
    }

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

TEST(SystemTimeRest, GetEmptyReturnsDefaultsAndRuntimeSnapshot) {
    Fx f(nextPort());
    auto cli = f.client();
    auto r = cli.Get("/api/system/time");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("config"));
    EXPECT_EQ(body["config"]["source"],          "system_local");
    EXPECT_EQ(body["config"]["server_timezone"], "UTC");
    EXPECT_EQ(body["config"]["manual"]["offset_ms"], 0);
    EXPECT_EQ(body["config"]["ntp"]["enabled"],      false);

    ASSERT_TRUE(body.contains("runtime"));
    EXPECT_EQ(body["runtime"]["source"],          "system_local");
    EXPECT_EQ(body["runtime"]["offset_ms"],       0);
    EXPECT_EQ(body["runtime"]["server_timezone"], "UTC");
    EXPECT_TRUE(body["runtime"]["effective_now"].is_number_integer());
    EXPECT_TRUE(body["runtime"]["system_now"].is_number_integer());
}

TEST(SystemTimeRest, PutManualOffsetPersistsAndReconfigures) {
    Fx f(nextPort());
    auto cli = f.client();

    json req = {
        {"source", "manual"},
        {"manual", {{"offset_ms", 60'000}}}  // +1m
    };
    auto r = cli.Put("/api/system/time", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    // Сразу GET — конфиг + runtime отражают.
    auto g = cli.Get("/api/system/time");
    ASSERT_TRUE(g);
    auto body = json::parse(g->body);
    EXPECT_EQ(body["config"]["source"], "manual");
    EXPECT_EQ(body["config"]["manual"]["offset_ms"], 60'000);
    EXPECT_EQ(body["runtime"]["source"],    "manual");
    EXPECT_EQ(body["runtime"]["offset_ms"], 60'000);
}

TEST(SystemTimeRest, PutManualEpochUnixMsConvertsToOffset) {
    Fx f(nextPort());
    auto cli = f.client();

    const std::int64_t sys_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t target_ms = sys_now_ms + 5'000;  // future by 5s

    json req = {
        {"source", "manual"},
        {"manual", {{"epoch_unix_ms", target_ms}}}
    };
    auto r = cli.Put("/api/system/time", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    auto body = json::parse(r->body);
    // Допустимый дрифт — пара миллисекунд между захватом sys_now_ms в тесте
    // и handler'е. Берём окно ±200ms.
    EXPECT_NEAR(body["config"]["manual"]["offset_ms"].get<std::int64_t>(),
                5'000, 200);
}

TEST(SystemTimeRest, PutInvalidTimezoneReturns400) {
    Fx f(nextPort());
    auto cli = f.client();
    json req = {{"server_timezone", "Mars/Olympus"}};
    auto r = cli.Put("/api/system/time", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "invalid_config");
}

TEST(SystemTimeRest, PutNtpWithoutServersReturns400) {
    Fx f(nextPort());
    auto cli = f.client();
    json req = {
        {"source", "ntp"},
        {"ntp", {{"enabled", true}, {"servers", json::array()}}}
    };
    auto r = cli.Put("/api/system/time", req.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << r->body;
}

TEST(SystemTimeRest, PutNtpWithServersUsesFakeSntpAndReportsOffset) {
    Fx f(nextPort());
    f.sntp->script("a.ntp", 42);

    auto cli = f.client();
    json req = {
        {"source", "ntp"},
        {"ntp", {
            {"enabled", true},
            {"servers", {"a.ntp"}},
            {"poll_interval_s", 3600}
        }}
    };
    auto r = cli.Put("/api/system/time", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    // Warm-up sync произошёл в reconfigure() → offset_ms = 42.
    auto g = cli.Get("/api/system/time");
    ASSERT_TRUE(g);
    auto body = json::parse(g->body);
    EXPECT_EQ(body["runtime"]["source"],    "ntp");
    EXPECT_EQ(body["runtime"]["offset_ms"], 42);
    // updateNtpSyncResult должен был быть вызван — отразилось в config.
    ASSERT_TRUE(body["config"]["ntp"]["last_offset_ms"].is_number_integer());
    EXPECT_EQ(body["config"]["ntp"]["last_offset_ms"], 42);
}

TEST(SystemTimeRest, PostTestProbesServersViaFakeSntp) {
    Fx f(nextPort());
    f.sntp->script("good.host", 100);
    // "bad.host" не заскриптован → query вернёт nullopt → ok:false.

    auto cli = f.client();
    json req = {
        {"servers", {"good.host", "bad.host"}},
        {"timeout_ms", 500}
    };
    auto r = cli.Post("/api/system/time/test", req.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;

    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("results"));
    ASSERT_EQ(body["results"].size(), 2u);
    EXPECT_EQ(body["results"][0]["server"],   "good.host");
    EXPECT_EQ(body["results"][0]["ok"],       true);
    EXPECT_EQ(body["results"][0]["offset_ms"],100);
    EXPECT_EQ(body["results"][1]["server"],   "bad.host");
    EXPECT_EQ(body["results"][1]["ok"],       false);
}

TEST(SystemTimeRest, PostTestWithoutBodyFallsBackToSavedServers) {
    Fx f(nextPort());
    f.sntp->script("saved.ntp", 77);

    // Сначала PUT с одним сервером — он попадёт в saved config.
    auto cli = f.client();
    json put_req = {
        {"source", "ntp"},
        {"ntp", {
            {"enabled", true},
            {"servers", {"saved.ntp"}},
            {"poll_interval_s", 3600}
        }}
    };
    auto p = cli.Put("/api/system/time", put_req.dump(), "application/json");
    ASSERT_TRUE(p);
    ASSERT_EQ(p->status, 200);

    // POST /test без body → должен взять servers из saved cfg.
    auto r = cli.Post("/api/system/time/test", "{}", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto body = json::parse(r->body);
    ASSERT_EQ(body["results"].size(), 1u);
    EXPECT_EQ(body["results"][0]["server"],   "saved.ntp");
    EXPECT_EQ(body["results"][0]["ok"],       true);
    EXPECT_EQ(body["results"][0]["offset_ms"],77);
}

TEST(SystemTimeRest, PostTestNoServersReturns400) {
    Fx f(nextPort());
    auto cli = f.client();
    auto r = cli.Post("/api/system/time/test", "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    auto body = json::parse(r->body);
    EXPECT_EQ(body["error"], "no_servers");
}
