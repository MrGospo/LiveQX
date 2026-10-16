// players/DeviceApi — the device port: a separate HTTPS server where players
// authenticate with their client certificate (mTLS) and send heartbeats.
//
// What these tests protect:
//   * ONE gate runs before any route and fails closed: no certificate, an
//     expired one, one from another center, one that is revoked, a CN that
//     differs from the id in the path, a deleted player — every case gets its
//     own HTTP code (A8/D-1) and NO handler runs. A test walks every route the
//     server registers to prove it;
//   * revocation takes effect on the next request, both through the in-process
//     list and — when the list was never told — through the database;
//   * heartbeat input is untrusted: sizes cut, types checked, nothing the
//     player sends can crash or bloat the store;
//   * every answer, errors included, carries X-Server-Time (D-7);
//   * a version change is recorded once, and a flood of heartbeats is slowed
//     with 429 + Retry-After.

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "events/EventBus.h"
#include "players/DeviceApi.h"
#include "players/RenewService.h"
#include "players/RevocationList.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <unistd.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
namespace kw  = liveqx::auth::keywrap;
using namespace liveqx::players;
using nlohmann::json;

namespace {

void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A key pair written to `key_path`, and its CSR.
std::string makeCsr(const fs::path& key_path) {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* kd = nullptr;
    long kn = BIO_get_mem_data(kb, &kd);
    write(key_path, std::string(kd, static_cast<size_t>(kn)));
    BIO_free_all(kb);
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

// A complete two-tier center: offline root + the server's intermediate.
struct Center {
    std::unique_ptr<tls::DeviceCa> ca;
    static Center make(const fs::path& dir, const liveqx::auth::MasterKey& mk) {
        Center c;
        const fs::path root = dir / "root";
        std::string err, csr, inter;
        EXPECT_TRUE(tls::createDeviceRoot(root, "pass", err, kw::KdfParams::minimal())) << err;
        EXPECT_TRUE(tls::DeviceCa::createIntermediateRequest(dir / "ca", mk, csr, err)) << err;
        EXPECT_TRUE(tls::signDeviceIntermediate(root, "pass", csr, 1825, inter, err)) << err;
        EXPECT_TRUE(tls::DeviceCa::importIntermediate(dir / "ca", mk, inter, slurp(root / "root.crt"), err)) << err;
        c.ca = tls::DeviceCa::open(dir / "ca", mk, err, false);
        EXPECT_TRUE(c.ca) << err;
        return c;
    }
};

struct PlayerCreds {
    std::string id;
    std::string serial;
    fs::path    cert_path;
    fs::path    key_path;
};

class DeviceApiTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<liveqx::auth::MasterKey> mk_;
    std::unique_ptr<PlayersDb> db_;
    RevocationList revocations_;
    liveqx::events::EventBus bus_;
    Center center_, other_center_;
    std::unique_ptr<RenewService> renew_;
    std::unique_ptr<DeviceApi> api_;
    std::atomic<std::int64_t> now_{0};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-devapi-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        now_ = std::time(nullptr);
        mk_ = std::make_unique<liveqx::auth::MasterKey>((dir_ / "master.key").string());
        ASSERT_TRUE(mk_->load());
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        center_       = Center::make(dir_ / "c1", *mk_);
        other_center_ = Center::make(dir_ / "c2", *mk_);

        // Server TLS certificate for the port, from the separate server CA.
        const fs::path srv = dir_ / "server_ca";
        ASSERT_TRUE(tls::ensureCa(srv).ok());
        tls::SanList sans;
        sans.dns_names = {"localhost"};
        sans.ip_v4 = {"127.0.0.1"};
        ASSERT_TRUE(tls::issueServerCert(srv, "localhost", sans).ok());

        RenewOptions ro;
        ro.ca          = [this] { return std::shared_ptr<const tls::DeviceCa>(center_.ca.get(), [](const tls::DeviceCa*) {}); };
        ro.ca_cert_pem = [] { return std::string("SERVER-ROOT-PEM"); };
        ro.now         = [this] { return now_.load(); };
        renew_ = std::make_unique<RenewService>(*db_, std::move(ro));

        DeviceApiOptions o;
        o.renew      = renew_.get();
        o.bind       = "127.0.0.1";
        o.port       = 0;
        o.cert_path  = srv / "server.crt";
        o.key_path   = srv / "server.key";
        o.trust_pem  = center_.ca->rootPem() + center_.ca->chainPem();
        o.now        = [this] { return now_.load(); };
        o.events     = &bus_;
        o.min_heartbeat_interval_s = 10;
        api_ = std::make_unique<DeviceApi>(*db_, revocations_, std::move(o));
        ASSERT_TRUE(api_->start());
    }
    void TearDown() override {
        if (api_) api_->stop();
        api_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A paired player with a certificate from `ca`, recorded in the database.
    PlayerCreds pair(const std::string& name, const tls::DeviceCa& ca, std::int64_t issue_at = 0,
                     int validity_days = 365) {
        auto c = db_->createPlayer(name, std::nullopt, "strict", 7 * 86400, now_);
        EXPECT_TRUE(c.has_value());
        PlayerCreds p;
        p.id = c->player.id;
        p.key_path = dir_ / (name + ".key");
        const std::string csr = makeCsr(p.key_path);
        tls::IssuedClientCert cert;
        std::string err;
        EXPECT_TRUE(ca.signClientCsr(csr, p.id, validity_days, cert, err, issue_at)) << err;
        p.serial = cert.serial_hex;
        p.cert_path = dir_ / (name + ".crt");
        write(p.cert_path, cert.cert_pem + cert.chain_pem);

        auto claim = db_->claimToken(p.id, c->token, now_);
        EXPECT_TRUE(claim.ok);
        CertRow row;
        row.serial_hex = cert.serial_hex; row.generation = cert.generation; row.player_id = p.id;
        row.issued_at = now_; row.not_before = cert.not_before_unix; row.not_after = cert.not_after_unix;
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, row, now_));
        return p;
    }
    PlayerCreds pair(const std::string& name) { return pair(name, *center_.ca); }

    using Client = std::unique_ptr<httplib::SSLClient>;   // SSLClient cannot be copied
    Client clientWith(const PlayerCreds& p) {
        auto c = std::make_unique<httplib::SSLClient>("127.0.0.1", api_->port(),
                                                      p.cert_path.c_str(), p.key_path.c_str());
        c->enable_server_certificate_verification(false);
        c->set_connection_timeout(3, 0);
        c->set_read_timeout(5, 0);
        return c;
    }
    Client anonymous() {
        auto c = std::make_unique<httplib::SSLClient>("127.0.0.1", api_->port());
        c->enable_server_certificate_verification(false);
        c->set_connection_timeout(3, 0);
        c->set_read_timeout(5, 0);
        return c;
    }

    static std::string path(const std::string& id, const char* tail) { return "/api/players/" + id + "/" + tail; }
    static std::string hb(const json& extra = json::object()) {
        json b = {{"schema", 1}, {"version", "0.3.5"}, {"uptime_seconds", 86412},
                  {"watchdog_state", "ok"}, {"health", "ok"}};
        for (auto it = extra.begin(); it != extra.end(); ++it) b[it.key()] = it.value();
        return b.dump();
    }
    static std::string errorOf(const httplib::Result& r) {
        if (!r) return "<no response>";
        auto j = json::parse(r->body, nullptr, false);
        return j.is_object() && j.contains("error") ? j["error"].get<std::string>() : "<no error field>";
    }
};

}  // namespace

