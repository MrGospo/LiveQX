// fix38 — TLS bootstrap primitives implementation.
//
// Layered on the OpenSSL EVP/X509 APIs. Every function isolates OpenSSL
// resource ownership through unique_ptr deleters so an early return
// cannot leak a BIGNUM/EVP_PKEY/X509 even on the error path. We never
// touch the legacy OPENSSL_malloc'd char* APIs except via OPENSSL_free,
// and that only inside utility helpers below.
//
// A note on private-key permissions: std::filesystem::permissions on
// Linux maps onto chmod(), which is exactly what we want — but only
// after the rename, otherwise the .tmp file would briefly exist with
// the umask-default 0644. Tests verify the final mode is 0600.

#include "utils/Tls.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace liveqx::tls {
namespace {

// ─── OpenSSL handle deleters ────────────────────────────────────────────────
struct EVP_PKEY_deleter   { void operator()(EVP_PKEY* p)       const noexcept { EVP_PKEY_free(p);       } };
struct X509_deleter       { void operator()(X509* p)           const noexcept { X509_free(p);           } };
struct BIO_deleter        { void operator()(BIO* p)            const noexcept { BIO_free_all(p);        } };
struct BIGNUM_deleter     { void operator()(BIGNUM* p)         const noexcept { BN_free(p);             } };
struct X509_NAME_deleter  { void operator()(X509_NAME* p)      const noexcept { X509_NAME_free(p);      } };
struct X509_EXT_deleter   { void operator()(X509_EXTENSION* p) const noexcept { X509_EXTENSION_free(p); } };
struct GENERAL_NAMES_deleter { void operator()(GENERAL_NAMES* p) const noexcept { sk_GENERAL_NAME_pop_free(p, GENERAL_NAME_free); } };
struct ASN1_INT_deleter   { void operator()(ASN1_INTEGER* p)   const noexcept { ASN1_INTEGER_free(p);   } };

using EVP_PKEY_ptr   = std::unique_ptr<EVP_PKEY,       EVP_PKEY_deleter>;
using X509_ptr       = std::unique_ptr<X509,           X509_deleter>;
using BIO_ptr        = std::unique_ptr<BIO,            BIO_deleter>;
using BIGNUM_ptr     = std::unique_ptr<BIGNUM,         BIGNUM_deleter>;
using GENNAMES_ptr   = std::unique_ptr<GENERAL_NAMES,  GENERAL_NAMES_deleter>;
using ASN1_INT_ptr   = std::unique_ptr<ASN1_INTEGER,   ASN1_INT_deleter>;

// ─── Error helpers ──────────────────────────────────────────────────────────
std::string lastSslError() {
    BIO_ptr bio(BIO_new(BIO_s_mem()));
    if (!bio) return "<bio_new failed>";
    ERR_print_errors(bio.get());
    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) return "<no openssl error>";
    return std::string(data, static_cast<size_t>(len));
}

// ─── Atomic file writer ─────────────────────────────────────────────────────
//
// Writes `bytes` to `dest` via dest.tmp + rename. The tmp file is
// removed on any failure path. The final file's permissions are
// determined by `mode` (0600 for keys, 0644 for certs).
bool atomicWrite(const fs::path& dest, std::string_view bytes,
                 fs::perms mode, std::string& err) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    // create_directories returning false with no ec is fine (already existed)
    if (ec) {
        err = "create_directories failed: " + ec.message();
        return false;
    }

    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            err = "open " + tmp.string() + " for write: " + std::strerror(errno);
            return false;
        }
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!o) {
            err = "write " + tmp.string() + ": " + std::strerror(errno);
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        err = "rename " + tmp.string() + " -> " + dest.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    fs::permissions(dest, mode,
                    fs::perm_options::replace, ec);
    if (ec) {
        err = "chmod " + dest.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// ─── PEM I/O ────────────────────────────────────────────────────────────────
EVP_PKEY_ptr loadKey(const fs::path& p, std::string& err) {
    BIO_ptr bio(BIO_new_file(p.string().c_str(), "r"));
    if (!bio) { err = "open key " + p.string(); return nullptr; }
    EVP_PKEY* k = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    if (!k) { err = "parse key " + p.string() + ": " + lastSslError(); return nullptr; }
    return EVP_PKEY_ptr(k);
}

X509_ptr loadCert(const fs::path& p, std::string& err) {
    BIO_ptr bio(BIO_new_file(p.string().c_str(), "r"));
    if (!bio) { err = "open cert " + p.string(); return nullptr; }
    X509* c = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (!c) { err = "parse cert " + p.string() + ": " + lastSslError(); return nullptr; }
    return X509_ptr(c);
}

bool writeKeyPem(const fs::path& p, EVP_PKEY* key, std::string& err) {
    BIO_ptr bio(BIO_new(BIO_s_mem()));
    if (!bio) { err = "BIO_new"; return false; }
    if (!PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr)) {
        err = "PEM_write_bio_PrivateKey: " + lastSslError();
        return false;
    }
    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) { err = "PEM key buffer empty"; return false; }
    return atomicWrite(p, std::string_view(data, static_cast<size_t>(len)),
                       fs::perms::owner_read | fs::perms::owner_write, err);
}

