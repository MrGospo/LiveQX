// Offline root of the device CA — see utils/DeviceRootCa.h.

#include "utils/DeviceRootCa.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sodium.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;
namespace kw = liveqx::auth::keywrap;

namespace liveqx::tls {
namespace {

constexpr std::size_t kMaxCsrBytes = 4096;
constexpr int kMaxIntermediateDays = 7300;
constexpr std::int64_t kSkewSec = 24 * 3600;

struct PkeyDel { void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); } };
struct X509Del { void operator()(X509* p) const noexcept { X509_free(p); } };
struct ReqDel  { void operator()(X509_REQ* p) const noexcept { X509_REQ_free(p); } };
struct BioDel  { void operator()(BIO* p) const noexcept { BIO_free_all(p); } };
struct BnDel   { void operator()(BIGNUM* p) const noexcept { BN_free(p); } };
struct AsnDel  { void operator()(ASN1_INTEGER* p) const noexcept { ASN1_INTEGER_free(p); } };
struct TimeDel { void operator()(ASN1_TIME* p) const noexcept { ASN1_TIME_free(p); } };

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;
using Cert = std::unique_ptr<X509, X509Del>;
using Req  = std::unique_ptr<X509_REQ, ReqDel>;
using Bio  = std::unique_ptr<BIO, BioDel>;
using Bn   = std::unique_ptr<BIGNUM, BnDel>;
using Asn  = std::unique_ptr<ASN1_INTEGER, AsnDel>;
using Time = std::unique_ptr<ASN1_TIME, TimeDel>;

std::int64_t wallNow() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string bioToString(BIO* b) {
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    return (n > 0 && d) ? std::string(d, static_cast<std::size_t>(n)) : std::string();
}

std::string certToPem(X509* c) {
    Bio b(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_X509(b.get(), c)) return {};
    return bioToString(b.get());
}

Cert certFromPem(const std::string& pem) {
    Bio b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return Cert(PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr));
}

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

bool writeFile(const fs::path& p, std::string_view bytes, mode_t mode) {
    const fs::path tmp = p.string() + ".tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!o) { fs::remove(tmp, ec); return false; }
    }
    if (::chmod(tmp.c_str(), mode) != 0) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

Asn randomSerial() {
    Bn bn(BN_new());
    if (!bn || !BN_rand(bn.get(), 63, BN_RAND_TOP_TWO, BN_RAND_BOTTOM_ANY)) return nullptr;
    return Asn(BN_to_ASN1_INTEGER(bn.get(), nullptr));
}

bool addExt(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!e) return false;
    const int rc = X509_add_ext(cert, e, -1);
    X509_EXTENSION_free(e);
    return rc == 1;
}

bool setTimes(X509* c, std::int64_t not_before, std::int64_t not_after) {
    Time nb(ASN1_TIME_set(nullptr, static_cast<time_t>(not_before)));
    Time na(ASN1_TIME_set(nullptr, static_cast<time_t>(not_after)));
    return nb && na && X509_set1_notBefore(c, nb.get()) == 1 &&
           X509_set1_notAfter(c, na.get()) == 1;
}

// P-256 private scalar (32 bytes) of a key.
bool scalarOf(EVP_PKEY* key, kw::Key& out) {
    BIGNUM* raw = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &raw) != 1) return false;
    Bn bn(raw);
    return BN_bn2binpad(bn.get(), out.data(), static_cast<int>(out.size())) ==
           static_cast<int>(out.size());
}

