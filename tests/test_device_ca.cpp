// Unit tests for utils/DeviceCa — the CA that issues player client certs.
//
// Coverage:
//   * generation 1 is created on first open, reopen is idempotent, the CA
//     key is stored sealed (never as clear PEM) with mode 0600;
//   * open() refuses (and changes nothing) on a wrong master key or a
//     tampered key blob;
//   * signClientCsr(): chain, forced CN, CA:FALSE, KU/EKU, validity window,
//     CSR subject/extensions ignored, unique positive serials;
//   * every CSR rejection (garbage, oversize, non-EC, wrong curve, bad
//     signature, wrong signature hash) and bad player ids / validity;
//   * generateCrl(): issuer, serials, cRLNumber, nextUpdate, signature;
//   * resealAll(): new key opens the CA, old one no longer does, no change
//     when the old key is wrong.

#include "utils/DeviceCa.h"

#include "auth/MasterKey.h"

#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace fs  = std::filesystem;
namespace tls = liveqx::tls;
using liveqx::auth::MasterKey;

namespace {

constexpr const char* kPlayerId = "3f2b8c1e-9a47-4d6b-8e05-1c7a2d9f4b60";  // v4

fs::path makeTempDir(const std::string& tag) {
    static std::atomic<int> seq{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = fs::temp_directory_path() /
                 ("lqx-devca-" + tag + "-" + std::to_string(::getpid()) + "-" +
                  std::to_string(stamp) + "-" + std::to_string(seq.fetch_add(1)));
    fs::create_directories(p);
    return p;
}

std::unique_ptr<MasterKey> makeKey(const fs::path& dir, const std::string& name) {
    auto k = std::make_unique<MasterKey>((dir / name).string());
    EXPECT_TRUE(k->load());
    return k;
}

mode_t modeOf(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return st.st_mode & 0777;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

struct Pkey {
    EVP_PKEY* p{nullptr};
    explicit Pkey(EVP_PKEY* k) : p(k) {}
    ~Pkey() { EVP_PKEY_free(p); }
    Pkey(const Pkey&) = delete;
    Pkey& operator=(const Pkey&) = delete;
};

std::string reqToPem(X509_REQ* req) {
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string s(d, static_cast<size_t>(n));
    BIO_free_all(b);
    return s;
}

// Builds a PEM CSR. `curve` is "P-256"/"P-384" or "RSA" for a 2048-bit key.
std::string makeCsr(const char* curve = "P-256", const char* cn = "evil-cn",
                    const EVP_MD* md = EVP_sha256(), bool add_san = false) {
    Pkey key(std::string(curve) == "RSA" ? EVP_RSA_gen(2048) : EVP_EC_gen(curve));
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_version(req, 0);
    X509_NAME* nm = X509_REQ_get_subject_name(req);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_REQ_set_pubkey(req, key.p);
    if (add_san) {
        STACK_OF(X509_EXTENSION)* exts = sk_X509_EXTENSION_new_null();
        X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, nullptr,
                                                NID_subject_alt_name, "DNS:evil.example");
        sk_X509_EXTENSION_push(exts, e);
        X509_REQ_add_extensions(req, exts);
        sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    }
    X509_REQ_sign(req, key.p, md);
    std::string pem = reqToPem(req);
    X509_REQ_free(req);
    return pem;
}

// CSR signed by key A but carrying key B's public key: signature must fail.
std::string makeCsrWithBrokenSignature() {
    Pkey a(EVP_EC_gen("P-256"));
    Pkey b(EVP_EC_gen("P-256"));
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_pubkey(req, a.p);
    X509_REQ_sign(req, a.p, EVP_sha256());
    X509_REQ_set_pubkey(req, b.p);  // invalidates the signature
    std::string pem = reqToPem(req);
    X509_REQ_free(req);
    return pem;
}

X509* parseCert(const std::string& pem) {
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* c = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    return c;
}

std::string serialOf(X509* c) {
    BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(c), nullptr);
    char* hex = BN_bn2hex(bn);
    std::string s(hex);
    OPENSSL_free(hex);
    BN_free(bn);
    return s;
}

std::int64_t asn1Unix(const ASN1_TIME* t) {
    struct tm tmv{};
    ASN1_TIME_to_tm(t, &tmv);
    return static_cast<std::int64_t>(timegm(&tmv));
}

class DeviceCaTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = makeTempDir("t");
        key_  = makeKey(root_, "master.key");
        dir_  = root_ / "devca";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    std::unique_ptr<tls::DeviceCa> open() {
        std::string err;
        auto ca = tls::DeviceCa::open(dir_, *key_, err);
        EXPECT_TRUE(ca) << err;
        return ca;
    }
    fs::path root_, dir_;
    std::unique_ptr<MasterKey> key_;
};

}  // namespace

