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

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

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
    std::mutex audit_mu_;
    std::vector<DeviceAuditEvent> audit_;
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
        o.audit      = [this](const DeviceAuditEvent& e) {
            std::lock_guard<std::mutex> lk(audit_mu_);
            audit_.push_back(e);
        };
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

    std::vector<DeviceAuditEvent> auditSnapshot() {
        std::lock_guard<std::mutex> lk(audit_mu_);
        return audit_;
    }

    // Serial number of the certificate the port presents right now.
    std::string serverSerial() {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        BIO* bio = BIO_new_ssl_connect(ctx);
        SSL* ssl = nullptr;
        BIO_get_ssl(bio, &ssl);
        BIO_set_conn_hostname(bio, ("127.0.0.1:" + std::to_string(api_->port())).c_str());
        std::string out;
        if (BIO_do_connect(bio) > 0) {
            if (X509* c = SSL_get1_peer_certificate(ssl)) {
                BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(c), nullptr);
                char* hex = BN_bn2hex(bn);
                out = hex ? hex : "";
                OPENSSL_free(hex);
                BN_free(bn);
                X509_free(c);
            }
        }
        BIO_free_all(bio);
        SSL_CTX_free(ctx);
        return out;
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

// ── audit of the device port ────────────────────────────────────────────────
TEST_F(DeviceApiTest, ARefusalIsAuditedAsAnAccessEventWithItsReason) {
    auto p = pair("Lobby");
    ASSERT_TRUE(db_->revokePlayer(p.id, "stolen", now_));
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 401);

    auto a = auditSnapshot();
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].action, "player.device_denied");
    EXPECT_TRUE(a[0].access_denial);
    EXPECT_EQ(a[0].player_id, p.id);
    EXPECT_EQ(a[0].reason, "cert_revoked");
    EXPECT_EQ(a[0].http_status, 401);
    EXPECT_EQ(a[0].client_ip, "127.0.0.1");
}

TEST_F(DeviceApiTest, ARefusalWithoutAnyIdentityIsAuditedByAddress) {
    auto cli = anonymous();
    ASSERT_EQ(cli->Patch(path("11111111-2222-4333-8444-555555555555", "heartbeat"), "{}", "application/json")->status, 401);
    auto a = auditSnapshot();
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].reason, "cert_untrusted");
    EXPECT_EQ(a[0].client_ip, "127.0.0.1");
}

TEST_F(DeviceApiTest, ARepeatedRefusalIsOneRowPerWindowWithACounter) {
    auto p = pair("Lobby");
    ASSERT_TRUE(db_->revokePlayer(p.id, "stolen", now_));
    for (int i = 0; i < 5; ++i) {
        auto cli = clientWith(p);
        ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 401);
    }
    EXPECT_EQ(auditSnapshot().size(), 1u) << "a revoked screen retrying must not flood the log";

    now_ += 601;                                    // the window passes
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 401);
    auto a = auditSnapshot();
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[1].suppressed, 4) << "the next row says how many were folded in";
}

TEST_F(DeviceApiTest, DifferentReasonsAreNotFoldedTogether) {
    auto a = pair("A");
    auto b = pair("B");
    ASSERT_TRUE(db_->revokePlayer(a.id, "x", now_));
    auto ca = clientWith(a);
    ASSERT_EQ(ca->Patch(path(a.id, "heartbeat"), hb(), "application/json")->status, 401);   // cert_revoked
    auto cb = clientWith(b);
    ASSERT_EQ(cb->Patch(path(a.id, "heartbeat"), hb(), "application/json")->status, 403);   // cn_mismatch
    EXPECT_EQ(auditSnapshot().size(), 2u);
}

TEST_F(DeviceApiTest, NormalHeartbeatsAreNotAudited) {
    auto p = pair("Lobby");
    for (int i = 0; i < 3; ++i) {
        now_ += 11;
        auto cli = clientWith(p);
        ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
    }
    EXPECT_TRUE(auditSnapshot().empty());
}