// Rebuilds the key pair from its private scalar (the public point is derived).
Pkey keyFromScalar(const kw::Key& scalar) {
    Bn priv(BN_bin2bn(scalar.data(), static_cast<int>(scalar.size()), nullptr));
    std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group(
        EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1), &EC_GROUP_free);
    std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> pub(
        group ? EC_POINT_new(group.get()) : nullptr, &EC_POINT_free);
    if (!priv || !group || !pub ||
        EC_POINT_mul(group.get(), pub.get(), priv.get(), nullptr, nullptr, nullptr) != 1)
        return nullptr;
    unsigned char oct[65];
    const std::size_t len = EC_POINT_point2oct(group.get(), pub.get(),
                                               POINT_CONVERSION_UNCOMPRESSED, oct, sizeof oct, nullptr);
    if (len != sizeof oct) return nullptr;

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) return nullptr;
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv.get());
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, oct, len);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    if (!params) return nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* pkey = nullptr;
    const bool ok = ctx && EVP_PKEY_fromdata_init(ctx) == 1 &&
                    EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1;
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    return ok ? Pkey(pkey) : nullptr;
}

}  // namespace

bool createDeviceRoot(const fs::path& out_dir, std::string_view passphrase,
                      std::string& err, const kw::KdfParams& kdf,
                      int validity_days, std::int64_t now_unix) {
    err.clear();
    const fs::path cert_path = out_dir / "root.crt";
    const fs::path key_path  = out_dir / "root.key.sealed";
    std::error_code ec;
    if (fs::exists(cert_path, ec) || fs::exists(key_path, ec)) {
        err = "root_already_exists";
        return false;
    }
    if (passphrase.empty() || validity_days <= 0) { err = "bad_arguments"; return false; }

    Pkey key(EVP_EC_gen("P-256"));
    kw::Key scalar{};
    if (!key || !scalarOf(key.get(), scalar)) { err = "internal_error"; return false; }
    const std::string sealed = kw::wrap(scalar, passphrase, kdf);
    sodium_memzero(scalar.data(), scalar.size());
    if (sealed.empty()) { err = "cannot_seal_key"; return false; }

    const std::int64_t now = now_unix ? now_unix : wallNow();
    Cert cert(X509_new());
    auto serial = randomSerial();
    if (!cert || !serial) { err = "internal_error"; return false; }
    X509_set_version(cert.get(), 2);
    X509_set_serialNumber(cert.get(), serial.get());
    const char* cn = "LiveQX Device Root CA";
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    if (!setTimes(cert.get(), now - kSkewSec, now + std::int64_t{validity_days} * 86400) ||
        !X509_set_pubkey(cert.get(), key.get()) ||
        // pathlen 1: exactly one intermediate may sit between the root and a player.
        !addExt(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:1") ||
        !addExt(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExt(cert.get(), cert.get(), NID_subject_key_identifier, "hash") ||
        !X509_sign(cert.get(), key.get(), EVP_sha256())) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }

    fs::create_directories(out_dir, ec);
    if (ec) { err = "cannot_create_directory"; return false; }
    ::chmod(out_dir.c_str(), 0700);
    if (!writeFile(key_path, sealed, 0600)) { err = "cannot_write_files"; return false; }
    if (!writeFile(cert_path, certToPem(cert.get()), 0644)) {
        fs::remove(key_path, ec);
        err = "cannot_write_files";
        return false;
    }
    return true;
}

bool signDeviceIntermediate(const fs::path& root_dir, std::string_view passphrase,
                            std::string_view csr_pem, int validity_days,
                            std::string& cert_pem, std::string& err,
                            std::int64_t now_unix) {
    err.clear();
    cert_pem.clear();

    // ── the request ──
    if (csr_pem.size() > kMaxCsrBytes) { err = "csr_too_large"; return false; }
    Bio bio(BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size())));
    Req req(PEM_read_bio_X509_REQ(bio.get(), nullptr, nullptr, nullptr));
    ERR_clear_error();
    if (!req) { err = "bad_csr_pem"; return false; }

    EVP_PKEY* pub = X509_REQ_get0_pubkey(req.get());
    bool p256 = false;
    if (pub && EVP_PKEY_id(pub) == EVP_PKEY_EC) {
        char grp[32] = {0};
        std::size_t len = 0;
        p256 = EVP_PKEY_get_utf8_string_param(pub, OSSL_PKEY_PARAM_GROUP_NAME, grp, sizeof grp,
                                              &len) == 1 &&
               std::strcmp(grp, "prime256v1") == 0;
    }
    ERR_clear_error();
    if (!p256) { err = "csr_bad_key_type"; return false; }
    if (X509_REQ_get_signature_nid(req.get()) != NID_ecdsa_with_SHA256) {
        err = "csr_bad_signature_algorithm";
        return false;
    }
    if (X509_REQ_verify(req.get(), pub) != 1) {
        ERR_clear_error();
        err = "csr_bad_signature";
        return false;
    }

    // Exactly one RDN, CN = "LiveQX Device CA <digits>". The subject is not
    // taken over blindly: this CSR becomes a CA certificate.
    X509_NAME* subj = X509_REQ_get_subject_name(req.get());
    char cn[128] = {0};
    bool subject_ok = X509_NAME_entry_count(subj) == 1 &&
                      X509_NAME_get_text_by_NID(subj, NID_commonName, cn, sizeof cn) > 0;
    static const char kPrefix[] = "LiveQX Device CA ";
    if (subject_ok) {
        const std::string s(cn);
        subject_ok = s.rfind(kPrefix, 0) == 0 && s.size() > sizeof(kPrefix) - 1 &&
                     s.size() <= sizeof(kPrefix) - 1 + 6 &&
                     s.find_first_not_of("0123456789", sizeof(kPrefix) - 1) == std::string::npos;
    }
    if (!subject_ok) { err = "csr_bad_subject"; return false; }
    if (validity_days <= 0 || validity_days > kMaxIntermediateDays) {
        err = "bad_validity";
        return false;
    }

    // ── the root ──
    auto root_pem = readFile(root_dir / "root.crt");
    auto sealed   = readFile(root_dir / "root.key.sealed");
    if (!root_pem || !sealed) { err = "root_unreadable"; return false; }
    Cert root = certFromPem(*root_pem);
    if (!root) { err = "root_unreadable"; return false; }

    kw::Key scalar{};
    switch (kw::unwrap(*sealed, passphrase, scalar)) {
        case kw::UnwrapStatus::Ok: break;
        case kw::UnwrapStatus::WrongPassphrase: err = "wrong_passphrase"; return false;
        default: err = "root_unreadable"; return false;
    }
    Pkey root_key = keyFromScalar(scalar);
    sodium_memzero(scalar.data(), scalar.size());
    if (!root_key || X509_check_private_key(root.get(), root_key.get()) != 1) {
        ERR_clear_error();
        err = "root_unreadable";
        return false;
    }

    // ── the intermediate ──
    const std::int64_t now = now_unix ? now_unix : wallNow();
    const std::int64_t na  = now + std::int64_t{validity_days} * 86400;
    Time na_t(ASN1_TIME_set(nullptr, static_cast<time_t>(na)));
    if (!na_t || ASN1_TIME_compare(na_t.get(), X509_get0_notAfter(root.get())) > 0) {
        err = "validity_exceeds_root";
        return false;
    }

    Cert cert(X509_new());
    auto serial = randomSerial();
    if (!cert || !serial) { err = "internal_error"; return false; }
    X509_set_version(cert.get(), 2);
    X509_set_serialNumber(cert.get(), serial.get());
    X509_NAME* sn = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(sn, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert.get(), X509_get_subject_name(root.get()));
    if (!setTimes(cert.get(), now - kSkewSec, na) ||
        !X509_set_pubkey(cert.get(), pub) ||
        // pathlen 0: this CA may issue players' certificates, never another CA.
        !addExt(cert.get(), root.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
        !addExt(cert.get(), root.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExt(cert.get(), root.get(), NID_subject_key_identifier, "hash") ||
        !addExt(cert.get(), root.get(), NID_authority_key_identifier, "keyid") ||
        !X509_sign(cert.get(), root_key.get(), EVP_sha256())) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }
    cert_pem = certToPem(cert.get());
    if (cert_pem.empty()) { err = "internal_error"; return false; }
    return true;
}

}  // namespace liveqx::tls
