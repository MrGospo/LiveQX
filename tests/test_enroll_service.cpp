// Unit tests for players/EnrollService — the pairing step (A4): a player with a
// one-time token and a CSR gets a client certificate.
//
// What these tests protect:
//   * success: certificate with CN = player_id (the CN in the CSR is ignored),
//     the player becomes active, the certificate is recorded, the token burns;
//   * every token failure (wrong, unknown player, expired, used, revoked
//     player, malformed id) is the same 401 invalid_token to the caller, the
//     reason only goes to the audit fields;
//   * a bad CSR does not burn the token (the player can retry);
//   * no CA / unsynced server clock: 503 and the token is NOT burnt;
//   * guessing is rate limited per address and per player, and a locked
//     address is refused before the token is even looked at;
//   * in two-tier mode the intermediate comes back as client_chain_pem.

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "players/EnrollService.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <unistd.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
namespace kw  = liveqx::auth::keywrap;
using namespace liveqx::players;
using nlohmann::json;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A CSR with the given subject CN (empty = none), P-256 / SHA-256 as in A10.
std::string makeCsr(const std::string& cn = "") {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_pubkey(req, key);
    if (!cn.empty()) {
        X509_NAME* n = X509_REQ_get_subject_name(req);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    }
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

class EnrollServiceTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<liveqx::auth::MasterKey> mk_;
    std::unique_ptr<PlayersDb> db_;
    std::shared_ptr<const tls::DeviceCa> ca_;
    std::optional<std::int64_t> clock_offset_s_;
    std::int64_t now_{0};
    std::unique_ptr<EnrollService> svc_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-enroll-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        now_ = std::time(nullptr);
        mk_ = std::make_unique<liveqx::auth::MasterKey>((dir_ / "master.key").string());
        ASSERT_TRUE(mk_->load());
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        std::string err;
        auto ca = tls::DeviceCa::open(dir_ / "device_ca", *mk_, err, /*allow_create=*/true);
        ASSERT_TRUE(ca) << err;
        ca_ = std::shared_ptr<const tls::DeviceCa>(std::move(ca));
        makeService();
    }
    void TearDown() override {
        svc_.reset();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void makeService() {
        EnrollOptions o;
        o.ca             = [this] { return ca_; };
        o.clock_offset_s = [this] { return clock_offset_s_; };
        o.now            = [this] { return now_; };
        svc_ = std::make_unique<EnrollService>(*db_, std::move(o));
    }

    CreatedPlayer newPlayer(const std::string& name = "Lobby") {
        auto c = db_->createPlayer(name, std::nullopt, "strict", 7 * 86400, now_);
        EXPECT_TRUE(c.has_value());
        return *c;
    }

    EnrollRequest request(const CreatedPlayer& p, const std::string& csr = makeCsr(),
                          const std::string& ip = "10.0.0.5") {
        return {p.player.id, p.token, csr, ip};
    }
};

}  // namespace

TEST_F(EnrollServiceTest, SuccessIssuesACertificateForThePlayerId) {
    auto p = newPlayer();
    auto out = svc_->enroll(request(p, makeCsr("evil-cn-chosen-by-the-client")));
    ASSERT_EQ(out.status, 200) << out.body.dump();
    EXPECT_EQ(out.body["schema"], 1);
    const std::string cert = out.body["client_cert_pem"];
    EXPECT_EQ(subjectCn(cert), p.player.id) << "the CN comes from the server, not from the CSR";
    EXPECT_FALSE(out.body.contains("client_chain_pem")) << "single-tier CA has no intermediate";
    EXPECT_EQ(out.audit_action, "player.enroll");
    EXPECT_EQ(out.player_id, p.player.id);

    EXPECT_EQ(db_->findPlayer(p.player.id)->state, PlayerState::Active);
    auto certs = db_->certsOf(p.player.id);
    ASSERT_EQ(certs.size(), 1u);
    EXPECT_EQ(certs[0].generation, ca_->generation());
    EXPECT_FALSE(certs[0].serial_hex.empty());
    bool enrolled = false;
    for (const auto& e : db_->events(p.player.id, 10)) enrolled |= (e.kind == "enrolled");
    EXPECT_TRUE(enrolled);
}

