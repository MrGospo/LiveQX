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
#include "audit/AuditDb.h"
#include "audit/AuditLogger.h"
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
    std::unique_ptr<liveqx::audit::AuditDb> audit_db;
    std::unique_ptr<liveqx::audit::AuditLogger> audit_logger;
    std::vector<CertRow>        crl_hook;
    std::vector<std::string>    reset_hook;
    std::int64_t                object_id{0};
    std::int64_t                group_id{0};
    ChannelManager              manager;
    std::unique_ptr<ControlApi> api;
    int                         port;

    PlayersApiFixture(int p, bool with_service = true, TlsBindings tls = {})
        : manager(nullptr, fs::path{}), port(p) {
        static std::atomic<int> seq{0};
        tmp = fs::temp_directory_path() /
              ("lqx-playersapi-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
        fs::create_directories(tmp / "tls");
        EXPECT_TRUE(liveqx::tls::ensureCa(tmp / "tls").ok());
        db = std::make_unique<PlayersDb>(tmp / "players.db");
        EXPECT_TRUE(db->open());
        {
            auto ob = db->createObject("Main");
            auto gr = db->createGroup(ob->id, "Hall");
            object_id = ob->id;
            group_id  = gr->id;
        }
        PlayersServiceOptions o;
        o.ca_cert_path = tmp / "tls" / "ca.crt";
        o.on_certs_revoked = [this](const std::vector<CertRow>& c) {
            crl_hook.insert(crl_hook.end(), c.begin(), c.end());
        };
        o.on_player_reset = [this](const std::string& id) { reset_hook.push_back(id); };
        svc = std::make_unique<PlayersService>(*db, std::move(o));

        mk = std::make_unique<liveqx::auth::MasterKey>((tmp / "master.key").string());
        EXPECT_TRUE(mk->load());
        std::string err;
        auto ca = liveqx::tls::DeviceCa::open(tmp / "device_ca", *mk, err, true);
        EXPECT_TRUE(ca) << err;
        device_ca = std::shared_ptr<const liveqx::tls::DeviceCa>(std::move(ca));
        audit_db = std::make_unique<liveqx::audit::AuditDb>(tmp / "audit.db", mk.get());
        EXPECT_TRUE(audit_db->open());
        audit_logger = std::make_unique<liveqx::audit::AuditLogger>(audit_db.get(), tmp / "audit-emergency.jsonl");
        audit_logger->start();
        EnrollOptions eo;
        eo.ca = [this] { return device_ca; };
        enroll = std::make_unique<EnrollService>(*db, std::move(eo));

        api = std::make_unique<ControlApi>(
            port, manager, nullptr, LivezOptions{}, nullptr, nullptr, nullptr, nullptr,
            /*rbac=*/nullptr, /*events=*/nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            tls, nullptr, nullptr, nullptr, audit_logger.get(), nullptr,
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
        if (audit_logger) audit_logger->stop();
        audit_logger.reset();
        audit_db.reset();
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
        auto r = cli.Post("/api/players", json{{"name", name}, {"group_id", group_id}}.dump(), "application/json");
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
    auto r = cli.Post("/api/players", json{{"name", "Lobby"}, {"group_id", f.group_id}}.dump(), "application/json");
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
    const json g = {{"group_id", f.group_id}};
    auto withGroup = [&](json b) { b["group_id"] = f.group_id; return b.dump(); };
    auto r = cli.Post("/api/players", g.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_name");

    r = cli.Post("/api/players", withGroup({{"name", "x"}, {"clock_policy", "lax"}}), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_clock_policy");

    r = cli.Post("/api/players", withGroup({{"name", 42}}), "application/json");
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

// ─── Audit: where the enroll rows land ──────────────────────────────────────

TEST(ControlApiPlayers, ADeniedEnrollIsFiledAsAccessAndASuccessAsPlayer) {
    PlayersApiFixture f(kBasePort + 18);
    f.configure();
    auto pkg = f.create("Lobby");
    auto bad = enrollBody(pkg);
    bad["bootstrap_token"] = "nope";
    auto cli = f.client();
    ASSERT_EQ(cli.Post("/api/players/enroll", bad.dump(), "application/json")->status, 401);
    ASSERT_EQ(cli.Post("/api/players/enroll", enrollBody(pkg).dump(), "application/json")->status, 200);
    f.audit_logger->flushForTesting();

    auto find = [&](const char* action) {
        liveqx::audit::AuditFilter af;
        af.action = action;
        af.limit  = 8;
        return f.audit_db->list(af);
    };
    auto denied = find("player.enroll_denied");
    ASSERT_EQ(denied.size(), 1u);
    EXPECT_EQ(denied[0].category, liveqx::audit::Category::Access)
        << "a refused pairing is a refused access: it belongs where brute force is looked for";
    EXPECT_NE(denied[0].details_json.find("token_mismatch"), std::string::npos);
    EXPECT_EQ(denied[0].details_json.find("nope"), std::string::npos) << "the guess itself is never logged";

    auto ok = find("player.enroll");
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0].category, liveqx::audit::Category::Player);
    EXPECT_EQ(ok[0].target_id, pkg["player_id"].get<std::string>());
}

// ─── Behind a reverse proxy ─────────────────────────────────────────────────
// The test client connects from 127.0.0.1, which plays the proxy.

namespace {
TlsBindings behindProxy(std::vector<std::string> trusted) {
    TlsBindings t;
    t.mode = "behind_proxy";
    t.trust_proxy_cidrs = std::move(trusted);
    return t;
}
httplib::Headers xff(const std::string& v) { return {{"X-Forwarded-For", v}}; }
}  // namespace

TEST(ControlApiPlayers, BehindATrustedProxyEachClientHasItsOwnRateLimit) {
    PlayersApiFixture f(kBasePort + 19, true, behindProxy({"127.0.0.1"}));
    f.configure();
    auto pkg = f.create("Lobby");
    auto bad = enrollBody(pkg);
    bad["bootstrap_token"] = "nope";
    auto cli = f.client();
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(cli.Post("/api/players/enroll", xff("198.51.100.1"), bad.dump(), "application/json")->status, 401);
    EXPECT_EQ(cli.Post("/api/players/enroll", xff("198.51.100.1"), enrollBody(pkg).dump(), "application/json")->status, 429)
        << "the guessing client is locked";
    EXPECT_EQ(cli.Post("/api/players/enroll", xff("198.51.100.2"), enrollBody(pkg).dump(), "application/json")->status, 200)
        << "another client behind the same proxy is not";
}

TEST(ControlApiPlayers, WithoutTrustTheProxyHeaderChangesNothing) {
    // behind_proxy but no trusted list: everyone is the proxy, and a forged
    // header must not buy a fresh rate-limit bucket.
    PlayersApiFixture f(kBasePort + 20, true, behindProxy({}));
    f.configure();
    auto pkg = f.create("Lobby");
    auto bad = enrollBody(pkg);
    bad["bootstrap_token"] = "nope";
    auto cli = f.client();
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(cli.Post("/api/players/enroll", xff("198.51.100." + std::to_string(i)), bad.dump(), "application/json")->status, 401);
    EXPECT_EQ(cli.Post("/api/players/enroll", xff("203.0.113.99"), enrollBody(pkg).dump(), "application/json")->status, 429);
}

TEST(ControlApiPlayers, AHeaderFromAPeerThatIsNotTrustedIsIgnored) {
    PlayersApiFixture f(kBasePort + 21, true, behindProxy({"10.0.0.0/8"}));   // 127.0.0.1 not in it
    f.configure();
    auto pkg = f.create("Lobby");
    auto bad = enrollBody(pkg);
    bad["bootstrap_token"] = "nope";
    auto cli = f.client();
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(cli.Post("/api/players/enroll", xff("198.51.100." + std::to_string(i)), bad.dump(), "application/json")->status, 401);
    EXPECT_EQ(cli.Post("/api/players/enroll", xff("203.0.113.99"), enrollBody(pkg).dump(), "application/json")->status, 429);
}

TEST(ControlApiPlayers, TheAuditTrailNamesTheRealClientBehindAProxy) {
    PlayersApiFixture f(kBasePort + 22, true, behindProxy({"127.0.0.1"}));
    f.configure();
    auto pkg = f.create("Lobby");
    auto bad = enrollBody(pkg);
    bad["bootstrap_token"] = "nope";
    auto cli = f.client();
    ASSERT_EQ(cli.Post("/api/players/enroll", xff("6.6.6.6, 198.51.100.7"), bad.dump(), "application/json")->status, 401);
    f.audit_logger->flushForTesting();
    liveqx::audit::AuditFilter af;
    af.action = "player.enroll_denied";
    af.limit  = 4;
    auto rows = f.audit_db->list(af);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].actor_ip, "198.51.100.7") << "rightmost entry, not the forged leading one";
}

// ─── Objects, groups and where a player sits ────────────────────────────────
TEST(ControlApiPlayers, CreatingAPlayerNeedsAnExistingGroup) {
    PlayersApiFixture f(kBasePort + 23);
    f.configure();
    auto cli = f.client();
    auto post = [&](const json& b) { return cli.Post("/api/players", b.dump(), "application/json"); };

    auto none = post({{"name", "x"}});
    ASSERT_TRUE(none);
    EXPECT_EQ(none->status, 400);
    EXPECT_EQ(json::parse(none->body)["error"], "group_required");
    EXPECT_EQ(json::parse(post({{"name", "x"}, {"group_id", nullptr}})->body)["error"], "group_required");

    auto bad = post({{"name", "x"}, {"group_id", "one"}});
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(json::parse(bad->body)["error"], "invalid_group");

    auto ghost = post({{"name", "x"}, {"group_id", 424242}});
    ASSERT_TRUE(ghost);
    EXPECT_EQ(ghost->status, 400);
    EXPECT_EQ(json::parse(ghost->body)["error"], "group_not_found");
    EXPECT_TRUE(f.db->listPlayers({}).empty());
}

TEST(ControlApiPlayers, PlayersCarryTheirPlaceInListAndDetails) {
    PlayersApiFixture f(kBasePort + 24);
    f.configure();
    auto pkg = f.create("Lobby");
    auto cli = f.client();
    auto list = json::parse(cli.Get("/api/players")->body)["players"];
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["group_id"], f.group_id);
    EXPECT_EQ(list[0]["group_name"], "Hall");
    EXPECT_EQ(list[0]["object_id"], f.object_id);
    EXPECT_EQ(list[0]["object_name"], "Main");
    auto d = json::parse(cli.Get(("/api/players/" + pkg["player_id"].get<std::string>()).c_str())->body);
    EXPECT_EQ(d["group_name"], "Hall");
    EXPECT_EQ(d["object_name"], "Main");
}