TEST_F(DeviceApiTest, ARenewalIsAuditedEveryTimeAndATooEarlyOneAsADenial) {
    auto early = pair("Early");                                       // 365 days left
    auto ce = clientWith(early);
    ASSERT_EQ(ce->Post(path(early.id, "renew"), json{{"csr_pem", makeCsr(dir_ / "e.key")}}.dump(), "application/json")->status, 409);

    auto ready = pair("Ready", *center_.ca, 0, 30);
    auto cr = clientWith(ready);
    ASSERT_EQ(cr->Post(path(ready.id, "renew"), json{{"csr_pem", makeCsr(dir_ / "r.key")}}.dump(), "application/json")->status, 200);

    auto a = auditSnapshot();
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].action, "player.renew_denied");
    EXPECT_TRUE(a[0].access_denial);
    EXPECT_EQ(a[0].reason.find("not_after="), 0u);
    EXPECT_EQ(a[1].action, "player.renew");
    EXPECT_FALSE(a[1].access_denial);
    EXPECT_EQ(a[1].player_id, ready.id);
}

// ── a re-issued server certificate ──────────────────────────────────────────
TEST_F(DeviceApiTest, AReissuedServerCertificateIsPickedUpOnTheSamePortWithTheSameTrust) {
    auto p = pair("Lobby");
    const int port_before = api_->port();
    const std::string before = serverSerial();
    ASSERT_FALSE(before.empty());

    // The administrator regenerates the server certificate (new serial, same files).
    const fs::path srv = dir_ / "server_ca";
    tls::SanList sans;
    sans.dns_names = {"localhost"};
    sans.ip_v4 = {"127.0.0.1"};
    ASSERT_TRUE(tls::issueServerCert(srv, "localhost", sans).ok());

    ASSERT_TRUE(api_->reloadServerCertificate());
    EXPECT_EQ(api_->port(), port_before) << "players are configured with this port";
    const std::string after = serverSerial();
    EXPECT_FALSE(after.empty());
    EXPECT_NE(after, before) << "the new certificate is the one presented";

    auto cli = clientWith(p);
    EXPECT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204)
        << "client certificates are still trusted after the reload";
}

TEST_F(DeviceApiTest, ABrokenCertificatePairDoesNotTakeThePortDown) {
    auto p = pair("Lobby");
    const std::string before = serverSerial();
    write(dir_ / "server_ca" / "server.key", "not a key");
    EXPECT_FALSE(api_->reloadServerCertificate());
    EXPECT_EQ(serverSerial(), before) << "the old certificate keeps being served";
    auto cli = clientWith(p);
    EXPECT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
}