bool writeCertPem(const fs::path& p, X509* cert, std::string& err) {
    BIO_ptr bio(BIO_new(BIO_s_mem()));
    if (!bio) { err = "BIO_new"; return false; }
    if (!PEM_write_bio_X509(bio.get(), cert)) {
        err = "PEM_write_bio_X509: " + lastSslError();
        return false;
    }
    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) { err = "PEM cert buffer empty"; return false; }
    return atomicWrite(p, std::string_view(data, static_cast<size_t>(len)),
                       fs::perms::owner_read  | fs::perms::owner_write |
                       fs::perms::group_read  | fs::perms::others_read,
                       err);
}

// ─── Key + cert primitives ──────────────────────────────────────────────────
EVP_PKEY_ptr genEcP256(std::string& err) {
    EVP_PKEY_ptr key(EVP_EC_gen("P-256"));
    if (!key) err = "EVP_EC_gen P-256: " + lastSslError();
    return key;
}

ASN1_INT_ptr randomSerial(std::string& err) {
    // 64-bit random unsigned, encoded as ASN1_INTEGER. Plenty unique
    // for a single internal CA's certificate stream and avoids the
    // negative-serial pitfall (BN_rand sets BN_FLG_NEG=0 here).
    BIGNUM_ptr bn(BN_new());
    if (!bn) { err = "BN_new"; return nullptr; }
    if (!BN_rand(bn.get(), 63, BN_RAND_TOP_TWO, BN_RAND_BOTTOM_ANY)) {
        err = "BN_rand: " + lastSslError();
        return nullptr;
    }
    ASN1_INT_ptr a(BN_to_ASN1_INTEGER(bn.get(), nullptr));
    if (!a) err = "BN_to_ASN1_INTEGER: " + lastSslError();
    return a;
}

bool addExtension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx{};
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) return false;
    int rc = X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return rc == 1;
}

// SAN line for X509V3_EXT_conf_nid: comma-separated list of typed entries.
// Example: "DNS:core.lan,DNS:localhost,IP:192.168.1.10,IP:::1".
std::string buildSanLine(const SanList& sans) {
    std::ostringstream out;
    bool first = true;
    auto add = [&](const char* prefix, const std::string& v) {
        if (!first) out << ',';
        first = false;
        out << prefix << v;
    };
    for (const auto& d : sans.dns_names) add("DNS:", d);
    for (const auto& v : sans.ip_v4)     add("IP:",  v);
    for (const auto& v : sans.ip_v6)     add("IP:",  v);
    return out.str();
}

void setSubjectCN(X509* cert, std::string_view cn) {
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn.data()),
        static_cast<int>(cn.size()), -1, 0);
}

// ─── Reading metadata ───────────────────────────────────────────────────────
std::string nameToString(X509_NAME* name) {
    if (!name) return {};
    BIO_ptr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    // RFC2253 — same flavour as openssl x509 -subject -nameopt RFC2253
    X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) return {};
    return std::string(data, static_cast<size_t>(len));
}

std::string serialHex(X509* cert) {
    ASN1_INTEGER* a = X509_get_serialNumber(cert);
    if (!a) return {};
    BIGNUM_ptr bn(ASN1_INTEGER_to_BN(a, nullptr));
    if (!bn) return {};
    char* hex = BN_bn2hex(bn.get());
    if (!hex) return {};
    std::string s(hex);
    OPENSSL_free(hex);
    return s;
}