TEST(ControlApiPlayers, TheTreeShowsObjectsGroupsAndHowManyPlayersEachHolds) {
    PlayersApiFixture f(kBasePort + 25);
    f.configure();
    f.create("A");
    f.create("B");
    auto other = f.db->createObject("Annex");
    ASSERT_TRUE(f.db->createGroup(other->id, "Empty room"));
    auto cli = f.client();
    auto r = cli.Get("/api/player-objects");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto objs = json::parse(r->body)["objects"];
    ASSERT_EQ(objs.size(), 2u);
    json main_obj, annex;
    for (auto& o : objs) (o["name"] == "Main" ? main_obj : annex) = o;
    ASSERT_EQ(main_obj["groups"].size(), 1u);
    EXPECT_EQ(main_obj["groups"][0]["name"], "Hall");
    EXPECT_EQ(main_obj["groups"][0]["players"], 2);
    EXPECT_EQ(annex["groups"][0]["players"], 0);
}

TEST(ControlApiPlayers, ObjectsAndGroupsCanBeCreatedRenamedAndRemovedWithClearErrors) {
    PlayersApiFixture f(kBasePort + 26);
    auto cli = f.client();
    auto send = [&](const char* method, const std::string& path, const json& body = json::object()) {
        const std::string b = body.dump();
        if (std::string(method) == "POST")   return cli.Post(path, b, "application/json");
        if (std::string(method) == "PUT")    return cli.Put(path, b, "application/json");
        return cli.Delete(path);
    };

    auto created = send("POST", "/api/player-objects", {{"name", "Mall"}});
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const auto oid = json::parse(created->body)["id"].get<std::int64_t>();
    EXPECT_EQ(send("POST", "/api/player-objects", {{"name", "Mall"}})->status, 409);
    EXPECT_EQ(json::parse(send("POST", "/api/player-objects", {{"name", "Mall"}})->body)["error"], "name_taken");
    EXPECT_EQ(json::parse(send("POST", "/api/player-objects", {{"name", "  "}})->body)["error"], "invalid_name");
    EXPECT_EQ(send("POST", "/api/player-objects", {{"name", 5}})->status, 400);

    EXPECT_EQ(send("PUT", "/api/player-objects/" + std::to_string(oid), {{"name", "Mall 2"}})->status, 200);
    EXPECT_EQ(send("PUT", "/api/player-objects/" + std::to_string(oid), {{"name", "Main"}})->status, 409)
        << "Main exists (the fixture's object)";
    EXPECT_EQ(send("PUT", "/api/player-objects/424242", {{"name", "X"}})->status, 404);

    auto g = send("POST", "/api/player-objects/" + std::to_string(oid) + "/groups", {{"name", "Floor 1"}});
    ASSERT_TRUE(g);
    ASSERT_EQ(g->status, 201) << g->body;
    const auto gid = json::parse(g->body)["id"].get<std::int64_t>();
    EXPECT_EQ(send("POST", "/api/player-objects/" + std::to_string(oid) + "/groups", {{"name", "Floor 1"}})->status, 409);
    EXPECT_EQ(send("POST", "/api/player-objects/424242/groups", {{"name", "X"}})->status, 404);
    EXPECT_EQ(send("PUT", "/api/player-groups/" + std::to_string(gid), {{"name", "Floor 2"}})->status, 200);
    EXPECT_EQ(send("PUT", "/api/player-groups/424242", {{"name", "X"}})->status, 404);

    EXPECT_EQ(send("DELETE", "/api/player-objects/" + std::to_string(oid))->status, 409) << "it still has a group";
    EXPECT_EQ(json::parse(send("DELETE", "/api/player-objects/" + std::to_string(oid))->body)["error"], "not_empty");

    f.configure();
    auto pkg = f.create("Screen");
    ASSERT_EQ(send("PUT", "/api/players/" + pkg["player_id"].get<std::string>() + "/group", {{"group_id", gid}})->status, 200);
    EXPECT_EQ(send("DELETE", "/api/player-groups/" + std::to_string(gid))->status, 409) << "it holds a player";

    ASSERT_EQ(send("PUT", "/api/players/" + pkg["player_id"].get<std::string>() + "/group", {{"group_id", f.group_id}})->status, 200);
    EXPECT_EQ(send("DELETE", "/api/player-groups/" + std::to_string(gid))->status, 200);
    EXPECT_EQ(send("DELETE", "/api/player-groups/" + std::to_string(gid))->status, 404);
    EXPECT_EQ(send("DELETE", "/api/player-objects/" + std::to_string(oid))->status, 200);
}

