// DeviceCa::ensureBrokerIdentity — the server's own client certificate for the
// MQTT broker (CN liveqx-server).
//
// The server is a client of the broker with a certificate of its own. It is
// issued by the same device CA (the broker trusts that CA for client
// certificates) but is NOT a player: its CN is not a player id, and the key
// is sealed with the master key like the CA key itself.
//
// What these tests protect:
//   * the certificate carries the fixed CN, clientAuth only, is not a CA, and
//     verifies against the device root through the intermediate;
//   * its private key never sits on disk in the clear and is re-sealed by
//     master key rotation together with the CA key;
//   * it is reused while good and re-issued when it is not: tampered key, key
//     that does not match, issuer gone (new CA generation), close to expiry;
//   * the player signing path still refuses this CN.

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"
#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
namespace kw  = liveqx::auth::keywrap;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

struct X509Free { void operator()(X509* x) const { X509_free(x); } };
using CertPtr = std::unique_ptr<X509, X509Free>;

CertPtr firstCert(const std::string& pem) {
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    CertPtr c(PEM_read_bio_X509(b, nullptr, nullptr, nullptr));
    BIO_free_all(b);
    return c;
}

std::string cnOf(X509* x) {
    char buf[256]{};
    X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, buf, sizeof buf);
    return buf;
}

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

class BrokerIdentityTest : public ::testing::Test {
protected:
    fs::path dir_;
    fs::path root_dir_;
    std::unique_ptr<liveqx::auth::MasterKey> mk_;
    std::unique_ptr<tls::DeviceCa> ca_;
    std::int64_t now_{0};

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-brokerid-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        now_ = std::time(nullptr);
        mk_ = std::make_unique<liveqx::auth::MasterKey>((dir_ / "master.key").string());
        ASSERT_TRUE(mk_->load());
        root_dir_ = dir_ / "root";
        std::string err;
        ASSERT_TRUE(tls::createDeviceRoot(root_dir_, "pass", err, kw::KdfParams::minimal())) << err;
        addGeneration();
    }
    void TearDown() override {
        ca_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // The next intermediate under the same offline root, then the CA reopened.
    void addGeneration() {
        std::string err, csr, inter;
        ca_.reset();
        ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(dir_ / "ca", *mk_, csr, err)) << err;
        ASSERT_TRUE(tls::signDeviceIntermediate(root_dir_, "pass", csr, 1825, inter, err)) << err;
        ASSERT_TRUE(tls::DeviceCa::importIntermediate(dir_ / "ca", *mk_, inter,
                                                      slurp(root_dir_ / "root.crt"), err)) << err;
        ca_ = tls::DeviceCa::open(dir_ / "ca", *mk_, err, false);
        ASSERT_TRUE(ca_) << err;
    }

    fs::path idDir() const { return dir_ / "ca" / "broker_client"; }
    bool ensure(tls::BrokerIdentity& out, std::int64_t at = 0) {
        std::string err;
        const bool ok = tls::DeviceCa::ensureBrokerIdentity(dir_ / "ca", *mk_, *ca_, out, err, at ? at : now_);
        EXPECT_TRUE(ok) << err;
        return ok;
    }
    // Does `leaf_pem` chain to the device root through the intermediate?
    bool chainsToRoot(const std::string& leaf_pem) {
        X509_STORE* store = X509_STORE_new();
        CertPtr root = firstCert(ca_->rootPem());
        X509_STORE_add_cert(store, root.get());
        CertPtr leaf = firstCert(leaf_pem);
        STACK_OF(X509)* chain = sk_X509_new_null();
        CertPtr inter = firstCert(ca_->chainPem());
        sk_X509_push(chain, inter.get());
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        X509_STORE_CTX_init(ctx, store, leaf.get(), chain);
        X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT);
        const bool ok = X509_verify_cert(ctx) == 1;
        X509_STORE_CTX_free(ctx);
        sk_X509_free(chain);
        X509_STORE_free(store);
        return ok;
    }
};

}  // namespace

TEST_F(BrokerIdentityTest, TheCertificateIsTheServersOwnClientCertificate) {
    tls::BrokerIdentity id;
    ASSERT_TRUE(ensure(id));
    CertPtr leaf = firstCert(id.cert_chain_pem);
    ASSERT_TRUE(leaf);
    EXPECT_EQ(cnOf(leaf.get()), "liveqx-server");
    EXPECT_NE(cnOf(leaf.get()).size(), 36u) << "not shaped like a player id";
    EXPECT_EQ(X509_check_ca(leaf.get()), 0) << "not a CA";
    EXPECT_TRUE(X509_get_extended_key_usage(leaf.get()) & XKU_SSL_CLIENT);
    EXPECT_FALSE(X509_get_extended_key_usage(leaf.get()) & XKU_SSL_SERVER) << "clientAuth only";
    EXPECT_TRUE(chainsToRoot(id.cert_chain_pem)) << "the broker, which trusts the device root, accepts it";
    EXPECT_NE(id.key_pem.find("PRIVATE KEY"), std::string::npos);
    EXPECT_GT(id.not_after_unix, now_ + 300LL * 86400);
    EXPECT_EQ(id.generation, ca_->generation());
    EXPECT_FALSE(id.serial_hex.empty());
}