// ── the happy path ──────────────────────────────────────────────────────────
TEST_F(DeviceApiTest, AHeartbeatWithAValidCertificateIsAccepted) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    EXPECT_TRUE(r->has_header("X-Server-Time"));
    EXPECT_EQ(r->get_header_value("X-Server-Time").size(), 20u);   // 2026-10-14T12:00:00Z

    auto st = db_->status(p.id);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(st->online);
    EXPECT_EQ(st->version, "0.3.5");
    EXPECT_EQ(st->updated_at, now_.load());
    auto payload = json::parse(st->payload_json);
    EXPECT_EQ(payload["uptime_seconds"], 86412);
    EXPECT_EQ(payload["watchdog_state"], "ok");
}

// ── the gate ────────────────────────────────────────────────────────────────
TEST_F(DeviceApiTest, WithoutACertificateEverythingIsRefused) {
    auto p = pair("Lobby");
    auto cli = anonymous();
    for (const std::string& pth : {path(p.id, "heartbeat"), std::string("/api/players"), std::string("/nothing")}) {
        auto r = cli->Patch(pth, hb(), "application/json");
        ASSERT_TRUE(r) << pth;
        EXPECT_EQ(r->status, 401) << pth;
        EXPECT_EQ(errorOf(r), "cert_untrusted") << pth;
        EXPECT_TRUE(r->has_header("X-Server-Time")) << "errors are stamped too";
    }
    EXPECT_FALSE(db_->status(p.id).has_value());
}