// ─── open ───────────────────────────────────────────────────────────────────
TEST_F(DeviceCaTest, OpenCreatesGenerationOneAndIsIdempotent) {
    auto ca1 = open();
    ASSERT_TRUE(ca1);
    EXPECT_EQ(ca1->generation(), 1);
    EXPECT_NE(ca1->certPem().find("BEGIN CERTIFICATE"), std::string::npos);
    std::string pem = ca1->certPem();
    ca1.reset();

    auto ca2 = open();
    ASSERT_TRUE(ca2);
    EXPECT_EQ(ca2->certPem(), pem) << "reopen must not regenerate the CA";
}

TEST_F(DeviceCaTest, CaKeyIsStoredSealedWithMode0600) {
    open();
    fs::path blob = dir_ / "gen-1" / "ca.key.enc";
    ASSERT_TRUE(fs::exists(blob));
    EXPECT_EQ(modeOf(blob), 0600u);
    EXPECT_EQ(slurp(blob).find("PRIVATE KEY"), std::string::npos);
    EXPECT_FALSE(fs::exists(dir_ / "gen-1" / "ca.key"));
}

TEST_F(DeviceCaTest, OpenRefusesWrongMasterKeyAndChangesNothing) {
    open();
    std::string before = slurp(dir_ / "gen-1" / "ca.key.enc");
    auto other = makeKey(root_, "other.key");
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::open(dir_, *other, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(slurp(dir_ / "gen-1" / "ca.key.enc"), before);
}

TEST_F(DeviceCaTest, OpenRefusesTamperedBlobAndDoesNotRegenerate) {
    auto ca = open();
    std::string pem = ca->certPem();
    ca.reset();
    std::string blob = slurp(dir_ / "gen-1" / "ca.key.enc");
    blob[blob.size() / 2] ^= 0x01;
    spit(dir_ / "gen-1" / "ca.key.enc", blob);

    std::string err;
    EXPECT_FALSE(tls::DeviceCa::open(dir_, *key_, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(slurp(dir_ / "gen-1" / "ca.crt"), pem);
}

TEST_F(DeviceCaTest, OpenRefusesKeyThatDoesNotMatchCertificate) {
    auto ca = open();
    ca.reset();
    // A second CA's sealed key under gen-1's certificate.
    fs::path dir2 = root_ / "devca2";
    std::string err;
    ASSERT_TRUE(tls::DeviceCa::open(dir2, *key_, err)) << err;
    fs::copy_file(dir2 / "gen-1" / "ca.key.enc", dir_ / "gen-1" / "ca.key.enc",
                  fs::copy_options::overwrite_existing);
    EXPECT_FALSE(tls::DeviceCa::open(dir_, *key_, err));
}

TEST_F(DeviceCaTest, HighestGenerationIsActive) {
    auto ca = open();
    ca.reset();
    fs::path dir2 = root_ / "devca2";
    std::string err;
    auto second = tls::DeviceCa::open(dir2, *key_, err);
    ASSERT_TRUE(second);
    std::string second_pem = second->certPem();
    second.reset();
    fs::copy(dir2 / "gen-1", dir_ / "gen-2", fs::copy_options::recursive);

    auto reopened = open();
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->generation(), 2);
    EXPECT_EQ(reopened->certPem(), second_pem);
}

// ─── signClientCsr ──────────────────────────────────────────────────────────
TEST_F(DeviceCaTest, SignedCertChainsToCaWithForcedCnAndClientAuthOnly) {
    auto ca = open();
    const std::int64_t now = 1'800'000'000;
    tls::IssuedClientCert out;
    std::string err;
    ASSERT_TRUE(ca->signClientCsr(makeCsr("P-256", "evil-cn", EVP_sha256(), true),
                                  kPlayerId, 365, out, err, now)) << err;

    X509* cert = parseCert(out.cert_pem);
    X509* cacert = parseCert(ca->certPem());
    ASSERT_TRUE(cert && cacert);

    EXPECT_EQ(X509_verify(cert, X509_get0_pubkey(cacert)), 1);

    char cn[128] = {0};
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn);
    EXPECT_STREQ(cn, kPlayerId);

    EXPECT_EQ(X509_check_ca(cert), 0);
    EXPECT_TRUE(X509_get_key_usage(cert) & KU_DIGITAL_SIGNATURE);
    EXPECT_FALSE(X509_get_key_usage(cert) & (KU_KEY_CERT_SIGN | KU_CRL_SIGN));
    EXPECT_EQ(X509_get_extended_key_usage(cert), static_cast<uint32_t>(XKU_SSL_CLIENT));
    EXPECT_EQ(X509_get_ext_by_NID(cert, NID_subject_alt_name, -1), -1)
        << "extensions requested in the CSR must be ignored";

    EXPECT_EQ(out.generation, 1);
    EXPECT_EQ(out.serial_hex, serialOf(cert));
    EXPECT_EQ(out.not_before_unix, now - 86400);
    EXPECT_EQ(out.not_after_unix, now + 365LL * 86400);
    EXPECT_EQ(asn1Unix(X509_get0_notBefore(cert)), out.not_before_unix);
    EXPECT_EQ(asn1Unix(X509_get0_notAfter(cert)), out.not_after_unix);

    X509_free(cert);
    X509_free(cacert);
}

TEST_F(DeviceCaTest, SerialsAreUniqueAndPositive) {
    auto ca = open();
    std::string err;
    std::set<std::string> seen;
    for (int i = 0; i < 20; ++i) {
        tls::IssuedClientCert out;
        ASSERT_TRUE(ca->signClientCsr(makeCsr(), kPlayerId, 30, out, err)) << err;
        EXPECT_TRUE(seen.insert(out.serial_hex).second);
        X509* c = parseCert(out.cert_pem);
        EXPECT_FALSE(ASN1_STRING_get0_data(X509_get0_serialNumber(c))[0] & 0x80)
            << "serial must not be negative";
        X509_free(c);
    }
}

TEST_F(DeviceCaTest, RejectsBadCsrs) {
    auto ca = open();
    tls::IssuedClientCert out;
    std::string err;

    EXPECT_FALSE(ca->signClientCsr("not a csr", kPlayerId, 365, out, err));
    EXPECT_EQ(err, "bad_csr_pem");

    EXPECT_FALSE(ca->signClientCsr(std::string(5000, 'A'), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "csr_too_large");

    EXPECT_FALSE(ca->signClientCsr(makeCsr("RSA"), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "csr_bad_key_type");

    EXPECT_FALSE(ca->signClientCsr(makeCsr("P-384", "x", EVP_sha384()), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "csr_bad_key_type");

    EXPECT_FALSE(ca->signClientCsr(makeCsrWithBrokenSignature(), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "csr_bad_signature");

    EXPECT_FALSE(ca->signClientCsr(makeCsr("P-256", "x", EVP_sha384()), kPlayerId, 365, out, err));
    EXPECT_EQ(err, "csr_bad_signature_algorithm");
}

TEST_F(DeviceCaTest, RejectsBadPlayerIdsAndValidity) {
    auto ca = open();
    tls::IssuedClientCert out;
    std::string err;
    const std::string csr = makeCsr();

    for (const char* bad : {"", "player-1", "3F2B8C1E-9A47-4D6B-8E05-1C7A2D9F4B60",
                            "3f2b8c1e-9a47-1d6b-8e05-1c7a2d9f4b60",   // version 1
                            "3f2b8c1e-9a47-4d6b-7e05-1c7a2d9f4b60",   // bad variant
                            "3f2b8c1e9a474d6b8e051c7a2d9f4b60"}) {
        EXPECT_FALSE(ca->signClientCsr(csr, bad, 365, out, err)) << bad;
        EXPECT_EQ(err, "bad_player_id") << bad;
    }
    for (int v : {0, -5}) {
        EXPECT_FALSE(ca->signClientCsr(csr, kPlayerId, v, out, err));
        EXPECT_EQ(err, "bad_validity");
    }
}

// ─── generateCrl ────────────────────────────────────────────────────────────
TEST_F(DeviceCaTest, CrlIsSignedByCaAndListsRevokedSerials) {
    auto ca = open();
    const std::int64_t now = 1'800'000'000;
    std::string pem, err;
    ASSERT_TRUE(ca->generateCrl({{"0A1B2C", now - 100}, {"FF00FF", now - 50}},
                                7, 14, pem, err, now)) << err;

    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509_CRL* crl = PEM_read_bio_X509_CRL(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    ASSERT_TRUE(crl);

    X509* cacert = parseCert(ca->certPem());
    EXPECT_EQ(X509_CRL_verify(crl, X509_get0_pubkey(cacert)), 1);
    EXPECT_EQ(X509_NAME_cmp(X509_CRL_get_issuer(crl), X509_get_subject_name(cacert)), 0);

    EXPECT_EQ(asn1Unix(X509_CRL_get0_nextUpdate(crl)), now + 14LL * 86400);

    ASN1_INTEGER* num = static_cast<ASN1_INTEGER*>(
        X509_CRL_get_ext_d2i(crl, NID_crl_number, nullptr, nullptr));
    ASSERT_TRUE(num);
    EXPECT_EQ(ASN1_INTEGER_get(num), 7);
    ASN1_INTEGER_free(num);

    STACK_OF(X509_REVOKED)* rev = X509_CRL_get_REVOKED(crl);
    ASSERT_EQ(sk_X509_REVOKED_num(rev), 2);
    std::set<std::string> serials;
    for (int i = 0; i < 2; ++i) {
        BIGNUM* bn = ASN1_INTEGER_to_BN(
            X509_REVOKED_get0_serialNumber(sk_X509_REVOKED_value(rev, i)), nullptr);
        char* hex = BN_bn2hex(bn);
        serials.insert(hex);
        OPENSSL_free(hex);
        BN_free(bn);
    }
    EXPECT_EQ(serials, (std::set<std::string>{"0A1B2C", "FF00FF"}));

    X509_free(cacert);
    X509_CRL_free(crl);
}

TEST_F(DeviceCaTest, CrlWithNoRevocationsAndBadSerialIsRejected) {
    auto ca = open();
    std::string pem, err;
    EXPECT_TRUE(ca->generateCrl({}, 1, 7, pem, err)) << err;
    EXPECT_NE(pem.find("BEGIN X509 CRL"), std::string::npos);
    EXPECT_FALSE(ca->generateCrl({{"not-hex", 1}}, 2, 7, pem, err));
    EXPECT_EQ(err, "bad_serial");
    EXPECT_FALSE(ca->generateCrl({}, 0, 7, pem, err));
    EXPECT_EQ(err, "bad_crl_number");
}

// ─── resealAll ──────────────────────────────────────────────────────────────
TEST_F(DeviceCaTest, ResealAllMovesCaToTheNewKey) {
    auto ca = open();
    std::string pem = ca->certPem();
    ca.reset();
    auto fresh = makeKey(root_, "fresh.key");

    std::string err;
    ASSERT_TRUE(tls::DeviceCa::resealAll(dir_, *key_, *fresh, err)) << err;

    EXPECT_FALSE(tls::DeviceCa::open(dir_, *key_, err)) << "old key must stop working";
    auto reopened = tls::DeviceCa::open(dir_, *fresh, err);
    ASSERT_TRUE(reopened) << err;
    EXPECT_EQ(reopened->certPem(), pem);
    EXPECT_EQ(modeOf(dir_ / "gen-1" / "ca.key.enc"), 0600u);
}

TEST_F(DeviceCaTest, ResealAllChangesNothingWhenOldKeyIsWrong) {
    open();
    std::string before = slurp(dir_ / "gen-1" / "ca.key.enc");
    auto wrong = makeKey(root_, "wrong.key");
    auto fresh = makeKey(root_, "fresh.key");
    std::string err;
    EXPECT_FALSE(tls::DeviceCa::resealAll(dir_, *wrong, *fresh, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(slurp(dir_ / "gen-1" / "ca.key.enc"), before);
}

TEST_F(DeviceCaTest, ResealAllOnMissingDirectoryIsNotAnError) {
    auto fresh = makeKey(root_, "fresh.key");
    std::string err;
    EXPECT_TRUE(tls::DeviceCa::resealAll(root_ / "nope", *key_, *fresh, err)) << err;
    EXPECT_FALSE(fs::exists(root_ / "nope"));
}