// ── a thousand players ──────────────────────────────────────────────────────
// B5: a fleet of 1000 screens sends one heartbeat a minute, so roughly 17 new
// connections a second, each a full TLS handshake with a client certificate
// (one request per connection). Here all 1000 arrive at once from 16 client
// threads — a restart of the server is the worst moment in real life.
TEST_F(DeviceApiTest, AThousandPlayersCanAllSendAHeartbeatAtOnce) {
    constexpr int kPlayers = 1000;
    constexpr int kClientThreads = 16;

    const fs::path shared_key = dir_ / "load.key";
    const std::string csr = makeCsr(shared_key);                   // one key pair; the certificates still differ
    const std::string key_pem = slurp(shared_key);

    struct Sim { std::string id; std::string cert_pem; };
    std::vector<Sim> sims;
    sims.reserve(kPlayers);
    const auto t_setup = std::chrono::steady_clock::now();
    for (int i = 0; i < kPlayers; ++i) {
        auto c = db_->createPlayer("P" + std::to_string(i), std::nullopt, "strict", 7 * 86400, now_);
        ASSERT_TRUE(c);
        tls::IssuedClientCert cert;
        std::string err;
        ASSERT_TRUE(center_.ca->signClientCsr(csr, c->player.id, 365, cert, err)) << err;
        auto claim = db_->claimToken(c->player.id, c->token, now_);
        ASSERT_TRUE(claim.ok);
        CertRow row;
        row.serial_hex = cert.serial_hex; row.generation = cert.generation; row.player_id = c->player.id;
        row.issued_at = now_; row.not_before = cert.not_before_unix; row.not_after = cert.not_after_unix;
        ASSERT_TRUE(db_->finishEnrollment(claim.token_id, row, now_));
        sims.push_back({c->player.id, cert.cert_pem});
    }
    const auto setup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_setup).count();

    std::atomic<int> next{0}, ok{0}, failed{0};
    std::vector<long> latencies_ms(kPlayers, -1);
    const int port = api_->port();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < kClientThreads; ++t)
        threads.emplace_back([&] {
            for (int i; (i = next.fetch_add(1)) < kPlayers;) {
                BIO* cb = BIO_new_mem_buf(sims[i].cert_pem.data(), static_cast<int>(sims[i].cert_pem.size()));
                X509* cert = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
                BIO_free_all(cb);
                BIO* kb = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
                EVP_PKEY* key = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
                BIO_free_all(kb);
                httplib::SSLClient cli("127.0.0.1", port, cert, key);
                X509_free(cert);
                EVP_PKEY_free(key);
                cli.enable_server_certificate_verification(false);
                cli.set_connection_timeout(10, 0);
                cli.set_read_timeout(20, 0);
                const auto s = std::chrono::steady_clock::now();
                auto r = cli.Patch("/api/players/" + sims[i].id + "/heartbeat", hb(), "application/json");
                latencies_ms[i] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s).count();
                (r && r->status == 204 ? ok : failed).fetch_add(1);
            }
        });
    for (auto& t : threads) t.join();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::sort(latencies_ms.begin(), latencies_ms.end());
    std::cout << "[load] setup " << setup_ms << " ms; " << kPlayers << " heartbeats in " << total_ms
              << " ms (" << (kPlayers * 1000.0 / std::max<long>(total_ms, 1)) << "/s); latency p50="
              << latencies_ms[kPlayers / 2] << " ms p95=" << latencies_ms[kPlayers * 95 / 100]
              << " ms max=" << latencies_ms.back() << " ms\n";

    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(ok.load(), kPlayers);
    EXPECT_EQ(db_->allStatuses().size(), static_cast<std::size_t>(kPlayers));
    EXPECT_EQ(api_->handlerInvocations(), static_cast<std::size_t>(kPlayers));
    // The fleet's real rate is ~17/s; here it is a burst, so the bar is generous
    // but still proves the queue does not stall.
    EXPECT_LT(total_ms, 60'000);
}

// ── optional heartbeat fields of the command-confirmation contract ─────────
TEST_F(DeviceApiTest, ConfirmationFieldsAreKeptWhenWellFormedAndDroppedWhenNot) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    const std::string cmd = "11111111-2222-4333-8444-555555555555";
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"),
                         hb({{"os_uptime_seconds", 1234}, {"last_command_id", cmd},
                             {"last_command_result", {{"id", cmd}, {"status", "executed"}}}}),
                         "application/json")->status, 204);
    auto payload = json::parse(db_->status(p.id)->payload_json);
    EXPECT_EQ(payload["os_uptime_seconds"], 1234);
    EXPECT_EQ(payload["last_command_id"], cmd);
    EXPECT_EQ(payload["last_command_result"], (json{{"id", cmd}, {"status", "executed"}}));
    EXPECT_FALSE(payload.contains("startup")) << "startup is a one-off marker, not part of the stored state";

    now_ += 11;
    auto cli2 = clientWith(p);
    ASSERT_EQ(cli2->Patch(path(p.id, "heartbeat"),
                          hb({{"os_uptime_seconds", -5},
                              {"last_command_result", {{"id", "not-a-uuid"}, {"status", "executed"}}}}),
                          "application/json")->status, 204);
    payload = json::parse(db_->status(p.id)->payload_json);
    EXPECT_FALSE(payload.contains("os_uptime_seconds")) << "negative uptime is nonsense";
    EXPECT_FALSE(payload.contains("last_command_result")) << "a result must name a real command id";

    now_ += 11;
    auto cli3 = clientWith(p);
    ASSERT_EQ(cli3->Patch(path(p.id, "heartbeat"),
                          hb({{"last_command_result", {{"id", cmd}, {"status", "melted"}}}}),
                          "application/json")->status, 204);
    EXPECT_FALSE(json::parse(db_->status(p.id)->payload_json).contains("last_command_result"))
        << "an unknown status is dropped, not stored";

    now_ += 11;
    auto cli4 = clientWith(p);
    ASSERT_EQ(cli4->Patch(path(p.id, "heartbeat"), hb({{"last_command_result", "executed"}}), "application/json")->status, 204);
    EXPECT_FALSE(json::parse(db_->status(p.id)->payload_json).contains("last_command_result")) << "wrong type";
}

