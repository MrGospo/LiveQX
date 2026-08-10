#pragma once
//
// fix38 — TLS bootstrap primitives.
//
// Wraps OpenSSL X509/EVP for two responsibilities the rest of the
// codebase needs:
//   1. Internal CA + server-cert generation when the operator has not
//      supplied their own PKI (auto mode, first boot).
//   2. Validation + introspection of admin-supplied or self-managed
//      certificates (provided mode + /api/tls/info + /api/tls/import).
//
// Algorithm: EC P-256 across both CA and server cert. P-256 is the
// modern default — universal browser/OS trust since 2007, ~10× faster
// signature ops than RSA-2048, and yields a noticeably smaller TLS
// handshake. CA validity defaults to 10 years; server certs to 1 year
// with rotation triggered externally (REST or expiry watchdog).
//
// All write operations are atomic: data is written to <path>.tmp and
// then std::filesystem::rename'd, so a SIGKILL mid-write cannot corrupt
// the existing file. Private keys are chmod'd to 0600 right after the
// rename, before the function returns.
//
// The header is OpenSSL-free at the API boundary so it can be included
// from headers that downstream consumers (ControlApi, main.cpp) pull in
// through normal "utils/Tls.h" includes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::tls {

// Subject Alternative Names embedded into the server certificate. At
// least one of the three vectors must be non-empty — TLS handshakes
// without a matching SAN fail in modern browsers (Chrome dropped CN
// fallback in 58, Firefox in 48).
struct SanList {
    std::vector<std::string> dns_names;   // e.g. "core.lan", "localhost"
    std::vector<std::string> ip_v4;       // dotted-quad ASCII
    std::vector<std::string> ip_v6;       // colon-hex ASCII

    bool empty() const noexcept {
        return dns_names.empty() && ip_v4.empty() && ip_v6.empty();
    }
};

// Cert metadata exposed through /api/tls/info and used to populate
// admin warnings ("expires in 14 days"). Times are unix seconds; -1
// indicates a parse error on the corresponding field.
struct CertInfo {
    std::string subject;                  // RFC2253 e.g. "CN=liveqx,O=..."
    std::string issuer;
    std::string serial_hex;               // uppercase, no separators
    std::string fingerprint_sha256;       // colon-separated uppercase hex
    std::string signature_algorithm;      // e.g. "ecdsa-with-SHA256"
    std::string public_key_algorithm;     // e.g. "id-ecPublicKey (P-256)"
    std::vector<std::string> san_dns;
    std::vector<std::string> san_ip;
    std::int64_t not_before_unix{0};
    std::int64_t not_after_unix{0};
    int          days_remaining{0};       // negative when expired
    bool         is_ca{false};
    bool         self_signed{false};
};

// Outcome of ensureCa(). When generated==false the CA was loaded from
// disk as-is — useful for boot-log messages ("CA already provisioned").
struct CaResult {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;
    bool        generated{false};
    std::string error;                    // empty on success
    bool ok() const noexcept { return error.empty(); }
};

// Outcome of issueServerCert() / loadServerCert(). The cert is always
// regenerated; callers decide rotation cadence.
struct ServerCertResult {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;
    std::string           fingerprint_sha256;
    std::int64_t          not_after_unix{0};
    std::string           error;
    bool ok() const noexcept { return error.empty(); }
};

// Outcome of verifyKeyPair() and verifyChain().
struct VerifyResult {
    bool        ok{false};
    std::string error;
};

// Idempotent. If both <dir>/ca.crt and <dir>/ca.key exist and parse as
// a self-signed CA whose key matches, returns generated=false. Otherwise
// generates a fresh EC P-256 keypair, builds a CA cert with
// BasicConstraints CA:TRUE + KeyUsage{keyCertSign,cRLSign,digitalSig},
// signs it self-signed with SHA-256, and writes both files atomically.
// The directory is created with 0700 if missing.
CaResult ensureCa(const std::filesystem::path& dir,
                  std::string_view             common_name = "LiveQX internal CA",
                  int                          validity_days = 3650);

// Issues a server cert signed by the CA at <dir>/ca.{crt,key}. Writes
// <dir>/server.crt + <dir>/server.key, overwriting any existing files
// atomically. SAN list must be non-empty. ExtendedKeyUsage=serverAuth,
// KeyUsage=digitalSignature+keyEncipherment, BasicConstraints CA:FALSE.
ServerCertResult issueServerCert(const std::filesystem::path& dir,
                                 std::string_view             common_name,
                                 const SanList&               sans,
                                 int                          validity_days = 365);

// Reads a cert PEM and returns metadata. Returns an info struct with
// empty subject/issuer + days_remaining=0 if the file is missing or
// malformed (caller checks subject.empty()).
CertInfo readCertInfo(const std::filesystem::path& cert_path);

// True iff the private key at key_path is the counterpart of the
// public key in cert_path's leaf certificate, AND the cert is within
// its validity window. Used at boot for `provided` mode and on every
// /api/tls/import attempt.
VerifyResult verifyKeyPair(const std::filesystem::path& cert_path,
                           const std::filesystem::path& key_path);

// True iff cert_path can be verified against ca_path. Used during
// /api/tls/import when the admin uploads a cert+key alongside the
// signing CA — we want to refuse certs that don't chain to the CA the
// operator says they trust.
VerifyResult verifyChain(const std::filesystem::path& cert_path,
                         const std::filesystem::path& ca_path);

// Best-effort SAN auto-detection for the auto mode: gethostname() +
// all non-loopback IPv4/IPv6 addresses on UP interfaces, plus any
// admin-supplied extras (config tls.san_extra). Duplicates are
// deduped; "localhost" + 127.0.0.1 + ::1 are always appended so the
// admin can always reach the box from the host itself.
SanList autoDetectSans(const std::vector<std::string>& san_extra);

// Atomic PEM write used by /api/tls/import. Writes via dest.tmp +
// rename and chmod's the result to 0600 when is_secret=true (private
// keys) or 0644 otherwise (certs / CA bundle). The caller is expected
// to validate parseability beforehand via verifyKeyPair / verifyChain.
// Returns "" on success or a human-readable error string.
std::string writePem(const std::filesystem::path& dest,
                     std::string_view             pem_bytes,
                     bool                         is_secret);

}  // namespace liveqx::tls