TEST_F(DeviceApiTest, ACertificateFromAnotherCenterIsUntrusted) {
    auto stranger = pair("Stranger", *other_center_.ca);
    auto cli = clientWith(stranger);
    auto r = cli->Patch(path(stranger.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_untrusted");
    EXPECT_FALSE(db_->status(stranger.id).has_value());
}

TEST_F(DeviceApiTest, AnExpiredCertificateSaysSo) {
    // Issued 3 years ago for one year.
    auto old = pair("Old", *center_.ca, now_ - 3LL * 365 * 86400, /*validity_days=*/365);
    auto cli = clientWith(old);
    auto r = cli->Patch(path(old.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_expired");
}

TEST_F(DeviceApiTest, ACertificateOfAnotherPlayerIsACnMismatch) {
    auto a = pair("A");
    auto b = pair("B");
    auto cli = clientWith(a);
    auto r = cli->Patch(path(b.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_EQ(errorOf(r), "cn_mismatch");
    EXPECT_FALSE(db_->status(b.id).has_value()) << "B must not be written by A's certificate";
}

TEST_F(DeviceApiTest, ADeletedPlayerIsNotFound) {
    auto p = pair("Gone");
    ASSERT_TRUE(db_->deletePlayer(p.id, "removed", now_));
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(errorOf(r), "player_not_found");
}

TEST_F(DeviceApiTest, RevocationThroughTheListWorksOnTheNextRequest) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);

    CertRow c;
    c.serial_hex = p.serial;
    revocations_.add({c});
    now_ += 11;
    auto cli2 = clientWith(p);                       // a fresh connection
    auto r = cli2->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_revoked");
}

TEST_F(DeviceApiTest, TheDatabaseRevokesEvenIfTheListWasNeverTold) {
    // The hook is a convenience; the database is the authority.
    auto p = pair("Lobby");
    ASSERT_TRUE(db_->revokePlayer(p.id, "stolen", now_));
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_revoked");
}

TEST_F(DeviceApiTest, ACertificateTheDatabaseNeverIssuedIsRefusedEvenIfTheChainIsFine) {
    // Valid chain, right CN, but a serial we have no record of (say, minted
    // from a copied CA key): not ours to accept.
    auto p = pair("Lobby");
    ASSERT_GT(db_->purgeExpiredCerts(now_ + 5000LL * 86400), 0u);   // drops the record
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_untrusted");
}

TEST_F(DeviceApiTest, NoRouteIsReachableWithoutACertificate) {
    // The structural guarantee: walk every registered route, call it without
    // a certificate, and see that none reaches its handler.
    const auto routes = api_->routes();
    ASSERT_FALSE(routes.empty());
    const std::string id = "11111111-2222-4333-8444-555555555555";
    auto cli = anonymous();
    for (const auto& route : routes) {
        const auto sp = route.find(' ');
        const std::string method = route.substr(0, sp);
        std::string pth = route.substr(sp + 1);
        for (std::string::size_type i; (i = pth.find("{id}")) != std::string::npos;) pth.replace(i, 4, id);
        httplib::Result r;
        if (method == "GET")        r = cli->Get(pth);
        else if (method == "PATCH") r = cli->Patch(pth, "{}", "application/json");
        else if (method == "POST")  r = cli->Post(pth, "{}", "application/json");
        else if (method == "PUT")   r = cli->Put(pth, "{}", "application/json");
        else if (method == "DELETE") r = cli->Delete(pth);
        else FAIL() << "unexpected method in route table: " << route;
        ASSERT_TRUE(r) << route;
        EXPECT_EQ(r->status, 401) << route;
    }
    EXPECT_EQ(api_->handlerInvocations(), 0u) << "a handler ran without authentication";
}

TEST_F(DeviceApiTest, UnknownPathsAndWrongMethodsDoNothingEvenWithACertificate) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    EXPECT_EQ(cli->Get("/api/players")->status, 404);
    EXPECT_EQ(cli->Get(path(p.id, "heartbeat"))->status, 404) << "heartbeat is PATCH only";
    EXPECT_EQ(cli->Get("/api/channels")->status, 404) << "the admin API does not exist on this port";
    EXPECT_FALSE(db_->status(p.id).has_value());
}

// ── heartbeat input ─────────────────────────────────────────────────────────
TEST_F(DeviceApiTest, HeartbeatFieldsAreCutAndCheckedNotTrusted) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    json extra = {{"current_source", std::string(5000, 's')},
                  {"last_error", std::string("bad\x01\x02 thing") + std::string(2000, 'e')},
                  {"last_renew_error", std::string(2000, 'r')},
                  {"uptime_seconds", "not a number"},
                  {"watchdog_state", "exploding"},
                  {"unknown_field", "dropped"},
                  {"buffer_progress", 7.5}};
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(extra), "application/json")->status, 204);
    auto payload = json::parse(db_->status(p.id)->payload_json);
    EXPECT_EQ(payload["current_source"].get<std::string>().size(), 256u);
    EXPECT_EQ(payload["last_error"].get<std::string>().size(), 512u);
    EXPECT_EQ(payload["last_renew_error"].get<std::string>().size(), 512u);
    EXPECT_EQ(payload["last_error"].get<std::string>().find('\x01'), std::string::npos) << "control characters out";
    EXPECT_FALSE(payload.contains("uptime_seconds") && payload["uptime_seconds"].is_string());
    EXPECT_NE(payload.value("watchdog_state", ""), "exploding");
    EXPECT_FALSE(payload.contains("unknown_field"));
    if (payload.contains("buffer_progress")) {
        EXPECT_LE(payload["buffer_progress"].get<double>(), 1.0);
        EXPECT_GE(payload["buffer_progress"].get<double>(), 0.0);
    }
}

