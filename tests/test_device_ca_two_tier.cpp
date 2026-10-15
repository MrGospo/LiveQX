// Two-tier device CA: an offline root signs an intermediate, the intermediate
// (which lives on the server, its key sealed with the master key) issues the
// player certificates.
//
// Coverage:
//   * root creation and its sealed key; signing an intermediate request
//     (wrong passphrase, hostile CSRs);
//   * the server side: request -> import, what import refuses;
//   * open() never creates a CA when creation is not allowed;
//   * issued certificates chain to the root through the intermediate, and
//     need that intermediate (client_chain_pem); CRLs are signed by it;
//   * a second generation under the same root; master-key re-sealing also
//     covers a request that has not been imported yet.

#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"

#include <gtest/gtest.h>

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
#include <memory>
#include <sstream>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
namespace kw  = liveqx::auth::keywrap;
using liveqx::auth::MasterKey;

namespace {

constexpr const char* kPlayerId = "3f2b8c1e-9a47-4d6b-8e05-1c7a2d9f4b60";
constexpr const char* kRootPass = "root key passphrase 123";

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

X509* parseCert(const std::string& pem) {
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* c = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    return c;
}

struct CertPtr {
    X509* c{nullptr};
    explicit CertPtr(const std::string& pem) : c(parseCert(pem)) {}
    ~CertPtr() { X509_free(c); }
    CertPtr(const CertPtr&) = delete;
    CertPtr& operator=(const CertPtr&) = delete;
};

// A player-style CSR (P-256, SHA-256).
std::string makePlayerCsr() {
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

// A CSR with a chosen subject CN and key type, for the intermediate request.
std::string makeCsr(const char* cn, const char* curve, const EVP_MD* md = EVP_sha256(),
                    bool tamper = false) {
    EVP_PKEY* key = std::string(curve) == "RSA" ? EVP_RSA_gen(2048) : EVP_EC_gen(curve);
    EVP_PKEY* other = EVP_EC_gen("P-256");
    X509_REQ* req = X509_REQ_new();
    X509_NAME_add_entry_by_txt(X509_REQ_get_subject_name(req), "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_REQ_set_pubkey(req, key);
    X509_REQ_sign(req, key, md);
    if (tamper) X509_REQ_set_pubkey(req, other);
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string pem(d, static_cast<size_t>(n));
    BIO_free_all(b);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    EVP_PKEY_free(other);
    return pem;
}

// Verifies `leaf` against `root` using `chain` as untrusted intermediates.
bool chainVerifies(const std::string& leaf_pem, const std::string& chain_pem,
                   const std::string& root_pem) {
    CertPtr leaf(leaf_pem), root(root_pem);
    if (!leaf.c || !root.c) return false;
    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.c);
    STACK_OF(X509)* untrusted = sk_X509_new_null();
    X509* chain_cert = nullptr;
    if (!chain_pem.empty()) {
        chain_cert = parseCert(chain_pem);
        if (chain_cert) sk_X509_push(untrusted, chain_cert);
    }
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, leaf.c, untrusted);
    X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT);
    const bool ok = X509_verify_cert(ctx) == 1;
    X509_STORE_CTX_free(ctx);
    sk_X509_free(untrusted);
    X509_free(chain_cert);
    X509_STORE_free(store);
    return ok;
}

class TwoTierTest : public ::testing::Test {
protected:
    fs::path root_;       // temp area
    fs::path rootDir_;    // offline root (cert + sealed key)
    fs::path caDir_;      // device_ca/ on the "server"
    std::unique_ptr<MasterKey> key_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        root_ = fs::temp_directory_path() /
                ("lqx-2tier-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(root_);
        rootDir_ = root_ / "offline-root";
        caDir_   = root_ / "device_ca";
        key_ = std::make_unique<MasterKey>((root_ / "master.key").string());
        ASSERT_TRUE(key_->load());
        std::string err;
        ASSERT_TRUE(tls::createDeviceRoot(rootDir_, kRootPass, err, kw::KdfParams::minimal())) << err;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // The full ceremony: request on the server, sign offline, import.
    std::string runCeremony(int intermediate_days = 1825) {
        std::string csr, err;
        EXPECT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
        std::string cert;
        EXPECT_TRUE(tls::signDeviceIntermediate(rootDir_, kRootPass, csr, intermediate_days,
                                                cert, err)) << err;
        EXPECT_TRUE(tls::DeviceCa::importIntermediate(caDir_, *key_, cert,
                                                      slurp(rootDir_ / "root.crt"), err)) << err;
        return cert;
    }

    std::unique_ptr<tls::DeviceCa> open(bool allow_create = false) {
        std::string err;
        auto ca = tls::DeviceCa::open(caDir_, *key_, err, allow_create);
        EXPECT_TRUE(ca) << err;
        return ca;
    }
};

}  // namespace

