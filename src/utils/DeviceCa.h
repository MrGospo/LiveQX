#pragma once
//
// Device certificate authority — issues client certificates to players
// (LiveQX-Player, phase 2). Separate from the internal server CA in
// utils/Tls.h: its own directory, its own trust chain, independent of the
// TLS mode of the control API.
//
// Layout (generations from day one, so a CA can be replaced later without
// invalidating the model):
//   <dir>/gen-<N>/ca.crt      CA certificate, PEM, 0644
//   <dir>/gen-<N>/ca.key.enc  CA private key (PEM) sealed with MasterKey, 0600
// The active generation is the highest N. Every issued certificate and every
// CRL belongs to exactly one generation; one CRL per issuing generation.
//
// The CA private key is never written in clear. Losing master.key therefore
// loses the CA: master-key rotation must re-seal it (see resealAll).
//
// The header is OpenSSL-free at the API boundary.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::auth { class MasterKey; }

namespace liveqx::tls {

// Result of signClientCsr().
struct IssuedClientCert {
    std::string  cert_pem;
    std::string  serial_hex;        // uppercase, no separators; store in DB
    int          generation{0};     // issuing CA generation; store in DB
    std::int64_t not_before_unix{0};
    std::int64_t not_after_unix{0};
};

// One revoked certificate in a CRL.
struct RevokedCert {
    std::string  serial_hex;
    std::int64_t revoked_at_unix{0};
};

class DeviceCa {
public:
    // Opens the CA in `dir`. Creates generation 1 when the directory holds no
    // generation at all. Returns nullptr and fills `err` when an existing
    // generation cannot be read (missing or tampered key blob, key not
    // matching the certificate, wrong master key) — never regenerates over
    // existing material.
    static std::unique_ptr<DeviceCa> open(const std::filesystem::path& dir,
                                          const liveqx::auth::MasterKey& key,
                                          std::string& err);

    ~DeviceCa();
    DeviceCa(const DeviceCa&) = delete;
    DeviceCa& operator=(const DeviceCa&) = delete;

    // Active generation number (>= 1).
    int generation() const noexcept;

    // CA certificate (PEM) of the active generation — goes into the pairing
    // package as ca_cert_pem.
    const std::string& certPem() const noexcept;

    // Signs a player CSR. The CSR must be a valid PEM PKCS#10 with an EC
    // P-256 public key, an ecdsa-with-SHA256 signature that verifies, and be
    // at most 4096 bytes. `player_id` must be a lowercase UUID v4 and becomes
    // the subject CN; the CSR subject and requested extensions are ignored.
    // Issued cert: CA:FALSE, KeyUsage digitalSignature, EKU clientAuth,
    // notBefore = now - 24h (device clocks drift), notAfter = now +
    // validity_days. `now_unix` is the OS wall clock (what peers verify
    // against); the default 0 means "read it".
    // Returns false and fills `err` with a stable short reason
    // (bad_csr_pem, csr_too_large, csr_bad_key_type, csr_bad_signature,
    // csr_bad_signature_algorithm, bad_player_id, bad_validity).
    bool signClientCsr(std::string_view csr_pem,
                       std::string_view player_id,
                       int validity_days,
                       IssuedClientCert& out,
                       std::string& err,
                       std::int64_t now_unix = 0) const;

    // Builds a CRL (PEM) signed by the active generation. `crl_number` must be
    // monotonically increasing per generation (caller persists it);
    // nextUpdate = now + next_update_days.
    bool generateCrl(const std::vector<RevokedCert>& revoked,
                     std::int64_t crl_number,
                     int next_update_days,
                     std::string& crl_pem,
                     std::string& err,
                     std::int64_t now_unix = 0) const;

    // Re-seals the CA key of every generation under `new_key`; used by master
    // key rotation. Each blob is decrypted with `old_key` first; nothing is
    // written unless all decrypt. Rolls back already written files on a
    // failed write. A missing `dir` is not an error (no device CA yet).
    static bool resealAll(const std::filesystem::path& dir,
                          const liveqx::auth::MasterKey& old_key,
                          const liveqx::auth::MasterKey& new_key,
                          std::string& err);

private:
    struct Impl;
    explicit DeviceCa(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace liveqx::tls