TEST_F(DeviceApiTest, ABodyThatIsNotAnObjectIsRefused) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), "[1,2,3]", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(errorOf(r), "invalid_heartbeat");
    now_ += 11;
    auto cli2 = clientWith(p);
    r = cli2->Patch(path(p.id, "heartbeat"), "{ nope", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(DeviceApiTest, AnOversizedBodyIsRefusedBeforeItIsParsed) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    auto r = cli->Patch(path(p.id, "heartbeat"), std::string(200 * 1024, 'x'), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 413);
    EXPECT_FALSE(db_->status(p.id).has_value());
}

TEST_F(DeviceApiTest, HeartbeatsComingTooOftenAreSlowedDown) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
    auto cli2 = clientWith(p);
    auto r = cli2->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429);
    ASSERT_TRUE(r->has_header("Retry-After"));
    EXPECT_GT(std::stoi(r->get_header_value("Retry-After")), 0);

    now_ += 11;
    auto cli3 = clientWith(p);
    EXPECT_EQ(cli3->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
}

TEST_F(DeviceApiTest, AVersionChangeIsRecordedOnceAndAnnounced) {
    auto p = pair("Lobby");
    auto sub = bus_.subscribe();
    auto send = [&](const char* version) {
        now_ += 11;
        auto cli = clientWith(p);
        return cli->Patch(path(p.id, "heartbeat"), hb({{"version", version}}), "application/json")->status;
    };
    ASSERT_EQ(send("0.3.5"), 204);
    ASSERT_EQ(send("0.3.5"), 204);
    ASSERT_EQ(send("0.3.6"), 204);

    int changes = 0;
    for (const auto& e : db_->events(p.id, 50)) changes += (e.kind == "version_changed");
    EXPECT_EQ(changes, 1) << "the first report is not a change, a repeat is not a change";

    int announced = 0;
    for (const auto& e : sub->drain(std::chrono::milliseconds(50)))
        if (e.type == liveqx::events::EventType::PlayerVersionChanged) {
            ++announced;
            EXPECT_EQ(e.payload["from"], "0.3.5");
            EXPECT_EQ(e.payload["to"], "0.3.6");
            EXPECT_EQ(e.payload["player_id"], p.id);
        }
    EXPECT_EQ(announced, 1);
}