TEST_F(DeviceApiTest, AStartupHeartbeatMayFollowAnOrdinaryOneImmediately) {
    // A player that restarts within the 10 s spacing must still be heard.
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);

    now_ += 2;                                       // far inside the normal spacing
    auto cli2 = clientWith(p);
    EXPECT_EQ(cli2->Patch(path(p.id, "heartbeat"), hb({{"startup", true}}), "application/json")->status, 204);

    int started = 0;
    for (const auto& e : db_->events(p.id, 20)) started += (e.kind == "started");
    EXPECT_EQ(started, 1);
}

TEST_F(DeviceApiTest, StartupHeartbeatsHaveTheirOwnLimitSoTheFlagIsNoLoophole) {
    auto p = pair("Lobby");
    auto send = [&](bool startup) {
        auto cli = clientWith(p);
        return cli->Patch(path(p.id, "heartbeat"), hb({{"startup", startup}}), "application/json");
    };
    ASSERT_EQ(send(true)->status, 204);

    now_ += 2;
    auto again = send(true);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 429) << "a player cannot flood by saying it just started";
    ASSERT_TRUE(again->has_header("Retry-After"));
    EXPECT_GT(std::stoi(again->get_header_value("Retry-After")), 0);

    now_ += 29;                                      // 31 s since the first startup heartbeat
    EXPECT_EQ(send(true)->status, 204);
}

TEST_F(DeviceApiTest, AStartupFlagThatIsNotABooleanIsJustAnOrdinaryHeartbeat) {
    auto p = pair("Lobby");
    auto cli = clientWith(p);
    ASSERT_EQ(cli->Patch(path(p.id, "heartbeat"), hb(), "application/json")->status, 204);
    now_ += 2;
    auto cli2 = clientWith(p);
    auto r = cli2->Patch(path(p.id, "heartbeat"), hb({{"startup", "yes"}}), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429) << "only a real true earns the exemption";
}

// ── the server's own certificate for the broker is not a player ─────────────
TEST_F(DeviceApiTest, TheServersOwnBrokerCertificateGetsNoFurtherThanTheGate) {
    // It chains to the same device root, so TLS accepts it — which makes the
    // gate's player check the thing that matters.
    tls::BrokerIdentity ident;
    std::string err;
    ASSERT_TRUE(tls::DeviceCa::ensureBrokerIdentity(dir_ / "c1" / "ca", *mk_, *center_.ca, ident, err)) << err;
    PlayerCreds svc;
    svc.id = "liveqx-server";
    svc.cert_path = dir_ / "svc.crt";
    svc.key_path  = dir_ / "svc.key";
    write(svc.cert_path, ident.cert_chain_pem);
    write(svc.key_path, ident.key_pem);

    auto own = clientWith(svc);
    auto r = own->Patch(path(svc.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404) << "there is no such player";
    EXPECT_EQ(errorOf(r), "player_not_found");

    auto victim = pair("Lobby");
    auto other = clientWith(svc);
    auto r2 = other->Patch(path(victim.id, "heartbeat"), hb(), "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 403) << "and it cannot speak for a real player either";
    EXPECT_EQ(errorOf(r2), "cn_mismatch");
    EXPECT_FALSE(db_->status(victim.id).has_value());
}

