// REST tests for /api/players (stage 3 of the player integration).
//
// ControlApi runs on an ephemeral port over a real PlayersDb/PlayersService
// in a temp directory; RBAC is not wired here (rbac == nullptr), so these
// tests cover shapes, status codes and the secrecy of the token. Roles are
// covered separately by the RBAC rule table test.
//
// What these tests protect:
//   * the bootstrap token appears in exactly one response — the one that
//     creates (or re-issues) the package — and in no list/detail answer;
//   * nothing is created while the module is not configured;
//   * bad input gets a 4xx with a snake_case code, never a 500;
//   * revoking / deleting reaches the CRL hook.

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "players/PlayersDb.h"
#include "players/EnrollService.h"
#include "players/PlayersService.h"
#include "auth/MasterKey.h"
#include "utils/DeviceCa.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "utils/Tls.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using namespace liveqx::players;

constexpr int kBasePort = 19400;

struct PlayersApiFixture {
    fs::path                    tmp;
    std::unique_ptr<PlayersDb>  db;
    std::unique_ptr<PlayersService> svc;
    std::unique_ptr<liveqx::auth::MasterKey> mk;
    std::shared_ptr<const liveqx::tls::DeviceCa> device_ca;
    std::unique_ptr<EnrollService> enroll;
    std::vector<CertRow>        crl_hook;
    ChannelManager              manager;
    std::unique_ptr<ControlApi> api;
    int                         port;

    PlayersApiFixture(int p, bool with_service = true)
        : manager(nullptr, fs::path{}), port(p) {
        static std::atomic<int> seq{0};
        tmp = fs::temp_directory_path() /
              ("lqx-playersapi-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
        fs::create_directories(tmp / "tls");
        EXPECT_TRUE(liveqx::tls::ensureCa(tmp / "tls").ok());
        db = std::make_unique<PlayersDb>(tmp / "players.db");
        EXPECT_TRUE(db->open());
        PlayersServiceOptions o;
        o.ca_cert_path = tmp / "tls" / "ca.crt";
        o.on_certs_revoked = [this](const std::vector<CertRow>& c) {
            crl_hook.insert(crl_hook.end(), c.begin(), c.end());
        };
        svc = std::make_unique<PlayersService>(*db, std::move(o));

        mk = std::make_unique<liveqx::auth::MasterKey>((tmp / "master.key").string());
        EXPECT_TRUE(mk->load());
        std::string err;
        auto ca = liveqx::tls::DeviceCa::open(tmp / "device_ca", *mk, err, true);
        EXPECT_TRUE(ca) << err;
        device_ca = std::shared_ptr<const liveqx::tls::DeviceCa>(std::move(ca));
        EnrollOptions eo;
        eo.ca = [this] { return device_ca; };
        enroll = std::make_unique<EnrollService>(*db, std::move(eo));

        api = std::make_unique<ControlApi>(
            port, manager, nullptr, LivezOptions{}, nullptr, nullptr, nullptr, nullptr,
            /*rbac=*/nullptr, /*events=*/nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            TlsBindings{}, nullptr, nullptr, nullptr, nullptr, nullptr,
            with_service ? svc.get() : nullptr,
            with_service ? enroll.get() : nullptr);
        api->start();
        httplib::Client cli("127.0.0.1", port);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started listening on port " << port;
    }
    ~PlayersApiFixture() {
        if (api) api->stop();
        api.reset();
        enroll.reset();
        device_ca.reset();
        svc.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    httplib::Client client() const { return httplib::Client("127.0.0.1", port); }

    void configure() {
        auto cli = client();
        auto r = cli.Put("/api/players/settings",
                         json{{"server_url", "https://liveqx.example:8443"},
                              {"device_api_url", "https://liveqx.example:8444"},
                              {"mqtt_broker_url", "mqtts://liveqx.example:8883"}}.dump(),
                         "application/json");
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200) << r->body;
    }

    json create(const std::string& name) {
        auto cli = client();
        auto r = cli.Post("/api/players", json{{"name", name}}.dump(), "application/json");
        EXPECT_TRUE(r);
        EXPECT_EQ(r->status, 201) << (r ? r->body : "");
        return r ? json::parse(r->body) : json{};
    }
};

CertRow certFor(const std::string& player, const std::string& serial) {
    const std::int64_t now = std::time(nullptr);
    CertRow c;
    c.serial_hex = serial; c.generation = 1; c.player_id = player;
    c.issued_at = now; c.not_before = now - 86400; c.not_after = now + 365 * 86400;
    return c;
}

}  // namespace