TEST_F(BrokerIdentityTest, TheChainFileHoldsTheLeafAndTheIntermediateForTheBroker) {
    tls::BrokerIdentity id;
    ASSERT_TRUE(ensure(id));
    EXPECT_NE(id.cert_chain_pem.find(ca_->chainPem()), std::string::npos)
        << "the broker's CA file holds the root; the client sends leaf + intermediate";
}

TEST_F(BrokerIdentityTest, TheKeyIsSealedOnDiskAndTheFilesArePrivate) {
    tls::BrokerIdentity id;
    ASSERT_TRUE(ensure(id));
    const std::string sealed = slurp(idDir() / "client.key.enc");
    ASSERT_FALSE(sealed.empty());
    EXPECT_EQ(sealed.find("PRIVATE KEY"), std::string::npos) << "never in the clear";
    struct stat st{};
    ASSERT_EQ(::stat((idDir() / "client.key.enc").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
    for (const auto& e : fs::recursive_directory_iterator(dir_ / "ca"))
        if (e.is_regular_file())
            EXPECT_EQ(slurp(e.path()).find("PRIVATE KEY"), std::string::npos)
                << e.path() << " holds a key in the clear";
}

TEST_F(BrokerIdentityTest, AGoodIdentityIsReusedNotReissued) {
    tls::BrokerIdentity a, b;
    ASSERT_TRUE(ensure(a));
    ASSERT_TRUE(ensure(b));
    EXPECT_EQ(a.serial_hex, b.serial_hex);
    EXPECT_EQ(a.key_pem, b.key_pem);
}

TEST_F(BrokerIdentityTest, ATamperedKeyFileMeansANewIdentityNotAnOutage) {
    tls::BrokerIdentity a, b;
    ASSERT_TRUE(ensure(a));
    write(idDir() / "client.key.enc", "garbage");
    ASSERT_TRUE(ensure(b));
    EXPECT_NE(a.serial_hex, b.serial_hex);
    EXPECT_TRUE(chainsToRoot(b.cert_chain_pem));
}

TEST_F(BrokerIdentityTest, ACertificateThatDoesNotFitTheKeyIsReplaced) {
    tls::BrokerIdentity a, b, c;
    ASSERT_TRUE(ensure(a));
    const std::string a_cert = slurp(idDir() / "client.crt");
    std::error_code ec;
    fs::remove_all(idDir(), ec);
    ASSERT_TRUE(ensure(b));                          // a different identity, a different key
    write(idDir() / "client.crt", a_cert);           // put a's certificate next to b's key
    ASSERT_TRUE(ensure(c));
    EXPECT_NE(c.serial_hex, a.serial_hex) << "the mismatched certificate was not trusted";
    EXPECT_NE(c.serial_hex, b.serial_hex);
    EXPECT_TRUE(chainsToRoot(c.cert_chain_pem));
}

TEST_F(BrokerIdentityTest, ANewCaGenerationBringsANewIdentity) {
    tls::BrokerIdentity a, b;
    ASSERT_TRUE(ensure(a));
    addGeneration();                                 // the intermediate is replaced
    ASSERT_EQ(ca_->generation(), 2);
    ASSERT_TRUE(ensure(b));
    EXPECT_NE(a.serial_hex, b.serial_hex);
    EXPECT_EQ(b.generation, 2);
    EXPECT_TRUE(chainsToRoot(b.cert_chain_pem)) << "and it chains through the NEW intermediate";
}

TEST_F(BrokerIdentityTest, AnIdentityCloseToExpiryIsRenewedBeforeItLapses) {
    tls::BrokerIdentity a, soon, b;
    ASSERT_TRUE(ensure(a));
    EXPECT_TRUE(ensure(soon, now_ + 300LL * 86400));   // 65 days left: still fine
    EXPECT_EQ(soon.serial_hex, a.serial_hex);
    ASSERT_TRUE(ensure(b, now_ + 340LL * 86400));      // 25 days left: renew
    EXPECT_NE(b.serial_hex, a.serial_hex);
}

TEST_F(BrokerIdentityTest, MasterKeyRotationReSealsTheIdentityAlongWithTheCa) {
    tls::BrokerIdentity before;
    ASSERT_TRUE(ensure(before));

    liveqx::auth::MasterKey fresh((dir_ / "master2.key").string());
    ASSERT_TRUE(fresh.load());
    std::string err;
    ASSERT_TRUE(tls::DeviceCa::resealAll(dir_ / "ca", *mk_, fresh, err)) << err;

    ca_.reset();
    ca_ = tls::DeviceCa::open(dir_ / "ca", fresh, err, false);
    ASSERT_TRUE(ca_) << err;
    tls::BrokerIdentity after;
    ASSERT_TRUE(tls::DeviceCa::ensureBrokerIdentity(dir_ / "ca", fresh, *ca_, after, err, now_)) << err;
    EXPECT_EQ(after.serial_hex, before.serial_hex) << "re-sealed, not re-issued";
    EXPECT_EQ(after.key_pem, before.key_pem);
}

TEST_F(BrokerIdentityTest, ARotationThatCannotReadTheIdentityRefusesAndChangesNothing) {
    tls::BrokerIdentity id;
    ASSERT_TRUE(ensure(id));
    const std::string ca_key_before = slurp(dir_ / "ca" / "gen-1" / "ca.key.enc");
    ASSERT_FALSE(ca_key_before.empty());
    write(idDir() / "client.key.enc", "garbage");

    liveqx::auth::MasterKey fresh((dir_ / "master3.key").string());
    ASSERT_TRUE(fresh.load());
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::resealAll(dir_ / "ca", *mk_, fresh, err));
    EXPECT_NE(err.find("broker"), std::string::npos) << err;
    EXPECT_EQ(slurp(dir_ / "ca" / "gen-1" / "ca.key.enc"), ca_key_before)
        << "nothing is written unless everything can be read";
}

TEST_F(BrokerIdentityTest, ThePlayerSigningPathStillRefusesThisCn) {
    std::string err;
    tls::IssuedClientCert out;
    EXPECT_FALSE(ca_->signClientCsr(makeCsr(), "liveqx-server", 365, out, err));
    EXPECT_EQ(err, "bad_player_id") << "only a player id can be a player certificate's CN";
}

// ── opening an older generation (needed to sign its revocation list) ────────
TEST_F(BrokerIdentityTest, AnOlderGenerationCanBeOpenedAndSignsItsOwnRevocationList) {
    const std::string gen1_cert = ca_->certPem();
    ASSERT_EQ(ca_->generation(), 1);
    addGeneration();                                       // the intermediate is replaced: generation 2 is active
    ASSERT_EQ(ca_->generation(), 2);
    EXPECT_EQ(tls::DeviceCa::generations(dir_ / "ca"), (std::vector<int>{1, 2}));

    std::string err;
    auto old = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 1, err);
    ASSERT_TRUE(old) << err;
    EXPECT_EQ(old->generation(), 1);
    EXPECT_EQ(old->certPem(), gen1_cert);
    EXPECT_NE(old->certPem(), ca_->certPem());

    std::string crl_pem;
    ASSERT_TRUE(old->generateCrl({{"ABCD01", now_}}, 1, 30, crl_pem, err, now_)) << err;
    // The list verifies against generation 1's intermediate and not against generation 2's.
    auto verifies = [&](const std::string& issuer_pem) {
        BIO* b = BIO_new_mem_buf(crl_pem.data(), static_cast<int>(crl_pem.size()));
        X509_CRL* crl = PEM_read_bio_X509_CRL(b, nullptr, nullptr, nullptr);
        BIO_free_all(b);
        CertPtr issuer = firstCert(issuer_pem);
        const bool ok = crl && X509_CRL_verify(crl, X509_get0_pubkey(issuer.get())) == 1;
        ERR_clear_error();
        X509_CRL_free(crl);
        return ok;
    };
    EXPECT_TRUE(verifies(gen1_cert));
    EXPECT_FALSE(verifies(ca_->certPem()));
}

TEST_F(BrokerIdentityTest, ANonexistentGenerationOrTheWrongMasterKeyIsRefusedWithAReason) {
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 9, err));
    EXPECT_EQ(err, "no_such_generation");

    liveqx::auth::MasterKey other((dir_ / "other.key").string());
    ASSERT_TRUE(other.load());
    EXPECT_FALSE(tls::DeviceCa::openGeneration(dir_ / "ca", other, 1, err));
    EXPECT_FALSE(err.empty()) << "a key that cannot be unsealed is said so";
    EXPECT_TRUE(tls::DeviceCa::generations(dir_ / "nowhere").empty());
}

TEST_F(BrokerIdentityTest, OpeningTheNewestStillWorksTheSameWayAsBefore) {
    addGeneration();
    std::string err;
    auto newest = tls::DeviceCa::open(dir_ / "ca", *mk_, err, false);
    ASSERT_TRUE(newest) << err;
    auto same = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 2, err);
    ASSERT_TRUE(same) << err;
    EXPECT_EQ(newest->certPem(), same->certPem());
    EXPECT_EQ(newest->generation(), same->generation());
}