TEST(ControlApiPlayers, APlayerCanBeMovedToAnotherGroup) {
    PlayersApiFixture f(kBasePort + 27);
    f.configure();
    auto pkg = f.create("Screen");
    const std::string id = pkg["player_id"];
    auto other = f.db->createGroup(f.object_id, "Cafe");
    ASSERT_TRUE(other);
    auto cli = f.client();
    auto move = [&](const std::string& pid, const json& b) {
        return cli.Put(("/api/players/" + pid + "/group").c_str(), b.dump(), "application/json");
    };

    auto r = move(id, {{"group_id", other->id}});
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["group_name"], "Cafe");
    auto d = json::parse(cli.Get(("/api/players/" + id).c_str())->body);
    EXPECT_EQ(d["group_id"], other->id);
    bool moved = false;
    for (auto& e : d["events"]) moved |= (e["kind"] == "moved");
    EXPECT_TRUE(moved);

    EXPECT_EQ(move("00000000-0000-4000-8000-000000000000", {{"group_id", other->id}})->status, 404);
    auto ghost = move(id, {{"group_id", 424242}});
    EXPECT_EQ(ghost->status, 404);
    EXPECT_EQ(json::parse(ghost->body)["error"], "group_not_found");
    EXPECT_EQ(json::parse(move(id, json::object())->body)["error"], "group_required");
    EXPECT_EQ(json::parse(move(id, {{"group_id", "x"}})->body)["error"], "invalid_group");
    EXPECT_EQ(json::parse(cli.Get(("/api/players/" + id).c_str())->body)["group_id"], other->id)
        << "failed moves change nothing";
}