std::string fingerprintSha256(X509* cert) {
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (!X509_digest(cert, EVP_sha256(), buf, &len) || len == 0) return {};
    std::ostringstream o;
    o << std::uppercase << std::hex << std::setfill('0');
    for (unsigned i = 0; i < len; ++i) {
        if (i) o << ':';
        o << std::setw(2) << static_cast<int>(buf[i]);
    }
    return o.str();
}

std::int64_t asn1ToUnix(const ASN1_TIME* t) {
    if (!t) return 0;
    // Diff from epoch: pass tm=epoch (1970-01-01) to ASN1_TIME_diff.
    int days = 0, secs = 0;
    ASN1_TIME* epoch = ASN1_TIME_set(nullptr, 0);
    if (!epoch) return 0;
    int rc = ASN1_TIME_diff(&days, &secs, epoch, t);
    ASN1_STRING_free(reinterpret_cast<ASN1_STRING*>(epoch));
    if (rc != 1) return 0;
    return static_cast<std::int64_t>(days) * 86400 + secs;
}

void extractSans(X509* cert, std::vector<std::string>& dns,
                 std::vector<std::string>& ip) {
    auto* names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!names) return;
    GENNAMES_ptr guard(names);
    int n = sk_GENERAL_NAME_num(names);
    for (int i = 0; i < n; ++i) {
        GENERAL_NAME* g = sk_GENERAL_NAME_value(names, i);
        if (!g) continue;
        if (g->type == GEN_DNS) {
            const unsigned char* data = ASN1_STRING_get0_data(g->d.dNSName);
            int len = ASN1_STRING_length(g->d.dNSName);
            if (data && len > 0)
                dns.emplace_back(reinterpret_cast<const char*>(data),
                                 static_cast<size_t>(len));
        } else if (g->type == GEN_IPADD) {
            const unsigned char* data = ASN1_STRING_get0_data(g->d.iPAddress);
            int len = ASN1_STRING_length(g->d.iPAddress);
            char buf[INET6_ADDRSTRLEN] = {0};
            if (len == 4 && inet_ntop(AF_INET, data, buf, sizeof(buf))) {
                ip.emplace_back(buf);
            } else if (len == 16 && inet_ntop(AF_INET6, data, buf, sizeof(buf))) {
                ip.emplace_back(buf);
            }
        }
    }
}

bool isCa(X509* cert) {
    return X509_check_ca(cert) != 0;
}

}  // namespace

// ─── Public API ─────────────────────────────────────────────────────────────
CaResult ensureCa(const fs::path& dir,
                  std::string_view common_name,
                  int validity_days) {
    CaResult r;
    r.cert_path = dir / "ca.crt";
    r.key_path  = dir / "ca.key";
    r.generated = false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { r.error = "create_directories: " + ec.message(); return r; }
    fs::permissions(dir,
                    fs::perms::owner_all,
                    fs::perm_options::replace, ec);
    // chmod failure on the dir is non-fatal; key files still get 0600.

    // If both files exist and are coherent, return existing CA.
    if (fs::exists(r.cert_path) && fs::exists(r.key_path)) {
        std::string lerr;
        auto existing_cert = loadCert(r.cert_path, lerr);
        auto existing_key  = loadKey (r.key_path,  lerr);
        if (existing_cert && existing_key &&
            X509_check_private_key(existing_cert.get(), existing_key.get()) == 1 &&
            isCa(existing_cert.get())) {
            return r;  // generated=false, error=""
        }
        // else: fall through and regenerate
    }

    EVP_PKEY_ptr key = genEcP256(r.error);
    if (!key) return r;

    X509_ptr cert(X509_new());
    if (!cert) { r.error = "X509_new"; return r; }

    X509_set_version(cert.get(), 2);  // X509 v3
    auto serial = randomSerial(r.error);
    if (!serial) return r;
    X509_set_serialNumber(cert.get(), serial.get());

    setSubjectCN(cert.get(), common_name);
    X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get()));

    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter (cert.get()),
                    static_cast<long>(validity_days) * 86400L);

    X509_set_pubkey(cert.get(), key.get());

    if (!addExtension(cert.get(), cert.get(), NID_basic_constraints,
                      "critical,CA:TRUE")) {
        r.error = "ext basicConstraints: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), cert.get(), NID_key_usage,
                      "critical,keyCertSign,cRLSign,digitalSignature")) {
        r.error = "ext keyUsage: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), cert.get(), NID_subject_key_identifier, "hash")) {
        r.error = "ext SKI: " + lastSslError(); return r;
    }

    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
        r.error = "X509_sign: " + lastSslError(); return r;
    }

    if (!writeKeyPem (r.key_path,  key.get(),  r.error)) return r;
    if (!writeCertPem(r.cert_path, cert.get(), r.error)) return r;

    r.generated = true;
    return r;
}

