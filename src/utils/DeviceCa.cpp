// Device certificate authority — see utils/DeviceCa.h for the contract.
//
// Self-contained on purpose: utils/Tls.cpp keeps its OpenSSL helpers in an
// anonymous namespace and models a different thing (file-based server CA with
// a clear-text key). Here the CA key only ever exists in memory or sealed.

#include "utils/DeviceCa.h"

#include "auth/MasterKey.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace liveqx::tls {
namespace {

constexpr std::size_t kMaxCsrBytes = 4096;
constexpr int kMaxValidityDays = 3650;
constexpr int kCaValidityDays  = 3650;
constexpr std::int64_t kNotBeforeSkewSec = 24 * 3600;

struct PkeyDel { void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); } };
struct X509Del { void operator()(X509* p) const noexcept { X509_free(p); } };
struct ReqDel  { void operator()(X509_REQ* p) const noexcept { X509_REQ_free(p); } };
struct CrlDel  { void operator()(X509_CRL* p) const noexcept { X509_CRL_free(p); } };
struct BioDel  { void operator()(BIO* p) const noexcept { BIO_free_all(p); } };
struct BnDel   { void operator()(BIGNUM* p) const noexcept { BN_free(p); } };
struct AsnDel  { void operator()(ASN1_INTEGER* p) const noexcept { ASN1_INTEGER_free(p); } };
struct TimeDel { void operator()(ASN1_TIME* p) const noexcept { ASN1_TIME_free(p); } };

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;
using Cert = std::unique_ptr<X509, X509Del>;
using Req  = std::unique_ptr<X509_REQ, ReqDel>;
using Crl  = std::unique_ptr<X509_CRL, CrlDel>;
using Bio  = std::unique_ptr<BIO, BioDel>;
using Bn   = std::unique_ptr<BIGNUM, BnDel>;
using Asn  = std::unique_ptr<ASN1_INTEGER, AsnDel>;
using Time = std::unique_ptr<ASN1_TIME, TimeDel>;

std::string sslErr() {
    unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (!e) return "openssl error";
    char buf[200];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

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

// Private key PEM in a buffer that is wiped on destruction.
struct SecretPem {
    std::string s;
    SecretPem() = default;
    SecretPem(const SecretPem&) = delete;
    SecretPem& operator=(const SecretPem&) = delete;
    ~SecretPem() { OPENSSL_cleanse(s.data(), s.size()); }
};

bool keyToPem(EVP_PKEY* k, SecretPem& out) {
    Bio b(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_PrivateKey(b.get(), k, nullptr, nullptr, 0, nullptr, nullptr))
        return false;
    out.s = bioToString(b.get());
    char* d = nullptr;
    BIO_get_mem_data(b.get(), &d);
    if (d) OPENSSL_cleanse(d, out.s.size());
    return !out.s.empty();
}

Pkey keyFromPem(const std::string& pem) {
    Bio b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return Pkey(PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr));
}

// tmp file + chmod + rename: the final name never exists with wrong mode.
bool atomicWrite(const fs::path& dest, std::string_view bytes, fs::perms mode,
                 std::string& err) {
    fs::path tmp = dest;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { err = "open " + tmp.string() + ": " + std::strerror(errno); return false; }
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        o.flush();
        if (!o) {
            err = "write " + tmp.string();
            o.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::permissions(tmp, mode, fs::perm_options::replace, ec);
    if (ec) { err = "chmod " + tmp.string() + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    fs::rename(tmp, dest, ec);
    if (ec) { err = "rename " + dest.string() + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

// gen-<digits> directory name -> number, or 0.
int parseGenName(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 4, "gen-") != 0) return 0;
    int n = 0;
    for (std::size_t i = 4; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return 0;
        n = n * 10 + (name[i] - '0');
        if (n > 1'000'000) return 0;
    }
    return n;
}

std::map<int, fs::path> listGenerations(const fs::path& dir) {
    std::map<int, fs::path> gens;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        int n = parseGenName(it->path().filename().string());
        if (n > 0) gens.emplace(n, it->path());
    }
    return gens;
}

Asn randomSerial() {
    Bn bn(BN_new());
    if (!bn || !BN_rand(bn.get(), 63, BN_RAND_TOP_TWO, BN_RAND_BOTTOM_ANY)) return nullptr;
    return Asn(BN_to_ASN1_INTEGER(bn.get(), nullptr));
}

std::string serialToHex(const ASN1_INTEGER* a) {
    Bn bn(ASN1_INTEGER_to_BN(a, nullptr));
    if (!bn) return {};
    char* hex = BN_bn2hex(bn.get());
    if (!hex) return {};
    std::string s(hex);
    OPENSSL_free(hex);
    return s;
}

bool addExt(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!e) return false;
    int rc = X509_add_ext(cert, e, -1);
    X509_EXTENSION_free(e);
    return rc == 1;
}