TEST(ControlApiPlayers, ThePlayerListCanBeFilteredByGroupAndObject) {
    PlayersApiFixture f(kBasePort + 28);
    f.configure();
    f.create("A");
    auto annex = f.db->createObject("Annex");
    auto room = f.db->createGroup(annex->id, "Room");
    auto pkg = f.create("B");
    auto cli = f.client();
    ASSERT_EQ(cli.Put(("/api/players/" + pkg["player_id"].get<std::string>() + "/group").c_str(),
                      json{{"group_id", room->id}}.dump(), "application/json")->status, 200);

    auto count = [&](const std::string& q) {
        auto r = cli.Get(("/api/players" + q).c_str());
        return r && r->status == 200 ? json::parse(r->body)["players"].size() : std::size_t(999);
    };
    EXPECT_EQ(count(""), 2u);
    EXPECT_EQ(count("?group_id=" + std::to_string(room->id)), 1u);
    EXPECT_EQ(count("?object_id=" + std::to_string(annex->id)), 1u);
    EXPECT_EQ(count("?object_id=" + std::to_string(f.object_id)), 1u);
    EXPECT_EQ(cli.Get("/api/players?group_id=abc")->status, 400);
    EXPECT_EQ(cli.Get("/api/players?object_id=-1")->status, 400);
}