TEST(ControlApiPlayers, WithoutTheServiceEverythingIs503) {
    PlayersApiFixture f(kBasePort + 0, /*with_service=*/false);
    auto cli = f.client();
    for (auto* path : {"/api/players", "/api/players/settings"}) {
        auto r = cli.Get(path);
        ASSERT_TRUE(r) << path;
        EXPECT_EQ(r->status, 503) << path;
    }
    auto r = cli.Post("/api/players", R"({"name":"x"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
}

TEST(ControlApiPlayers, SettingsHaveDefaultsAndAcceptValidChanges) {
    PlayersApiFixture f(kBasePort + 1);
    auto cli = f.client();
    auto g = cli.Get("/api/players/settings");
    ASSERT_TRUE(g);
    ASSERT_EQ(g->status, 200);
    auto s = json::parse(g->body);
    EXPECT_EQ(s["token_ttl_days"], 7);
    EXPECT_EQ(s["clock_policy"], "strict");

    auto p = cli.Put("/api/players/settings",
                     R"({"server_url":"https://a.example","token_ttl_days":30,"clock_policy":"relaxed"})",
                     "application/json");
    ASSERT_TRUE(p);
    ASSERT_EQ(p->status, 200) << p->body;
    auto after = json::parse(p->body);
    EXPECT_EQ(after["server_url"], "https://a.example");
    EXPECT_EQ(after["token_ttl_days"], 30);
    EXPECT_EQ(after["clock_policy"], "relaxed");
}

TEST(ControlApiPlayers, InvalidSettingsAreRefusedWithACode) {
    PlayersApiFixture f(kBasePort + 2);
    auto cli = f.client();
    auto r = cli.Put("/api/players/settings", R"({"server_url":"http://insecure.example"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_server_url");

    r = cli.Put("/api/players/settings", R"({"colour":"blue"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "unknown_setting");

    r = cli.Put("/api/players/settings", R"({"token_ttl_days":[1]})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);

    r = cli.Put("/api/players/settings", "{ nope", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(ControlApiPlayers, CreatingBeforeConfigurationIsAConflict) {
    PlayersApiFixture f(kBasePort + 3);
    auto cli = f.client();
    auto r = cli.Post("/api/players", R"({"name":"Lobby"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(json::parse(r->body)["error"], "not_configured");
    EXPECT_TRUE(f.db->listPlayers({}).empty());
}

TEST(ControlApiPlayers, CreateReturnsThePackageOnce) {
    PlayersApiFixture f(kBasePort + 4);
    f.configure();
    auto pkg = f.create("Lobby");
    EXPECT_EQ(pkg["schema"], 1);
    EXPECT_EQ(pkg["server_url"], "https://liveqx.example:8443");
    const std::string token = pkg["bootstrap_token"];
    const std::string id = pkg["player_id"];
    ASSERT_FALSE(token.empty());

    auto cli = f.client();
    auto list = cli.Get("/api/players");
    ASSERT_TRUE(list);
    ASSERT_EQ(list->status, 200);
    EXPECT_EQ(list->body.find(token), std::string::npos) << "the token must not be listed";
    auto items = json::parse(list->body)["players"];
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0]["id"], id);
    EXPECT_EQ(items[0]["name"], "Lobby");
    EXPECT_EQ(items[0]["state"], "pending");

    auto one = cli.Get(("/api/players/" + id).c_str());
    ASSERT_TRUE(one);
    ASSERT_EQ(one->status, 200);
    EXPECT_EQ(one->body.find(token), std::string::npos);
    auto d = json::parse(one->body);
    EXPECT_EQ(d["id"], id);
    EXPECT_TRUE(d["certificates"].is_array());
    EXPECT_TRUE(d["events"].is_array());
}

TEST(ControlApiPlayers, BadCreateRequestsAreClientErrors) {
    PlayersApiFixture f(kBasePort + 5);
    f.configure();
    auto cli = f.client();
    auto r = cli.Post("/api/players", R"({})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_name");

    r = cli.Post("/api/players", R"({"name":"x","clock_policy":"lax"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_clock_policy");

    r = cli.Post("/api/players", R"({"name":42})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);

    r = cli.Post("/api/players", "{ nope", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_TRUE(f.db->listPlayers({}).empty());
}

TEST(ControlApiPlayers, UnknownOrMalformedIdsAre404) {
    PlayersApiFixture f(kBasePort + 6);
    auto cli = f.client();
    for (auto* id : {"00000000-0000-4000-8000-000000000000", "not-a-uuid"}) {
        auto g = cli.Get((std::string("/api/players/") + id).c_str());
        ASSERT_TRUE(g) << id;
        EXPECT_EQ(g->status, 404) << id;
        auto d = cli.Delete((std::string("/api/players/") + id).c_str());
        ASSERT_TRUE(d) << id;
        EXPECT_EQ(d->status, 404) << id;
        auto v = cli.Post((std::string("/api/players/") + id + "/revoke-cert").c_str(), "", "application/json");
        ASSERT_TRUE(v) << id;
        EXPECT_EQ(v->status, 404) << id;
    }
}

TEST(ControlApiPlayers, ListCanBeFilteredByState) {
    PlayersApiFixture f(kBasePort + 7);
    f.configure();
    auto a = f.create("A");
    f.create("B");
    auto cli = f.client();
    ASSERT_TRUE(cli.Post(("/api/players/" + a["player_id"].get<std::string>() + "/revoke-cert").c_str(),
                         "", "application/json"));
    auto rev = cli.Get("/api/players?state=revoked");
    ASSERT_TRUE(rev);
    ASSERT_EQ(rev->status, 200);
    EXPECT_EQ(json::parse(rev->body)["players"].size(), 1u);
    auto pend = cli.Get("/api/players?state=pending");
    ASSERT_TRUE(pend);
    EXPECT_EQ(json::parse(pend->body)["players"].size(), 1u);
    auto bad = cli.Get("/api/players?state=zombie");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
}

TEST(ControlApiPlayers, ReissueGivesANewTokenAndKillsTheOldOne) {
    PlayersApiFixture f(kBasePort + 8);
    f.configure();
    auto first = f.create("Lobby");
    const std::string id = first["player_id"];
    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + id + "/reissue-token").c_str(), "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto second = json::parse(r->body);
    EXPECT_NE(second["bootstrap_token"], first["bootstrap_token"]);
    EXPECT_FALSE(f.db->claimToken(id, first["bootstrap_token"]).ok);
    EXPECT_TRUE(f.db->claimToken(id, second["bootstrap_token"]).ok);
}

TEST(ControlApiPlayers, ReissueForAnActivePlayerIsAConflict) {
    PlayersApiFixture f(kBasePort + 9);
    f.configure();
    auto pkg = f.create("Lobby");
    const std::string id = pkg["player_id"];
    auto claim = f.db->claimToken(id, pkg["bootstrap_token"]);
    ASSERT_TRUE(claim.ok);
    ASSERT_TRUE(f.db->finishEnrollment(claim.token_id, certFor(id, "AB01")));
    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + id + "/reissue-token").c_str(), "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(json::parse(r->body)["error"], "not_pending");
}

TEST(ControlApiPlayers, RevokeCertRevokesAndReportsToTheCrlHook) {
    PlayersApiFixture f(kBasePort + 10);
    f.configure();
    auto pkg = f.create("Lobby");
    const std::string id = pkg["player_id"];
    auto claim = f.db->claimToken(id, pkg["bootstrap_token"]);
    ASSERT_TRUE(f.db->finishEnrollment(claim.token_id, certFor(id, "AB02")));

    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + id + "/revoke-cert").c_str(),
                      R"({"reason":"stolen display"})", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["revoked_certs"], 1);
    ASSERT_EQ(f.crl_hook.size(), 1u);
    EXPECT_EQ(f.crl_hook[0].serial_hex, "AB02");

    auto d = json::parse(cli.Get(("/api/players/" + id).c_str())->body);
    EXPECT_EQ(d["state"], "revoked");
    EXPECT_EQ(d["revoke_reason"], "stolen display");
    ASSERT_EQ(d["certificates"].size(), 1u);
    EXPECT_EQ(d["certificates"][0]["serial"], "AB02");
    EXPECT_TRUE(d["certificates"][0].contains("revoked_at"));
}

TEST(ControlApiPlayers, DeleteRemovesThePlayerButTheCertificateStaysRevoked) {
    PlayersApiFixture f(kBasePort + 11);
    f.configure();
    auto pkg = f.create("Lobby");
    const std::string id = pkg["player_id"];
    auto claim = f.db->claimToken(id, pkg["bootstrap_token"]);
    ASSERT_TRUE(f.db->finishEnrollment(claim.token_id, certFor(id, "AB03")));

    auto cli = f.client();
    auto r = cli.Delete(("/api/players/" + id).c_str());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(f.crl_hook.size(), 1u);
    EXPECT_EQ(cli.Get(("/api/players/" + id).c_str())->status, 404);
    EXPECT_EQ(f.db->revokedSerials(1).size(), 1u);
}

TEST(ControlApiPlayers, OverlongReasonIsRefused) {
    PlayersApiFixture f(kBasePort + 12);
    f.configure();
    auto pkg = f.create("Lobby");
    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + pkg["player_id"].get<std::string>() + "/revoke-cert").c_str(),
                      json{{"reason", std::string(1000, 'x')}}.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_reason");
}

// ─── Contract with openapi.yaml ──────────────────────────────────────────────
// The `required:` lists below mirror components.schemas.{PairingPackage,
// Player, PlayerDetails, PlayersSettings, RevokeResult}. When one side
// changes, update both (same convention as test_openapi_contract.cpp).

namespace {
void expectKeys(const json& obj, std::initializer_list<const char*> keys, const char* what) {
    for (auto* k : keys) EXPECT_TRUE(obj.contains(k)) << what << " lacks " << k;
}
}  // namespace

TEST(ControlApiPlayers, ResponsesMatchTheOpenApiRequiredFields) {
    PlayersApiFixture f(kBasePort + 13);
    f.configure();
    auto cli = f.client();

    expectKeys(json::parse(cli.Get("/api/players/settings")->body),
               {"server_url", "device_api_url", "mqtt_broker_url", "ca_cert_pem", "token_ttl_days",
                "clock_policy"}, "PlayersSettings");

    auto pkg = f.create("Lobby");
    expectKeys(pkg, {"schema", "player_id", "server_url", "ca_cert_pem", "device_api_url",
                     "mqtt_broker_url", "bootstrap_token", "token_expires_at", "clock_policy"},
               "PairingPackage");

    auto list = json::parse(cli.Get("/api/players")->body);
    expectKeys(list, {"players"}, "list");
    expectKeys(list["players"][0], {"id", "name", "state", "clock_policy", "created_at"}, "Player");

    const std::string id = pkg["player_id"];
    expectKeys(json::parse(cli.Get(("/api/players/" + id).c_str())->body),
               {"id", "name", "state", "clock_policy", "created_at", "certificates", "events"},
               "PlayerDetails");

    auto rv = cli.Post(("/api/players/" + id + "/revoke-cert").c_str(), "", "application/json");
    expectKeys(json::parse(rv->body), {"revoked_certs"}, "RevokeResult");
}

// ─── POST /api/players/enroll (open endpoint, A4) ───────────────────────────

namespace {
std::string csrPem() {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_pubkey(req, key);
    X509_REQ_sign(req, key, EVP_sha256());
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string pem(d, static_cast<size_t>(n));
    BIO_free_all(b);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    return pem;
}
json enrollBody(const json& pkg, const std::string& csr = csrPem()) {
    return {{"schema", 1}, {"player_id", pkg["player_id"]},
            {"bootstrap_token", pkg["bootstrap_token"]}, {"csr_pem", csr}};
}
}  // namespace

TEST(ControlApiPlayers, EnrollIssuesACertificateAndStampsTheServerTime) {
    PlayersApiFixture f(kBasePort + 14);
    f.configure();
    auto pkg = f.create("Lobby");
    auto cli = f.client();
    auto r = cli.Post("/api/players/enroll", enrollBody(pkg).dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto b = json::parse(r->body);
    EXPECT_EQ(b["schema"], 1);
    EXPECT_NE(b["client_cert_pem"].get<std::string>().find("BEGIN CERTIFICATE"), std::string::npos);
    ASSERT_TRUE(r->has_header("X-Server-Time"));
    const std::string t = r->get_header_value("X-Server-Time");
    EXPECT_EQ(t.size(), 20u) << t;                       // 2026-10-14T12:00:00Z
    EXPECT_EQ(t.back(), 'Z');
    EXPECT_EQ(f.db->findPlayer(pkg["player_id"])->state, PlayerState::Active);
}

TEST(ControlApiPlayers, EnrollFailuresAreUniformAndStampedToo) {
    PlayersApiFixture f(kBasePort + 15);
    f.configure();
    auto pkg = f.create("Lobby");
    auto body = enrollBody(pkg);
    body["bootstrap_token"] = "nope";
    auto cli = f.client();
    auto r = cli.Post("/api/players/enroll", body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(json::parse(r->body), (json{{"error", "invalid_token"}}));
    EXPECT_TRUE(r->has_header("X-Server-Time"));

    r = cli.Post("/api/players/enroll", "{ nope", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_TRUE(r->has_header("X-Server-Time"));

    r = cli.Post("/api/players/enroll",
                 json{{"schema", 1}, {"player_id", 42}, {"bootstrap_token", json::array({1})},
                      {"csr_pem", csrPem()}}.dump(),
                 "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401) << "wrongly typed fields are just a failed token";
}

TEST(ControlApiPlayers, EnrollRateLimitCarriesRetryAfter) {
    PlayersApiFixture f(kBasePort + 16);
    f.configure();
    auto pkg = f.create("Lobby");
    auto body = enrollBody(pkg);
    body["bootstrap_token"] = "nope";
    auto cli = f.client();
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(cli.Post("/api/players/enroll", body.dump(), "application/json")->status, 401);
    auto r = cli.Post("/api/players/enroll", enrollBody(pkg).dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429);
    EXPECT_EQ(json::parse(r->body)["error"], "rate_limited");
    ASSERT_TRUE(r->has_header("Retry-After"));
    EXPECT_GT(std::stoi(r->get_header_value("Retry-After")), 0);
}

TEST(ControlApiPlayers, EnrollWithoutTheModuleIs503) {
    PlayersApiFixture f(kBasePort + 17, /*with_service=*/false);
    auto cli = f.client();
    auto r = cli.Post("/api/players/enroll", "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
}