ServerCertResult issueServerCert(const fs::path& dir,
                                 std::string_view common_name,
                                 const SanList& sans,
                                 int validity_days) {
    ServerCertResult r;
    r.cert_path = dir / "server.crt";
    r.key_path  = dir / "server.key";

    if (sans.empty()) {
        r.error = "SAN list is empty — modern TLS clients require at least "
                  "one SubjectAltName";
        return r;
    }

    auto ca_cert = loadCert(dir / "ca.crt", r.error);
    if (!ca_cert) return r;
    auto ca_key  = loadKey (dir / "ca.key", r.error);
    if (!ca_key)  return r;

    EVP_PKEY_ptr key = genEcP256(r.error);
    if (!key) return r;

    X509_ptr cert(X509_new());
    if (!cert) { r.error = "X509_new"; return r; }

    X509_set_version(cert.get(), 2);
    auto serial = randomSerial(r.error);
    if (!serial) return r;
    X509_set_serialNumber(cert.get(), serial.get());

    setSubjectCN(cert.get(), common_name);
    X509_set_issuer_name(cert.get(), X509_get_subject_name(ca_cert.get()));

    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter (cert.get()),
                    static_cast<long>(validity_days) * 86400L);

    X509_set_pubkey(cert.get(), key.get());

    if (!addExtension(cert.get(), ca_cert.get(), NID_basic_constraints,
                      "critical,CA:FALSE")) {
        r.error = "ext basicConstraints: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), ca_cert.get(), NID_key_usage,
                      "critical,digitalSignature,keyEncipherment")) {
        r.error = "ext keyUsage: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), ca_cert.get(), NID_ext_key_usage, "serverAuth")) {
        r.error = "ext extendedKeyUsage: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), ca_cert.get(), NID_subject_key_identifier, "hash")) {
        r.error = "ext SKI: " + lastSslError(); return r;
    }
    if (!addExtension(cert.get(), ca_cert.get(), NID_authority_key_identifier,
                      "keyid:always")) {
        r.error = "ext AKI: " + lastSslError(); return r;
    }
    auto san_line = buildSanLine(sans);
    if (!addExtension(cert.get(), ca_cert.get(), NID_subject_alt_name,
                      san_line.c_str())) {
        r.error = "ext SAN '" + san_line + "': " + lastSslError(); return r;
    }

    if (!X509_sign(cert.get(), ca_key.get(), EVP_sha256())) {
        r.error = "X509_sign: " + lastSslError(); return r;
    }

    if (!writeKeyPem (r.key_path,  key.get(),  r.error)) return r;
    if (!writeCertPem(r.cert_path, cert.get(), r.error)) return r;

    r.fingerprint_sha256 = fingerprintSha256(cert.get());
    r.not_after_unix     = asn1ToUnix(X509_get0_notAfter(cert.get()));
    return r;
}

CertInfo readCertInfo(const fs::path& cert_path) {
    CertInfo info;
    std::string err;
    auto cert = loadCert(cert_path, err);
    if (!cert) return info;

    info.subject              = nameToString(X509_get_subject_name(cert.get()));
    info.issuer               = nameToString(X509_get_issuer_name (cert.get()));
    info.serial_hex           = serialHex(cert.get());
    info.fingerprint_sha256   = fingerprintSha256(cert.get());

    int sig_nid = X509_get_signature_nid(cert.get());
    info.signature_algorithm  = OBJ_nid2ln(sig_nid) ? OBJ_nid2ln(sig_nid) : "";

    EVP_PKEY* pk = X509_get0_pubkey(cert.get());
    if (pk) {
        int  pk_nid = EVP_PKEY_id(pk);
        const char* lname = OBJ_nid2ln(pk_nid);
        info.public_key_algorithm = lname ? lname : "";
    }

    extractSans(cert.get(), info.san_dns, info.san_ip);

    info.not_before_unix = asn1ToUnix(X509_get0_notBefore(cert.get()));
    info.not_after_unix  = asn1ToUnix(X509_get0_notAfter (cert.get()));
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    info.days_remaining = static_cast<int>((info.not_after_unix - now) / 86400);

    info.is_ca       = isCa(cert.get());
    info.self_signed = info.subject == info.issuer;
    return info;
}