bool setTime(int (*setter)(X509*, const ASN1_TIME*), X509* c, std::int64_t t) {
    Time at(ASN1_TIME_set(nullptr, static_cast<time_t>(t)));
    return at && setter(c, at.get()) == 1;
}

bool isLowerUuidV4(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    if (s[14] != '4') return false;
    char v = s[19];
    return v == '8' || v == '9' || v == 'a' || v == 'b';
}

bool isHex(std::string_view s) {
    if (s.empty() || s.size() > 40) return false;
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Generates a CA certificate for `gen` and its key.
bool generateCa(int gen, Pkey& key, Cert& cert, std::string& err) {
    key.reset(EVP_EC_gen("P-256"));
    if (!key) { err = "EVP_EC_gen: " + sslErr(); return false; }
    cert.reset(X509_new());
    if (!cert) { err = "X509_new"; return false; }
    X509_set_version(cert.get(), 2);
    auto serial = randomSerial();
    if (!serial || !X509_set_serialNumber(cert.get(), serial.get())) {
        err = "serial: " + sslErr();
        return false;
    }
    std::string cn = "LiveQX Device CA " + std::to_string(gen);
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn.data()),
                               static_cast<int>(cn.size()), -1, 0);
    X509_set_issuer_name(cert.get(), name);
    std::int64_t now = wallNow();
    if (!setTime(X509_set1_notBefore, cert.get(), now - kNotBeforeSkewSec) ||
        !setTime(X509_set1_notAfter, cert.get(), now + std::int64_t{kCaValidityDays} * 86400)) {
        err = "validity: " + sslErr();
        return false;
    }
    X509_set_pubkey(cert.get(), key.get());
    if (!addExt(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
        !addExt(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExt(cert.get(), cert.get(), NID_subject_key_identifier, "hash")) {
        err = "extensions: " + sslErr();
        return false;
    }
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
        err = "X509_sign: " + sslErr();
        return false;
    }
    return true;
}

}  // namespace

struct DeviceCa::Impl {
    fs::path dir;
    int      generation{0};
    Cert     cert;
    Pkey     key;
    std::string cert_pem;
};

DeviceCa::DeviceCa(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DeviceCa::~DeviceCa() = default;

int DeviceCa::generation() const noexcept { return impl_->generation; }
const std::string& DeviceCa::certPem() const noexcept { return impl_->cert_pem; }

std::unique_ptr<DeviceCa> DeviceCa::open(const fs::path& dir,
                                         const liveqx::auth::MasterKey& key,
                                         std::string& err) {
    err.clear();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { err = "create_directories: " + ec.message(); return nullptr; }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);

    auto gens = listGenerations(dir);
    if (gens.empty()) {
        // Build generation 1 in a scratch directory and rename it into place,
        // so a crash never leaves a half-written gen-1 that open() would
        // then (rightly) refuse forever.
        fs::path scratch = dir / "gen-1.tmp";
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch, ec);
        if (ec) { err = "create_directories: " + ec.message(); return nullptr; }
        fs::permissions(scratch, fs::perms::owner_all, fs::perm_options::replace, ec);

        Pkey ca_key;
        Cert ca_cert;
        if (!generateCa(1, ca_key, ca_cert, err)) { fs::remove_all(scratch, ec); return nullptr; }
        SecretPem pem;
        if (!keyToPem(ca_key.get(), pem)) {
            err = "serialize CA key";
            fs::remove_all(scratch, ec);
            return nullptr;
        }
        auto blob = key.encrypt(pem.s);
        if (blob.empty()) {
            err = "cannot seal CA key";
            fs::remove_all(scratch, ec);
            return nullptr;
        }
        std::string cert_pem = certToPem(ca_cert.get());
        if (!atomicWrite(scratch / "ca.key.enc",
                         std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()),
                         fs::perms::owner_read | fs::perms::owner_write, err) ||
            !atomicWrite(scratch / "ca.crt", cert_pem,
                         fs::perms::owner_read | fs::perms::owner_write |
                         fs::perms::group_read | fs::perms::others_read, err)) {
            fs::remove_all(scratch, ec);
            return nullptr;
        }
        fs::rename(scratch, dir / "gen-1", ec);
        if (ec) {
            err = "rename gen-1: " + ec.message();
            fs::remove_all(scratch, ec);
            return nullptr;
        }
        gens = listGenerations(dir);
    }

    const auto& [gen, gdir] = *gens.rbegin();
    auto cert_pem = readFile(gdir / "ca.crt");
    auto blob     = readFile(gdir / "ca.key.enc");
    if (!cert_pem || !blob) {
        err = gdir.filename().string() + ": ca.crt or ca.key.enc is missing";
        return nullptr;
    }
    std::vector<std::uint8_t> sealed(blob->begin(), blob->end());
    auto plain = key.decrypt(sealed);
    if (!plain) {
        err = gdir.filename().string() +
              ": CA key cannot be decrypted (wrong master key or damaged file)";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->key = keyFromPem(*plain);
    OPENSSL_cleanse(plain->data(), plain->size());
    impl->cert = certFromPem(*cert_pem);
    if (!impl->key || !impl->cert) {
        ERR_clear_error();
        err = gdir.filename().string() + ": CA certificate or key is not valid PEM";
        return nullptr;
    }
    if (X509_check_private_key(impl->cert.get(), impl->key.get()) != 1 ||
        X509_check_ca(impl->cert.get()) == 0) {
        ERR_clear_error();
        err = gdir.filename().string() + ": CA key does not match its certificate";
        return nullptr;
    }
    impl->dir = dir;
    impl->generation = gen;
    impl->cert_pem = *cert_pem;
    return std::unique_ptr<DeviceCa>(new DeviceCa(std::move(impl)));
}