TEST(ControlApiPlayers, ChangesToTheHierarchyAreAudited) {
    PlayersApiFixture f(kBasePort + 29);
    f.configure();
    auto cli = f.client();
    auto o = json::parse(cli.Post("/api/player-objects", R"({"name":"Mall"})", "application/json")->body);
    auto g = json::parse(cli.Post(("/api/player-objects/" + std::to_string(o["id"].get<std::int64_t>()) + "/groups").c_str(),
                                  R"({"name":"Floor"})", "application/json")->body);
    auto pkg = f.create("Screen");
    ASSERT_EQ(cli.Put(("/api/players/" + pkg["player_id"].get<std::string>() + "/group").c_str(),
                      json{{"group_id", g["id"]}}.dump(), "application/json")->status, 200);
    f.audit_logger->flushForTesting();

    auto find = [&](const char* action) {
        liveqx::audit::AuditFilter af;
        af.action = action;
        af.limit  = 8;
        return f.audit_db->list(af);
    };
    auto oc = find("player.object_create");
    ASSERT_EQ(oc.size(), 1u);
    EXPECT_EQ(oc[0].category, liveqx::audit::Category::Player);
    EXPECT_EQ(oc[0].target_type, "player_object");
    EXPECT_EQ(oc[0].target_id, std::to_string(o["id"].get<std::int64_t>()));
    ASSERT_EQ(find("player.group_create").size(), 1u);
    auto mv = find("player.move");
    ASSERT_EQ(mv.size(), 1u);
    EXPECT_EQ(mv[0].target_type, "player");
    EXPECT_EQ(mv[0].target_id, pkg["player_id"].get<std::string>());
    EXPECT_NE(mv[0].details_json.find("Floor"), std::string::npos) << "the destination is named in the trail";
}

// ─── Binding an enrolled screen again ────────────────────────────────────────
namespace {
// A screen that has finished enrolling, as it stands after the first binding.
std::string enrolledScreen(PlayersApiFixture& f, const std::string& serial, json* first_package = nullptr) {
    auto pkg = f.create("Lobby");
    const std::string id = pkg["player_id"];
    auto claim = f.db->claimToken(id, pkg["bootstrap_token"]);
    EXPECT_TRUE(claim.ok);
    EXPECT_TRUE(f.db->finishEnrollment(claim.token_id, certFor(id, serial)));
    if (first_package) *first_package = pkg;
    return id;
}
}  // namespace

