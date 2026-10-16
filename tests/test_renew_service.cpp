// Unit tests for players/RenewService — certificate renewal (A5): a player that
// still holds a valid certificate trades a new CSR for a fresh certificate.
//
// What these tests protect:
//   * renewal opens 90 days before the end (the player starts at 60), not
//     earlier: a fresh certificate gets 409 renew_too_early;
//   * the answer carries the new certificate, the intermediate when there is
//     one, and the current server-TLS root (so a rotated root reaches players);
//   * the CN is always the player id, and the old certificate is NOT revoked
//     by renewing (a player that crashes before storing the new one must still
//     get in; compromise is handled by revoking the player);
//   * a bad CSR, a missing center or an unsynced server clock change nothing;
//   * a certificate the database does not know, or a player that is not
//     active, cannot renew.

#include "auth/MasterKey.h"
#include "players/RenewService.h"
#include "utils/DeviceCa.h"

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <unistd.h>

#include <atomic>
#include <ctime>
#include <filesystem>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
using namespace liveqx::players;

namespace {

std::string makeCsr() {
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

std::string subjectCn(const std::string& cert_pem) {
    BIO* b = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    if (!x) return {};
    char buf[256]{};
    X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, buf, sizeof buf);
    X509_free(x);
    return buf;
}

constexpr std::int64_t kDay = 86400;

class RenewServiceTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<liveqx::auth::MasterKey> mk_;
    std::unique_ptr<PlayersDb> db_;
    std::shared_ptr<const tls::DeviceCa> ca_;
    std::optional<std::int64_t> clock_offset_s_;
    std::int64_t now_{0};
    std::unique_ptr<RenewService> svc_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-renew-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        now_ = std::time(nullptr);
        mk_ = std::make_unique<liveqx::auth::MasterKey>((dir_ / "master.key").string());
        ASSERT_TRUE(mk_->load());
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        std::string err;
        auto ca = tls::DeviceCa::open(dir_ / "device_ca", *mk_, err, true);
        ASSERT_TRUE(ca) << err;
        ca_ = std::shared_ptr<const tls::DeviceCa>(std::move(ca));

        RenewOptions o;
        o.ca             = [this] { return ca_; };
        o.clock_offset_s = [this] { return clock_offset_s_; };
        o.ca_cert_pem    = [] { return std::string("SERVER-ROOT-PEM"); };
        o.now            = [this] { return now_; };
        svc_ = std::make_unique<RenewService>(*db_, std::move(o));
    }
    void TearDown() override {
        svc_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    struct Paired { std::string id; std::string serial; };
    // An active player whose current certificate lives `days_valid` more days.
    Paired pair(int days_valid) {
        auto c = db_->createPlayer("Lobby", std::nullopt, "strict", 7 * kDay, now_);
        EXPECT_TRUE(c);
        tls::IssuedClientCert cert;
        std::string err;
        // Issued "now" for exactly `days_valid` days, so notAfter = now + days_valid.
        EXPECT_TRUE(ca_->signClientCsr(makeCsr(), c->player.id, days_valid, cert, err, now_)) << err;
        auto claim = db_->claimToken(c->player.id, c->token, now_);
        EXPECT_TRUE(claim.ok);
        CertRow row;
        row.serial_hex = cert.serial_hex; row.generation = cert.generation; row.player_id = c->player.id;
        row.issued_at = now_; row.not_before = cert.not_before_unix; row.not_after = cert.not_after_unix;
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, row, now_));
        return {c->player.id, cert.serial_hex};
    }
};

}  // namespace

TEST_F(RenewServiceTest, ANearlyExpiredCertificateIsRenewed) {
    auto p = pair(60);
    auto out = svc_->renew({p.id, p.serial, makeCsr()});
    ASSERT_EQ(out.status, 200) << out.body.dump();
    EXPECT_EQ(out.body["schema"], 1);
    EXPECT_EQ(subjectCn(out.body["client_cert_pem"]), p.id);
    EXPECT_EQ(out.body["ca_cert_pem"], "SERVER-ROOT-PEM") << "the current root travels with every renewal";
    EXPECT_EQ(out.audit_action, "player.renew");

    auto certs = db_->certsOf(p.id);
    ASSERT_EQ(certs.size(), 2u);
    EXPECT_NE(certs[0].serial_hex, certs[1].serial_hex);
    for (const auto& c : certs) EXPECT_FALSE(c.revoked_at.has_value()) << "the old one stays valid";
    bool renewed = false;
    for (const auto& e : db_->events(p.id, 20)) renewed |= (e.kind == "renewed");
    EXPECT_TRUE(renewed);
}

TEST_F(RenewServiceTest, TheWindowOpens90DaysBeforeTheEnd) {
    auto early = pair(365);
    auto out = svc_->renew({early.id, early.serial, makeCsr()});
    EXPECT_EQ(out.status, 409);
    EXPECT_EQ(out.body["error"], "renew_too_early");
    EXPECT_EQ(db_->certsOf(early.id).size(), 1u);

    auto edge_open = pair(90);
    EXPECT_EQ(svc_->renew({edge_open.id, edge_open.serial, makeCsr()}).status, 200);
    auto edge_closed = pair(91);
    EXPECT_EQ(svc_->renew({edge_closed.id, edge_closed.serial, makeCsr()}).status, 409);
}

TEST_F(RenewServiceTest, ABadCsrChangesNothing) {
    auto p = pair(30);
    for (const std::string& csr : {std::string(""), std::string("garbage"), std::string(5000, 'A')}) {
        auto out = svc_->renew({p.id, p.serial, csr});
        EXPECT_EQ(out.status, 400);
        EXPECT_EQ(out.body["error"], "invalid_csr");
    }
    EXPECT_EQ(db_->certsOf(p.id).size(), 1u);
}

TEST_F(RenewServiceTest, WithoutACenterNothingIsIssued) {
    auto p = pair(30);
    auto saved = ca_;
    ca_.reset();
    auto out = svc_->renew({p.id, p.serial, makeCsr()});
    EXPECT_EQ(out.status, 503);
    EXPECT_EQ(out.body["error"], "ca_unavailable");
    ca_ = saved;
    EXPECT_EQ(svc_->renew({p.id, p.serial, makeCsr()}).status, 200);
}

TEST_F(RenewServiceTest, AnUnsyncedServerClockStopsRenewalToo) {
    auto p = pair(30);
    clock_offset_s_ = 3 * kDay;
    auto out = svc_->renew({p.id, p.serial, makeCsr()});
    EXPECT_EQ(out.status, 503);
    EXPECT_EQ(out.body["error"], "server_clock_unsynced");
    EXPECT_EQ(db_->certsOf(p.id).size(), 1u);
}

TEST_F(RenewServiceTest, ACertificateTheDatabaseDoesNotKnowCannotRenew) {
    auto p = pair(30);
    auto out = svc_->renew({p.id, "DEADBEEF", makeCsr()});
    EXPECT_EQ(out.status, 401);
    EXPECT_EQ(out.body["error"], "cert_untrusted");
}

TEST_F(RenewServiceTest, ARevokedPlayerCannotRenew) {
    auto p = pair(30);
    ASSERT_TRUE(db_->revokePlayer(p.id, "x", now_));
    auto out = svc_->renew({p.id, p.serial, makeCsr()});
    EXPECT_NE(out.status, 200);
    EXPECT_EQ(db_->certsOf(p.id).size(), 1u);
}
