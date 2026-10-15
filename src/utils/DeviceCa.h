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
//   <dir>/gen-<N>/root.crt    two-tier only: the offline root that signed ca.crt
//   <dir>/gen-<N>.pending/    two-tier only: a request waiting for the root's
//                             signature (ca.key.enc + ca.csr); not a generation
// The active generation is the highest N. Every issued certificate and every
// CRL belongs to exactly one generation; one CRL per issuing generation.
//
// Two modes. One-tier: ca.crt is self-signed (lab setups). Two-tier: ca.crt is
// an INTERMEDIATE signed by an offline root (utils/DeviceRootCa.h); the root's
// key never touches this server. Players present their certificate together
// with chainPem() (the intermediate), servers and the broker trust rootPem().
// Replacing the intermediate (a new generation under the same root) needs no
// change on any player.
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
    std::string  chain_pem;         // two-tier: the intermediate (client_chain_pem); else empty
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
    // Opens the CA in `dir`. With `allow_create`, a directory without any
    // generation gets a self-signed one-tier generation 1; without it (the
    // two-tier setup) that is an error and NOTHING is created — a one-tier CA
    // appearing by itself would defeat keeping the root offline. Returns
    // nullptr and fills `err` when an existing generation cannot be read
    // (missing or tampered key blob, key not matching the certificate, wrong
    // master key, intermediate not signed by its root) — never regenerates
    // over existing material.
    static std::unique_ptr<DeviceCa> open(const std::filesystem::path& dir,
                                          const liveqx::auth::MasterKey& key,
                                          std::string& err,
                                          bool allow_create = true);

    // ── two-tier: getting an intermediate signed ──
    // Creates the key of the next generation (sealed with MasterKey) and a CSR
    // for the offline root to sign. Calling it again while a request is
    // pending returns the same CSR (no orphaned keys).
    static bool createIntermediateRequest(const std::filesystem::path& dir,
                                          const liveqx::auth::MasterKey& key,
                                          std::string& csr_pem,
                                          std::string& err);

    // Turns the pending request into a generation, once `cert_pem` (signed by
    // the root in `root_pem`) is shown to belong to the pending key, to be a
    // CA with pathlen 0, to be valid now and not to outlive the root.
    // err: no_pending_request, bad_certificate_pem, bad_root_pem,
    //      root_not_a_ca, certificate_not_signed_by_root,
    //      certificate_not_an_intermediate_ca, certificate_does_not_match_request,
    //      certificate_not_currently_valid, certificate_outlives_root,
    //      generation_exists, pending_key_unreadable, cannot_write_files
    static bool importIntermediate(const std::filesystem::path& dir,
                                   const liveqx::auth::MasterKey& key,
                                   std::string_view cert_pem,
                                   std::string_view root_pem,
                                   std::string& err,
                                   std::int64_t now_unix = 0);

    ~DeviceCa();
    DeviceCa(const DeviceCa&) = delete;
    DeviceCa& operator=(const DeviceCa&) = delete;

    // Active generation number (>= 1).
    int generation() const noexcept;

    // Certificate (PEM) of the active generation's issuing CA: self-signed in
    // one-tier mode, the intermediate in two-tier mode.
    const std::string& certPem() const noexcept;

    bool twoTier() const noexcept;
    // Two-tier: the intermediate players present with their certificate
    // (client_chain_pem). Empty in one-tier mode.
    const std::string& chainPem() const noexcept;
    // Two-tier: the offline root that servers and the broker trust. Empty in
    // one-tier mode.
    const std::string& rootPem() const noexcept;

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
    // csr_bad_signature_algorithm, bad_player_id, bad_validity,
    // ca_validity_too_short — the certificate would outlive its issuing CA).
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