// ── the offline root ────────────────────────────────────────────────────────
TEST_F(TwoTierTest, RootIsASelfSignedCaWithRoomForOneIntermediate) {
    CertPtr root(slurp(rootDir_ / "root.crt"));
    ASSERT_TRUE(root.c);
    EXPECT_EQ(X509_verify(root.c, X509_get0_pubkey(root.c)), 1) << "self-signed";
    EXPECT_NE(X509_check_ca(root.c), 0);
    EXPECT_EQ(X509_get_pathlen(root.c), 1);
}

TEST_F(TwoTierTest, RootKeyIsSealedAndNotReadableWithoutThePassphrase) {
    const auto sealed = rootDir_ / "root.key.sealed";
    ASSERT_TRUE(fs::exists(sealed));
    struct stat st{};
    ASSERT_EQ(::stat(sealed.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
    EXPECT_EQ(slurp(sealed).find("PRIVATE KEY"), std::string::npos);
    EXPECT_TRUE(kw::isWrapped(slurp(sealed)));
}

TEST_F(TwoTierTest, CreatingARootNeverOverwritesAnExistingOne) {
    const auto before = slurp(rootDir_ / "root.crt");
    std::string err;
    EXPECT_FALSE(tls::createDeviceRoot(rootDir_, "another passphrase 456", err,
                                       kw::KdfParams::minimal()));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(slurp(rootDir_ / "root.crt"), before);
}

TEST_F(TwoTierTest, SigningWithTheWrongPassphraseIsRefused) {
    std::string cert, err;
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, "not the passphrase", makeCsr("LiveQX Device CA 1", "P-256"),
                                             1825, cert, err));
    EXPECT_EQ(err, "wrong_passphrase");
    EXPECT_TRUE(cert.empty());
}

TEST_F(TwoTierTest, SigningRefusesHostileRequests) {
    std::string cert, err;
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, kRootPass, "garbage", 1825, cert, err));
    EXPECT_EQ(err, "bad_csr_pem");
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, kRootPass, makeCsr("LiveQX Device CA 1", "RSA"),
                                             1825, cert, err));
    EXPECT_EQ(err, "csr_bad_key_type");
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, kRootPass,
                                             makeCsr("LiveQX Device CA 1", "P-256", EVP_sha256(), true),
                                             1825, cert, err));
    EXPECT_EQ(err, "csr_bad_signature");
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, kRootPass, makeCsr("evil.example.com", "P-256"),
                                             1825, cert, err));
    EXPECT_EQ(err, "csr_bad_subject") << "only 'LiveQX Device CA <n>' may be signed as a CA";
    EXPECT_FALSE(tls::signDeviceIntermediate(rootDir_, kRootPass, makeCsr("LiveQX Device CA 1", "P-256"),
                                             0, cert, err));
    EXPECT_EQ(err, "bad_validity");
}

TEST_F(TwoTierTest, SignedIntermediateIsACaThatCannotIssueFurtherCas) {
    const auto cert = runCeremony();
    CertPtr inter(cert), root(slurp(rootDir_ / "root.crt"));
    ASSERT_TRUE(inter.c && root.c);
    EXPECT_EQ(X509_verify(inter.c, X509_get0_pubkey(root.c)), 1);
    EXPECT_NE(X509_check_ca(inter.c), 0);
    EXPECT_EQ(X509_get_pathlen(inter.c), 0);
    EXPECT_TRUE(X509_get_key_usage(inter.c) & KU_KEY_CERT_SIGN);
    EXPECT_TRUE(X509_get_key_usage(inter.c) & KU_CRL_SIGN);
}

// ── open() and the request / import steps ───────────────────────────────────
TEST_F(TwoTierTest, OpenWithoutCreationDoesNotInventACa) {
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::open(caDir_, *key_, err, /*allow_create=*/false));
    EXPECT_FALSE(err.empty());
    // Nothing was created: a one-tier CA appearing here would silently
    // defeat the point of keeping the root offline.
    EXPECT_TRUE(!fs::exists(caDir_) || fs::is_empty(caDir_));
}

