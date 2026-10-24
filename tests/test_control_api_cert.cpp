// REST: the step of each screen's certificate in the list and on the card.
//
// ControlApi runs over a real PlayersDb/PlayersService (no accounts here; who sees which
// screen is covered in test_player_scope_rest.cpp).
//
// What these tests protect:
//   * a bound screen carries `cert` {stage, not_after, days_left}; a pending or revoked
//     one carries none;
//   * `cert_counts` count every bound screen of the place and search, whatever the status
//     or `cert` filter says, so the buttons keep their numbers while the list narrows;
//   * `cert=` accepts the groups (soon / expired / attention) and each step by name, and
//     refuses anything else with a code;
//   * a screen with no valid certificate left is over, with no end date to show.

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "players/PlayersDb.h"
#include "players/PlayersService.h"
#include "utils/Tls.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using namespace liveqx::players;

constexpr int kBasePort = 21700;
constexpr std::int64_t kDay = 86400;

struct CertApiFixture {
    fs::path                        tmp;
    std::unique_ptr<PlayersDb>      db;
    std::unique_ptr<PlayersService> svc;
    ChannelManager                  manager;
    std::unique_ptr<ControlApi>     api;
    std::int64_t                    group_id{0};
    int                             port;
    int                             serial{0};

    explicit CertApiFixture(int p) : manager(nullptr, fs::path{}), port(p) {
        static std::atomic<int> seq{0};
        tmp = fs::temp_directory_path() /
              ("lqx-certapi-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
        fs::create_directories(tmp / "tls");
        EXPECT_TRUE(liveqx::tls::ensureCa(tmp / "tls").ok());
        db = std::make_unique<PlayersDb>(tmp / "players.db");
        EXPECT_TRUE(db->open());
        group_id = db->createGroup(db->createObject("Main")->id, "Hall")->id;
        PlayersServiceOptions o;
        o.ca_cert_path = tmp / "tls" / "ca.crt";
        svc = std::make_unique<PlayersService>(*db, std::move(o));
        api = std::make_unique<ControlApi>(
            port, manager, nullptr, LivezOptions{}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, TlsBindings{}, nullptr, nullptr, nullptr, nullptr,
            nullptr, svc.get());
        api->start();
        httplib::Client cli("127.0.0.1", port);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started on port " << port;
    }
    ~CertApiFixture() {
        if (api) api->stop();
        api.reset();
        svc.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    static std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

    // A bound screen whose certificate ends `seconds` from now (negative: already over).
    std::string bound(const std::string& name, std::int64_t seconds) {
        auto p = db->createPlayer(name, std::nullopt, "strict", 3600, 0, group_id);
        EXPECT_TRUE(p.has_value());
        auto claim = db->claimToken(p->player.id, p->token);
        EXPECT_TRUE(claim.ok);
        CertRow c;
        c.serial_hex = "D" + std::to_string(++serial);
        c.generation = 1;
        c.player_id  = p->player.id;
        c.issued_at  = now() - 400 * kDay;
        c.not_before = now() - 400 * kDay;
        c.not_after  = now() + seconds;
        EXPECT_TRUE(db->finishEnrollment(claim.token_id, c));
        return p->player.id;
    }
    std::string pending(const std::string& name) {
        auto p = db->createPlayer(name, std::nullopt, "strict", 3600, 0, group_id);
        EXPECT_TRUE(p.has_value());
        return p->player.id;
    }

    // Five bound screens, one at each step, a pending one and a revoked one.
    void fleet() {
        bound("s-ok",      300 * kDay);
        bound("s-soon30",  25 * kDay + 3600);
        bound("s-soon14",  10 * kDay + 3600);
        bound("s-soon3",   2 * kDay + 3600);
        bound("s-expired", -(3 * kDay) - 3600);
        pending("s-pending");
        const auto r = bound("s-revoked", 300 * kDay);
        EXPECT_TRUE(db->revokePlayer(r, "test").has_value());
    }

    httplib::Client client() const {
        httplib::Client c("127.0.0.1", port);
        c.set_read_timeout(10, 0);
        return c;
    }
    json list(const std::string& query = "", int expect = 200) {
        auto cli = client();
        auto r = cli.Get(("/api/players" + (query.empty() ? std::string() : "?" + query)).c_str());
        EXPECT_TRUE(r);
        if (!r) return json::object();
        EXPECT_EQ(r->status, expect) << r->body;
        return json::parse(r->body);
    }
    static std::set<std::string> names(const json& body) {
        std::set<std::string> out;
        for (const auto& p : body["players"]) out.insert(p["name"].get<std::string>());
        return out;
    }
};

using Names = std::set<std::string>;

}  // namespace

TEST(ControlApiCert, EachBoundScreenCarriesItsStepAndOthersCarryNone) {
    CertApiFixture f(kBasePort + 0);
    f.fleet();
    const json body = f.list();
    std::map<std::string, json> by_name;
    for (const auto& p : body["players"]) by_name[p["name"]] = p;

    EXPECT_EQ(by_name["s-ok"]["cert"]["stage"], "ok");
    EXPECT_EQ(by_name["s-soon30"]["cert"]["stage"], "soon30");
    EXPECT_EQ(by_name["s-soon30"]["cert"]["days_left"], 25);
    EXPECT_EQ(by_name["s-soon14"]["cert"]["stage"], "soon14");
    EXPECT_EQ(by_name["s-soon14"]["cert"]["days_left"], 10);
    EXPECT_EQ(by_name["s-soon3"]["cert"]["stage"], "soon3");
    EXPECT_EQ(by_name["s-soon3"]["cert"]["days_left"], 2);
    EXPECT_EQ(by_name["s-expired"]["cert"]["stage"], "expired");
    EXPECT_LT(by_name["s-expired"]["cert"]["days_left"].get<int>(), 0);
    EXPECT_TRUE(by_name["s-soon3"]["cert"].contains("not_after"));

    EXPECT_FALSE(by_name["s-pending"].contains("cert")) << "no certificate yet, nothing to count";
    EXPECT_FALSE(by_name["s-revoked"].contains("cert"));
}

TEST(ControlApiCert, CountsCoverEveryBoundScreenAndIgnoreTheStatusAndCertFilters) {
    CertApiFixture f(kBasePort + 1);
    f.fleet();
    for (const char* q : {"", "cert=expired", "status=offline", "cert=soon&status=offline", "limit=1"}) {
        const json body = f.list(q);
        const auto& c = body["cert_counts"];
        EXPECT_EQ(c["ok"], 1) << q;
        EXPECT_EQ(c["soon30"], 1) << q;
        EXPECT_EQ(c["soon14"], 1) << q;
        EXPECT_EQ(c["soon3"], 1) << q;
        EXPECT_EQ(c["expired"], 1) << q;
    }
    // The search narrows the counts, as it narrows the status counts.
    const json narrowed = f.list("q=soon");
    EXPECT_EQ(narrowed["cert_counts"]["ok"], 0);
    EXPECT_EQ(narrowed["cert_counts"]["soon30"], 1);
    EXPECT_EQ(narrowed["cert_counts"]["expired"], 0);
}

TEST(ControlApiCert, TheFilterTakesGroupsAndSingleSteps) {
    CertApiFixture f(kBasePort + 2);
    f.fleet();
    EXPECT_EQ(CertApiFixture::names(f.list("cert=soon")), (Names{"s-soon30", "s-soon14", "s-soon3"}));
    EXPECT_EQ(CertApiFixture::names(f.list("cert=expired")), (Names{"s-expired"}));
    EXPECT_EQ(CertApiFixture::names(f.list("cert=attention")),
              (Names{"s-soon30", "s-soon14", "s-soon3", "s-expired"}));
    EXPECT_EQ(CertApiFixture::names(f.list("cert=ok")), (Names{"s-ok"}));
    EXPECT_EQ(CertApiFixture::names(f.list("cert=soon14")), (Names{"s-soon14"}));
    EXPECT_EQ(f.list("cert=soon")["total"], 3) << "the total follows the filter";
    EXPECT_EQ(f.list("cert=attention&q=expired")["total"], 1) << "and it works with the search";
}

TEST(ControlApiCert, AnUnknownStepIsRefusedWithACode) {
    CertApiFixture f(kBasePort + 3);
    f.fleet();
    for (const char* bad : {"cert=bogus", "cert=SOON", "cert=soon%20"}) {
        auto cli = f.client();
        auto r = cli.Get((std::string("/api/players?") + bad).c_str());
        ASSERT_TRUE(r) << bad;
        EXPECT_EQ(r->status, 400) << bad;
        EXPECT_EQ(json::parse(r->body)["error"], "invalid_cert") << bad;
    }
}

TEST(ControlApiCert, AScreenWhoseEveryCertificateIsRevokedIsOverWithNoEndDate) {
    CertApiFixture f(kBasePort + 4);
    const auto id = f.bound("lost", 300 * kDay);
    for (const auto& c : f.db->certsOf(id)) ASSERT_TRUE(f.db->revokeCert(c.generation, c.serial_hex, "test"));
    const json body = f.list();
    ASSERT_EQ(body["players"].size(), 1u);
    EXPECT_EQ(body["players"][0]["cert"]["stage"], "expired");
    EXPECT_FALSE(body["players"][0]["cert"].contains("not_after"));
    EXPECT_FALSE(body["players"][0]["cert"].contains("days_left"));
    EXPECT_EQ(body["cert_counts"]["expired"], 1);
}

TEST(ControlApiCert, TheCardShowsTheStepToo) {
    CertApiFixture f(kBasePort + 5);
    const auto bound = f.bound("Lobby", 10 * kDay + 3600);
    const auto wait  = f.pending("Waiting");
    auto cli = f.client();
    auto r = cli.Get(("/api/players/" + bound).c_str());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    const json card = json::parse(r->body);
    EXPECT_EQ(card["cert"]["stage"], "soon14");
    EXPECT_EQ(card["cert"]["days_left"], 10);

    auto p = cli.Get(("/api/players/" + wait).c_str());
    ASSERT_TRUE(p);
    ASSERT_EQ(p->status, 200);
    EXPECT_FALSE(json::parse(p->body).contains("cert"));
}