bool DeviceCa::signClientCsr(std::string_view csr_pem, std::string_view player_id,
                             int validity_days, IssuedClientCert& out,
                             std::string& err, std::int64_t now_unix) const {
    err.clear();
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
        p256 = EVP_PKEY_get_utf8_string_param(pub, OSSL_PKEY_PARAM_GROUP_NAME, grp,
                                              sizeof grp, &len) == 1 &&
               std::strcmp(grp, "prime256v1") == 0;
    }
    if (!p256) { ERR_clear_error(); err = "csr_bad_key_type"; return false; }
    if (X509_REQ_get_signature_nid(req.get()) != NID_ecdsa_with_SHA256) {
        err = "csr_bad_signature_algorithm";
        return false;
    }
    if (X509_REQ_verify(req.get(), pub) != 1) {
        ERR_clear_error();
        err = "csr_bad_signature";
        return false;
    }
    if (!isLowerUuidV4(player_id)) { err = "bad_player_id"; return false; }
    if (validity_days <= 0 || validity_days > kMaxValidityDays) {
        err = "bad_validity";
        return false;
    }

    const std::int64_t now = now_unix ? now_unix : wallNow();
    Cert cert(X509_new());
    if (!cert) { err = "internal_error"; return false; }
    X509_set_version(cert.get(), 2);
    auto serial = randomSerial();
    if (!serial || !X509_set_serialNumber(cert.get(), serial.get())) {
        err = "internal_error";
        return false;
    }
    // Subject is built from scratch: whatever the CSR claims is ignored.
    X509_NAME* subj = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(player_id.data()),
                               static_cast<int>(player_id.size()), -1, 0);
    X509_set_issuer_name(cert.get(), X509_get_subject_name(impl_->cert.get()));

    const std::int64_t nb = now - kNotBeforeSkewSec;
    const std::int64_t na = now + std::int64_t{validity_days} * 86400;
    if (!setTime(X509_set1_notBefore, cert.get(), nb) ||
        !setTime(X509_set1_notAfter, cert.get(), na) ||
        !X509_set_pubkey(cert.get(), pub)) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }
    if (!addExt(cert.get(), impl_->cert.get(), NID_basic_constraints, "critical,CA:FALSE") ||
        !addExt(cert.get(), impl_->cert.get(), NID_key_usage, "critical,digitalSignature") ||
        !addExt(cert.get(), impl_->cert.get(), NID_ext_key_usage, "clientAuth") ||
        !addExt(cert.get(), impl_->cert.get(), NID_subject_key_identifier, "hash") ||
        !addExt(cert.get(), impl_->cert.get(), NID_authority_key_identifier, "keyid")) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }
    if (!X509_sign(cert.get(), impl_->key.get(), EVP_sha256())) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }

    out.cert_pem        = certToPem(cert.get());
    out.serial_hex      = serialToHex(serial.get());
    out.generation      = impl_->generation;
    out.not_before_unix = nb;
    out.not_after_unix  = na;
    if (out.cert_pem.empty() || out.serial_hex.empty()) { err = "internal_error"; return false; }
    return true;
}