TEST_F(TwoTierTest, PendingRequestIsNotAGeneration) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
    EXPECT_NE(csr.find("BEGIN CERTIFICATE REQUEST"), std::string::npos);
    EXPECT_TRUE(fs::exists(caDir_ / "gen-1.pending" / "ca.key.enc"));
    EXPECT_EQ(slurp(caDir_ / "gen-1.pending" / "ca.key.enc").find("PRIVATE KEY"), std::string::npos);
    EXPECT_FALSE(tls::DeviceCa::open(caDir_, *key_, err, false)) << "not usable before import";
}

TEST_F(TwoTierTest, AskingAgainReturnsTheSamePendingRequest) {
    std::string a, b, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, a, err)) << err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, b, err)) << err;
    EXPECT_EQ(a, b) << "no orphaned keys from repeated requests";
    int pendings = 0;
    for (const auto& e : fs::directory_iterator(caDir_))
        if (e.path().filename().string().find(".pending") != std::string::npos) ++pendings;
    EXPECT_EQ(pendings, 1);
}

TEST_F(TwoTierTest, ImportTurnsThePendingRequestIntoGenerationOne) {
    const auto inter = runCeremony();
    EXPECT_FALSE(fs::exists(caDir_ / "gen-1.pending"));
    ASSERT_TRUE(fs::exists(caDir_ / "gen-1" / "ca.crt"));
    ASSERT_TRUE(fs::exists(caDir_ / "gen-1" / "root.crt"));
    EXPECT_FALSE(fs::exists(caDir_ / "gen-1" / "ca.csr"));

    auto ca = open();
    ASSERT_TRUE(ca);
    EXPECT_TRUE(ca->twoTier());
    EXPECT_EQ(ca->generation(), 1);
    EXPECT_EQ(ca->chainPem(), inter);
    EXPECT_EQ(ca->rootPem(), slurp(rootDir_ / "root.crt"));
}

TEST_F(TwoTierTest, ImportRefusesWhenThereIsNoPendingRequest) {
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, "x", "y", err));
    EXPECT_EQ(err, "no_pending_request");
}

TEST_F(TwoTierTest, ImportRefusesACertificateOfAnotherKey) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
    // A valid intermediate for a different key pair.
    std::string foreign;
    ASSERT_TRUE(tls::signDeviceIntermediate(rootDir_, kRootPass, makeCsr("LiveQX Device CA 1", "P-256"),
                                            1825, foreign, err)) << err;
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, foreign,
                                                   slurp(rootDir_ / "root.crt"), err));
    EXPECT_EQ(err, "certificate_does_not_match_request");
    EXPECT_TRUE(fs::exists(caDir_ / "gen-1.pending")) << "the request stays usable";
}

TEST_F(TwoTierTest, ImportRefusesACertificateFromAnotherRoot) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
    const auto otherRoot = root_ / "other-root";
    ASSERT_TRUE(tls::createDeviceRoot(otherRoot, kRootPass, err, kw::KdfParams::minimal())) << err;
    std::string cert;
    ASSERT_TRUE(tls::signDeviceIntermediate(otherRoot, kRootPass, csr, 1825, cert, err)) << err;
    // Signed by "other-root" but presented together with our root.
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, cert,
                                                   slurp(rootDir_ / "root.crt"), err));
    EXPECT_EQ(err, "certificate_not_signed_by_root");
}

TEST_F(TwoTierTest, ImportRefusesGarbage) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, "garbage",
                                                   slurp(rootDir_ / "root.crt"), err));
    EXPECT_EQ(err, "bad_certificate_pem");
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, "garbage", "garbage", err));
}

TEST_F(TwoTierTest, ImportRefusesAnExpiredIntermediate) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;
    std::string cert;
    // Signed as if "now" were 5 years ago, valid for 1 day: long expired.
    const std::int64_t past = static_cast<std::int64_t>(std::time(nullptr)) - 5LL * 365 * 86400;
    ASSERT_TRUE(tls::signDeviceIntermediate(rootDir_, kRootPass, csr, 1, cert, err, past)) << err;
    EXPECT_FALSE(tls::DeviceCa::importIntermediate(caDir_, *key_, cert,
                                                   slurp(rootDir_ / "root.crt"), err));
    EXPECT_EQ(err, "certificate_not_currently_valid");
}

// ── issuing through the intermediate ────────────────────────────────────────
TEST_F(TwoTierTest, PlayerCertificateChainsToTheRootOnlyThroughTheIntermediate) {
    runCeremony();
    auto ca = open();
    tls::IssuedClientCert out;
    std::string err;
    ASSERT_TRUE(ca->signClientCsr(makePlayerCsr(), kPlayerId, 365, out, err)) << err;

    EXPECT_EQ(out.chain_pem, ca->chainPem());
    EXPECT_TRUE(chainVerifies(out.cert_pem, out.chain_pem, ca->rootPem()));
    EXPECT_FALSE(chainVerifies(out.cert_pem, "", ca->rootPem()))
        << "without client_chain_pem the verifier cannot reach the root";
}