TEST_F(EnrollServiceTest, TheTokenWorksOnlyOnce) {
    auto p = newPlayer();
    ASSERT_EQ(svc_->enroll(request(p)).status, 200);
    auto again = svc_->enroll(request(p));
    EXPECT_EQ(again.status, 401);
    EXPECT_EQ(again.body["error"], "invalid_token");
}

TEST_F(EnrollServiceTest, EveryTokenFailureLooksTheSameToTheCaller) {
    auto p = newPlayer();
    auto expired = db_->createPlayer("Old", std::nullopt, "strict", 60, now_ - 3600);
    ASSERT_TRUE(expired);
    auto revoked = newPlayer("Gone");
    ASSERT_TRUE(db_->revokePlayer(revoked.player.id, "x", now_));

    std::vector<EnrollRequest> bad = {
        {p.player.id, "wrong-token", makeCsr(), "10.1.0.1"},
        {"00000000-0000-4000-8000-000000000000", p.token, makeCsr(), "10.1.0.2"},
        {expired->player.id, expired->token, makeCsr(), "10.1.0.3"},
        {revoked.player.id, revoked.token, makeCsr(), "10.1.0.4"},
        {"not-a-uuid", p.token, makeCsr(), "10.1.0.5"},
        {"", "", makeCsr(), "10.1.0.6"},
    };
    std::set<std::string> reasons;
    for (const auto& r : bad) {
        auto out = svc_->enroll(r);
        EXPECT_EQ(out.status, 401) << r.player_id;
        EXPECT_EQ(out.body, (json{{"error", "invalid_token"}})) << "no hint about the reason";
        EXPECT_EQ(out.audit_action, "player.enroll_denied");
        EXPECT_FALSE(out.audit_reason.empty()) << "the audit trail keeps the real reason";
        reasons.insert(out.audit_reason);
    }
    EXPECT_GE(reasons.size(), 3u) << "the audit distinguishes what the caller cannot";
}

TEST_F(EnrollServiceTest, ABadCsrDoesNotBurnTheToken) {
    auto p = newPlayer();
    auto garbage = svc_->enroll(request(p, "-----BEGIN CERTIFICATE REQUEST-----\nzzz\n-----END CERTIFICATE REQUEST-----\n"));
    EXPECT_EQ(garbage.status, 400);
    EXPECT_EQ(garbage.body["error"], "invalid_csr");
    EXPECT_EQ(svc_->enroll(request(p, "")).status, 400) << "no CSR at all";
    EXPECT_EQ(svc_->enroll(request(p, std::string(5000, 'A'))).status, 400) << "A10: at most 4096 bytes";

    EXPECT_EQ(db_->findPlayer(p.player.id)->state, PlayerState::Pending);
    EXPECT_EQ(svc_->enroll(request(p)).status, 200) << "the same token still works";
}

TEST_F(EnrollServiceTest, WithoutACenterTheAnswerIs503AndTheTokenSurvives) {
    auto p = newPlayer();
    auto saved = ca_;
    ca_.reset();
    auto out = svc_->enroll(request(p));
    EXPECT_EQ(out.status, 503);
    EXPECT_EQ(out.body["error"], "ca_unavailable");
    ca_ = saved;
    EXPECT_EQ(svc_->enroll(request(p)).status, 200);
}

TEST_F(EnrollServiceTest, AnUnsyncedServerClockStopsIssuingButOnlyWhenKnown) {
    auto p = newPlayer();
    clock_offset_s_ = 2 * 86400;                       // OS clock 2 days off NTP
    auto out = svc_->enroll(request(p));
    EXPECT_EQ(out.status, 503);
    EXPECT_EQ(out.body["error"], "server_clock_unsynced");
    EXPECT_EQ(db_->findPlayer(p.player.id)->state, PlayerState::Pending);

    clock_offset_s_ = -2 * 86400;                      // the sign does not matter
    EXPECT_EQ(svc_->enroll(request(p)).status, 503);

    clock_offset_s_ = 30;                              // small drift is fine
    EXPECT_EQ(svc_->enroll(request(p)).status, 200) << "and the token was not burnt before";
}