bool DeviceCa::generateCrl(const std::vector<RevokedCert>& revoked,
                           std::int64_t crl_number, int next_update_days,
                           std::string& crl_pem, std::string& err,
                           std::int64_t now_unix) const {
    err.clear();
    if (crl_number <= 0) { err = "bad_crl_number"; return false; }
    if (next_update_days <= 0) { err = "bad_validity"; return false; }
    for (const auto& r : revoked)
        if (!isHex(r.serial_hex)) { err = "bad_serial"; return false; }

    const std::int64_t now = now_unix ? now_unix : wallNow();
    Crl crl(X509_CRL_new());
    if (!crl) { err = "internal_error"; return false; }
    X509_CRL_set_version(crl.get(), 1);
    X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(impl_->cert.get()));
    Time last(ASN1_TIME_set(nullptr, static_cast<time_t>(now)));
    Time next(ASN1_TIME_set(nullptr, static_cast<time_t>(now + std::int64_t{next_update_days} * 86400)));
    if (!last || !next || !X509_CRL_set1_lastUpdate(crl.get(), last.get()) ||
        !X509_CRL_set1_nextUpdate(crl.get(), next.get())) {
        err = "internal_error";
        return false;
    }
    for (const auto& r : revoked) {
        BIGNUM* raw = nullptr;
        if (!BN_hex2bn(&raw, r.serial_hex.c_str())) { err = "bad_serial"; return false; }
        Bn bn(raw);
        Asn ser(BN_to_ASN1_INTEGER(bn.get(), nullptr));
        Time when(ASN1_TIME_set(nullptr, static_cast<time_t>(r.revoked_at_unix)));
        X509_REVOKED* rev = X509_REVOKED_new();
        if (!ser || !when || !rev || !X509_REVOKED_set_serialNumber(rev, ser.get()) ||
            !X509_REVOKED_set_revocationDate(rev, when.get()) ||
            !X509_CRL_add0_revoked(crl.get(), rev)) {
            if (rev) X509_REVOKED_free(rev);
            err = "internal_error";
            return false;
        }
    }
    Asn num(ASN1_INTEGER_new());
    if (!num || !ASN1_INTEGER_set_int64(num.get(), crl_number) ||
        X509_CRL_add1_ext_i2d(crl.get(), NID_crl_number, num.get(), 0, 0) != 1) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }
    X509_CRL_sort(crl.get());
    if (!X509_CRL_sign(crl.get(), impl_->key.get(), EVP_sha256())) {
        ERR_clear_error();
        err = "internal_error";
        return false;
    }
    Bio out(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_X509_CRL(out.get(), crl.get())) { err = "internal_error"; return false; }
    crl_pem = bioToString(out.get());
    return true;
}

bool DeviceCa::resealAll(const fs::path& dir, const liveqx::auth::MasterKey& old_key,
                         const liveqx::auth::MasterKey& new_key, std::string& err) {
    err.clear();
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;

    struct Item {
        fs::path path;
        std::string old_blob;
        std::vector<std::uint8_t> new_blob;
    };
    std::vector<Item> items;
    for (const auto& [gen, gdir] : listGenerations(dir)) {
        Item it;
        it.path = gdir / "ca.key.enc";
        auto blob = readFile(it.path);
        if (!blob) {
            err = "device CA " + gdir.filename().string() + ": ca.key.enc is missing";
            return false;
        }
        it.old_blob = std::move(*blob);
        std::vector<std::uint8_t> sealed(it.old_blob.begin(), it.old_blob.end());
        auto plain = old_key.decrypt(sealed);
        if (!plain) {
            err = "device CA " + gdir.filename().string() + " key cannot be decrypted";
            return false;
        }
        it.new_blob = new_key.encrypt(*plain);
        OPENSSL_cleanse(plain->data(), plain->size());
        if (it.new_blob.empty()) {
            err = "device CA " + gdir.filename().string() + " key cannot be re-sealed";
            return false;
        }
        items.push_back(std::move(it));
    }

    const auto mode = fs::perms::owner_read | fs::perms::owner_write;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::string werr;
        if (atomicWrite(items[i].path,
                        std::string_view(reinterpret_cast<const char*>(items[i].new_blob.data()),
                                         items[i].new_blob.size()),
                        mode, werr))
            continue;
        err = "device CA re-seal failed: " + werr;
        std::string not_restored;
        for (std::size_t j = 0; j < i; ++j) {
            std::string rerr;
            if (!atomicWrite(items[j].path, items[j].old_blob, mode, rerr))
                not_restored += (not_restored.empty() ? "" : ", ") +
                                items[j].path.parent_path().filename().string();
        }
        if (!not_restored.empty()) err += "; ROLLBACK FAILED for: " + not_restored;
        return false;
    }
    return true;
}

}  // namespace liveqx::tls