TEST_F(TwoTierTest, OneTierCaReportsNoChain) {
    std::string err;
    auto ca = tls::DeviceCa::open(caDir_, *key_, err, /*allow_create=*/true);
    ASSERT_TRUE(ca) << err;
    EXPECT_FALSE(ca->twoTier());
    EXPECT_TRUE(ca->chainPem().empty());
    EXPECT_TRUE(ca->rootPem().empty());
    tls::IssuedClientCert out;
    ASSERT_TRUE(ca->signClientCsr(makePlayerCsr(), kPlayerId, 365, out, err)) << err;
    EXPECT_TRUE(out.chain_pem.empty());
}

TEST_F(TwoTierTest, CertificateThatWouldOutliveTheIntermediateIsRefused) {
    runCeremony(/*intermediate_days=*/30);
    auto ca = open();
    tls::IssuedClientCert out;
    std::string err;
    EXPECT_FALSE(ca->signClientCsr(makePlayerCsr(), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "ca_validity_too_short");
    EXPECT_TRUE(ca->signClientCsr(makePlayerCsr(), kPlayerId, 7, out, err)) << err;
}

TEST_F(TwoTierTest, CrlIsSignedByTheIntermediate) {
    const auto inter = runCeremony();
    auto ca = open();
    std::string pem, err;
    ASSERT_TRUE(ca->generateCrl({{"0A1B2C", 1'800'000'000}}, 1, 7, pem, err, 1'800'000'100)) << err;
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509_CRL* crl = PEM_read_bio_X509_CRL(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    ASSERT_TRUE(crl);
    CertPtr issuer(inter);
    EXPECT_EQ(X509_CRL_verify(crl, X509_get0_pubkey(issuer.c)), 1);
    X509_CRL_free(crl);
}

// ── a second generation under the same root ─────────────────────────────────
TEST_F(TwoTierTest, SecondIntermediateBecomesActiveAndFirstStillValidates) {
    runCeremony();
    std::string first_leaf_pem, first_chain;
    std::string root_pem;
    {
        auto ca1 = open();
        tls::IssuedClientCert out;
        std::string err;
        ASSERT_TRUE(ca1->signClientCsr(makePlayerCsr(), kPlayerId, 365, out, err)) << err;
        first_leaf_pem = out.cert_pem;
        first_chain    = out.chain_pem;
        root_pem       = ca1->rootPem();
    }
    // Replace the intermediate: same root, no re-pairing of anyone.
    const auto second = runCeremony();
    auto ca2 = open();
    EXPECT_EQ(ca2->generation(), 2);
    EXPECT_EQ(ca2->chainPem(), second);
    EXPECT_EQ(ca2->rootPem(), root_pem) << "the root, and so what players/servers trust, is unchanged";

    tls::IssuedClientCert out2;
    std::string err;
    ASSERT_TRUE(ca2->signClientCsr(makePlayerCsr(), kPlayerId, 365, out2, err)) << err;
    EXPECT_EQ(out2.generation, 2);
    EXPECT_TRUE(chainVerifies(out2.cert_pem, out2.chain_pem, root_pem));
    // The certificate issued under generation 1 still verifies against the same root.
    EXPECT_TRUE(chainVerifies(first_leaf_pem, first_chain, root_pem));
}

// ── master-key rotation ─────────────────────────────────────────────────────
TEST_F(TwoTierTest, ResealAllAlsoCoversARequestThatIsNotImportedYet) {
    std::string csr, err;
    ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(caDir_, *key_, csr, err)) << err;

    MasterKey fresh((root_ / "fresh.key").string());
    ASSERT_TRUE(fresh.load());
    ASSERT_TRUE(tls::DeviceCa::resealAll(caDir_, *key_, fresh, err)) << err;

    // The ceremony finishes later, under the NEW master key.
    std::string cert;
    ASSERT_TRUE(tls::signDeviceIntermediate(rootDir_, kRootPass, csr, 1825, cert, err)) << err;
    ASSERT_TRUE(tls::DeviceCa::importIntermediate(caDir_, fresh, cert,
                                                  slurp(rootDir_ / "root.crt"), err)) << err;
    auto ca = tls::DeviceCa::open(caDir_, fresh, err, false);
    EXPECT_TRUE(ca) << err;
}