// ── renewal over mTLS ───────────────────────────────────────────────────────
TEST_F(DeviceApiTest, RenewTradesACsrForACertificateThatWorksStraightAway) {
    auto p = pair("Lobby", *center_.ca, 0, /*validity_days=*/30);   // inside the 90-day window
    const fs::path new_key = dir_ / "Lobby.new.key";
    auto cli = clientWith(p);
    auto r = cli->Post(path(p.id, "renew"), json{{"schema", 1}, {"csr_pem", makeCsr(new_key)}}.dump(),
                       "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_TRUE(r->has_header("X-Server-Time"));
    auto b = json::parse(r->body);
    EXPECT_EQ(b["ca_cert_pem"], "SERVER-ROOT-PEM");
    ASSERT_TRUE(b.contains("client_chain_pem"));

    // The new pair connects and is accepted: the certificate really chains.
    PlayerCreds renewed = p;
    renewed.key_path  = new_key;
    renewed.cert_path = dir_ / "Lobby.new.crt";
    write(renewed.cert_path, b["client_cert_pem"].get<std::string>() + b["client_chain_pem"].get<std::string>());
    auto cli2 = clientWith(renewed);
    EXPECT_EQ(cli2->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);

    // ...and so does the old one, until it expires.
    now_ += 11;
    auto cli3 = clientWith(p);
    EXPECT_EQ(cli3->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
}

TEST_F(DeviceApiTest, RenewTooEarlyIsAConflict) {
    auto p = pair("Lobby");                                          // 365 days left
    auto cli = clientWith(p);
    auto r = cli->Post(path(p.id, "renew"), json{{"schema", 1}, {"csr_pem", makeCsr(dir_ / "x.key")}}.dump(),
                       "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(errorOf(r), "renew_too_early");
}

TEST_F(DeviceApiTest, RenewWithoutACsrIsABadRequest) {
    auto p = pair("Lobby", *center_.ca, 0, 30);
    auto cli = clientWith(p);
    auto r = cli->Post(path(p.id, "renew"), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(errorOf(r), "invalid_csr");
}

TEST_F(DeviceApiTest, ARevokedPlayerCannotRenewEitherBecauseTheGateComesFirst) {
    auto p = pair("Lobby", *center_.ca, 0, 30);
    ASSERT_TRUE(db_->revokePlayer(p.id, "stolen", now_));
    auto cli = clientWith(p);
    auto r = cli->Post(path(p.id, "renew"), json{{"csr_pem", makeCsr(dir_ / "y.key")}}.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_revoked");
    EXPECT_EQ(db_->certsOf(p.id).size(), 1u);
}

// ── trust that arrives later ────────────────────────────────────────────────
TEST_F(DeviceApiTest, TheDeviceCaCanAppearWhileTheServerIsRunning) {
    // The server starts before the root ceremony: nothing is trusted yet.
    auto p = pair("Lobby");
    api_->stop();
    DeviceApiOptions o;
    o.bind = "127.0.0.1";
    o.cert_path = dir_ / "server_ca" / "server.crt";
    o.key_path  = dir_ / "server_ca" / "server.key";
    o.now = [this] { return now_.load(); };
    api_ = std::make_unique<DeviceApi>(*db_, revocations_, std::move(o));
    ASSERT_TRUE(api_->start()) << "an empty trust store must not stop the port from starting";

    auto before = clientWith(p);
    auto r = before->Patch(path(p.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(errorOf(r), "cert_untrusted");

    // The ceremony finishes; no restart.
    EXPECT_GE(api_->addTrustAnchors(center_.ca->rootPem() + center_.ca->chainPem()), 2u);
    auto after = clientWith(p);
    EXPECT_EQ(after->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
    EXPECT_EQ(api_->addTrustAnchors("not a pem"), 0u);
}