VerifyResult verifyKeyPair(const fs::path& cert_path, const fs::path& key_path) {
    VerifyResult v;
    auto cert = loadCert(cert_path, v.error);
    if (!cert) return v;
    auto key  = loadKey (key_path,  v.error);
    if (!key)  return v;

    if (X509_check_private_key(cert.get(), key.get()) != 1) {
        v.error = "private key does not match certificate public key: "
                  + lastSslError();
        return v;
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto nb  = asn1ToUnix(X509_get0_notBefore(cert.get()));
    auto na  = asn1ToUnix(X509_get0_notAfter (cert.get()));
    if (now < nb) { v.error = "certificate not yet valid";  return v; }
    if (now > na) { v.error = "certificate has expired";    return v; }

    v.ok = true;
    return v;
}

VerifyResult verifyChain(const fs::path& cert_path, const fs::path& ca_path) {
    VerifyResult v;
    auto cert = loadCert(cert_path, v.error);
    if (!cert) return v;
    auto ca   = loadCert(ca_path,   v.error);
    if (!ca)   return v;

    EVP_PKEY* ca_pub = X509_get0_pubkey(ca.get());
    if (!ca_pub) { v.error = "CA cert has no public key"; return v; }
    if (X509_verify(cert.get(), ca_pub) != 1) {
        v.error = "certificate not signed by CA: " + lastSslError();
        return v;
    }
    v.ok = true;
    return v;
}

SanList autoDetectSans(const std::vector<std::string>& san_extra) {
    SanList out;
    std::unordered_set<std::string> seen_dns, seen_ip;

    auto add_dns = [&](const std::string& s) {
        if (s.empty()) return;
        if (!seen_dns.insert(s).second) return;
        out.dns_names.push_back(s);
    };
    auto add_ip = [&](const std::string& s, bool v6) {
        if (s.empty()) return;
        if (!seen_ip.insert(s).second) return;
        if (v6) out.ip_v6.push_back(s);
        else    out.ip_v4.push_back(s);
    };

    // Hostname
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') {
        add_dns(host);
    }
    add_dns("localhost");

    // NIC iteration
    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) == 0 && head) {
        for (auto* p = head; p; p = p->ifa_next) {
            if (!p->ifa_addr) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;
            if (p->ifa_flags & IFF_LOOPBACK) continue;
            char buf[INET6_ADDRSTRLEN] = {0};
            if (p->ifa_addr->sa_family == AF_INET) {
                auto* sa = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
                if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                    add_ip(buf, false);
                }
            } else if (p->ifa_addr->sa_family == AF_INET6) {
                auto* sa = reinterpret_cast<sockaddr_in6*>(p->ifa_addr);
                // Skip link-local (fe80::/10) — they require zone-id and
                // are not useful for cert SANs.
                if (IN6_IS_ADDR_LINKLOCAL(&sa->sin6_addr)) continue;
                if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf))) {
                    add_ip(buf, true);
                }
            }
        }
        freeifaddrs(head);
    }

    // Loopbacks always present so admin can reach the box from itself.
    add_ip("127.0.0.1", false);
    add_ip("::1",       true);

    for (const auto& extra : san_extra) {
        if (extra.empty()) continue;
        // Use inet_pton to classify — robust for IPv6 hex/abbrev forms
        // ("fd00::1") that ad-hoc isalpha() heuristics misclassify.
        struct in_addr  v4{};
        struct in6_addr v6{};
        if (inet_pton(AF_INET,  extra.c_str(), &v4) == 1) {
            add_ip(extra, false);
        } else if (inet_pton(AF_INET6, extra.c_str(), &v6) == 1) {
            add_ip(extra, true);
        } else {
            add_dns(extra);
        }
    }

    return out;
}

std::string writePem(const fs::path& dest, std::string_view pem_bytes,
                     bool is_secret) {
    if (pem_bytes.empty()) return "empty PEM payload";
    fs::perms mode = is_secret
        ? (fs::perms::owner_read | fs::perms::owner_write)
        : (fs::perms::owner_read | fs::perms::owner_write |
           fs::perms::group_read | fs::perms::others_read);
    std::string err;
    if (!atomicWrite(dest, pem_bytes, mode, err)) return err;
    return {};
}

}  // namespace liveqx::tls