TEST(ControlApiPlayers, ReEnrollingAnActiveScreenCutsTheOldDeviceOffAndGivesANewPackage) {
    PlayersApiFixture f(kBasePort + 30);
    f.configure();
    json first;
    const std::string id = enrolledScreen(f, "AB10", &first);

    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + id + "/re-enroll").c_str(),
                      R"({"reason":"display replaced"})", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto pkg = json::parse(r->body);
    EXPECT_EQ(pkg["player_id"], id) << "the same identity";
    EXPECT_NE(pkg["bootstrap_token"], first["bootstrap_token"]);
    EXPECT_TRUE(pkg.contains("ca_cert_pem"));

    ASSERT_EQ(f.crl_hook.size(), 1u) << "the CRL has to learn about the old certificate";
    EXPECT_EQ(f.crl_hook[0].serial_hex, "AB10");
    EXPECT_EQ(f.reset_hook, std::vector<std::string>{id}) << "the broker record is rebuilt";

    auto d = json::parse(cli.Get(("/api/players/" + id).c_str())->body);
    EXPECT_EQ(d["state"], "pending");
    EXPECT_EQ(d["name"], "Lobby");
    EXPECT_EQ(d["group_id"], f.group_id) << "the place stays";
    ASSERT_EQ(d["certificates"].size(), 1u);
    EXPECT_TRUE(d["certificates"][0].contains("revoked_at"));
    EXPECT_EQ(d["events"][0]["kind"], "re_enrolled");

    EXPECT_TRUE(f.db->claimToken(id, pkg["bootstrap_token"]).ok) << "the new token enrols";
}

TEST(ControlApiPlayers, ReEnrollingARevokedScreenBringsItBack) {
    PlayersApiFixture f(kBasePort + 31);
    f.configure();
    const std::string id = enrolledScreen(f, "AB11");
    auto cli = f.client();
    ASSERT_EQ(cli.Post(("/api/players/" + id + "/revoke-cert").c_str(), "", "application/json")->status, 200);

    auto r = cli.Post(("/api/players/" + id + "/re-enroll").c_str(), "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(cli.Get(("/api/players/" + id).c_str())->body)["state"], "pending");
}

TEST(ControlApiPlayers, ReEnrollingRefusesWhatCannotBeBoundAndChangesNothing) {
    PlayersApiFixture f(kBasePort + 32);
    f.configure();
    auto cli = f.client();

    auto pending = f.create("Fresh");
    auto r = cli.Post(("/api/players/" + pending["player_id"].get<std::string>() + "/re-enroll").c_str(), "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(json::parse(r->body)["error"], "already_pending");

    r = cli.Post("/api/players/00000000-0000-4000-8000-000000000000/re-enroll", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);

    const std::string id = enrolledScreen(f, "AB12");
    r = cli.Post(("/api/players/" + id + "/re-enroll").c_str(), json{{"reason", std::string(1000, 'x')}}.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["error"], "invalid_reason");
    EXPECT_EQ(json::parse(cli.Get(("/api/players/" + id).c_str())->body)["state"], "active");
    EXPECT_TRUE(f.crl_hook.empty());
    EXPECT_TRUE(f.reset_hook.empty());
}

TEST(ControlApiPlayers, ReEnrollingDoesNotCutAWorkingScreenOffWhenTheModuleIsNotSetUp) {
    PlayersApiFixture f(kBasePort + 33);
    f.configure();
    const std::string id = enrolledScreen(f, "AB13");
    auto cli = f.client();
    ASSERT_EQ(cli.Put("/api/players/settings", R"({"server_url":""})", "application/json")->status, 200);

    auto r = cli.Post(("/api/players/" + id + "/re-enroll").c_str(), "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(json::parse(r->body)["error"], "not_configured");
    EXPECT_EQ(json::parse(cli.Get(("/api/players/" + id).c_str())->body)["state"], "active");
    EXPECT_TRUE(f.crl_hook.empty());
}

TEST(ControlApiPlayers, ReEnrollingIsAuditedWithoutTheTokenAndWithTheOldState) {
    PlayersApiFixture f(kBasePort + 34);
    f.configure();
    const std::string id = enrolledScreen(f, "AB14");
    auto cli = f.client();
    auto r = cli.Post(("/api/players/" + id + "/re-enroll").c_str(), R"({"reason":"swap"})", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    const std::string token = json::parse(r->body)["bootstrap_token"];
    f.audit_logger->flushForTesting();

    liveqx::audit::AuditFilter af;
    af.action = "player.re_enroll";
    af.limit  = 8;
    const auto rows = f.audit_db->list(af);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].category, liveqx::audit::Category::Player);
    EXPECT_EQ(rows[0].target_type, "player");
    EXPECT_EQ(rows[0].target_id, id);
    EXPECT_NE(rows[0].details_json.find("\"previous_state\":\"active\""), std::string::npos);
    EXPECT_NE(rows[0].details_json.find("swap"), std::string::npos);
    EXPECT_EQ(rows[0].details_json.find(token), std::string::npos) << "the token never reaches the trail";
}