TEST_F(EnrollServiceTest, UnknownClockOffsetDoesNotBlock) {
    auto p = newPlayer();
    clock_offset_s_.reset();                           // no NTP configured / not measured
    EXPECT_EQ(svc_->enroll(request(p)).status, 200);
}

TEST_F(EnrollServiceTest, GuessingIsLimitedPerAddress) {
    auto p = newPlayer();
    // Distinct (bogus) player ids from one address: only the address limit applies.
    for (int i = 0; i < 3; ++i) {
        auto out = svc_->enroll({"00000000-0000-4000-8000-00000000000" + std::to_string(i), "guess",
                                 makeCsr(), "203.0.113.9"});
        EXPECT_EQ(out.status, 401);
    }
    auto locked = svc_->enroll({p.player.id, p.token, makeCsr(), "203.0.113.9"});
    EXPECT_EQ(locked.status, 429) << "refused before the token is even looked at";
    EXPECT_EQ(locked.body["error"], "rate_limited");
    EXPECT_GT(locked.retry_after_s, 0);
    EXPECT_EQ(db_->findPlayer(p.player.id)->state, PlayerState::Pending);

    // Another address is not affected.
    EXPECT_EQ(svc_->enroll({p.player.id, p.token, makeCsr(), "198.51.100.7"}).status, 200);
}

TEST_F(EnrollServiceTest, AStrangerCannotLockAPlayerOutByKnowingItsId) {
    // The id is not a secret (it is in the list, the package and the CN), the
    // token has 256 bits. Throttling per id would only let a third party stop
    // a legitimate pairing, so the limit is per address alone.
    auto p = newPlayer();
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(svc_->enroll({p.player.id, "guess", makeCsr(), "192.0.2." + std::to_string(i + 1)}).status, 401);
    EXPECT_EQ(svc_->enroll({p.player.id, p.token, makeCsr(), "192.0.2.99"}).status, 200);
}

TEST_F(EnrollServiceTest, ALockedAddressRecoversWithTime) {
    for (int i = 0; i < 3; ++i)
        svc_->enroll({"00000000-0000-4000-8000-00000000000" + std::to_string(i), "g", makeCsr(), "203.0.113.50"});
    auto p = newPlayer();
    EXPECT_EQ(svc_->enroll({p.player.id, p.token, makeCsr(), "203.0.113.50"}).status, 429);
    now_ += 3600;
    // The token was made at the old "now" and lasts 7 days, so it is still valid.
    EXPECT_EQ(svc_->enroll({p.player.id, p.token, makeCsr(), "203.0.113.50"}).status, 200);
}

TEST_F(EnrollServiceTest, TwoTierCenterReturnsTheIntermediateAsClientChain) {
    // Offline root signs the server's intermediate (as in the ceremony).
    const fs::path root_dir = dir_ / "offline-root";
    std::string err;
    ASSERT_TRUE(tls::createDeviceRoot(root_dir, "root-passphrase", err, kw::KdfParams::minimal())) << err;
    std::string csr;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(dir_ / "device_ca2", *mk_, csr, err)) << err;
    std::string inter;
    ASSERT_TRUE(tls::signDeviceIntermediate(root_dir, "root-passphrase", csr, 1825, inter, err)) << err;
    ASSERT_TRUE(tls::DeviceCa::importIntermediate(dir_ / "device_ca2", *mk_, inter,
                                                  slurp(root_dir / "root.crt"), err)) << err;
    auto ca = tls::DeviceCa::open(dir_ / "device_ca2", *mk_, err, /*allow_create=*/false);
    ASSERT_TRUE(ca) << err;
    ca_ = std::shared_ptr<const tls::DeviceCa>(std::move(ca));

    auto p = newPlayer();
    auto out = svc_->enroll(request(p));
    ASSERT_EQ(out.status, 200) << out.body.dump();
    ASSERT_TRUE(out.body.contains("client_chain_pem"));
    EXPECT_EQ(out.body["client_chain_pem"], ca_->chainPem());
    EXPECT_NE(out.body["client_chain_pem"], ca_->rootPem()) << "the chain is NOT the root";
}
