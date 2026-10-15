#pragma once
//
// The OFFLINE root of the two-tier device CA (see utils/DeviceCa.h).
//
// The root exists to sign an intermediate and to be trusted by the server and
// the broker. Its private key never lives on the LiveQX server: it is created
// by an operator on another machine (or at least moved off the server right
// after), sealed with an operator passphrase (Argon2id, auth/KeyWrap.h).
//
// Files in the root directory:
//   root.crt         root certificate (PEM, 0644) — copied to the server
//   root.key.sealed  the P-256 private scalar, sealed (LQXKEY1 file, 0600)
//
// Error strings are short stable codes, not prose.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "auth/KeyWrap.h"

namespace liveqx::tls {

// Creates root.crt + root.key.sealed in `out_dir`. Refuses ("root_already_exists")
// if either file is already there — a root is never overwritten.
bool createDeviceRoot(const std::filesystem::path& out_dir,
                      std::string_view passphrase,
                      std::string& err,
                      const liveqx::auth::keywrap::KdfParams& kdf =
                          liveqx::auth::keywrap::KdfParams::moderate(),
                      int validity_days = 7300,
                      std::int64_t now_unix = 0);

// Signs an intermediate-CA request produced by DeviceCa::createIntermediateRequest.
// The CSR must be PEM, EC P-256, signed ecdsa-with-SHA256, and its subject must
// be exactly one CN of the form "LiveQX Device CA <number>". The certificate is
// CA:TRUE with pathlen 0 (it can issue players' certificates, not other CAs)
// and never outlives the root.
// err: bad_csr_pem, csr_too_large, csr_bad_key_type, csr_bad_signature_algorithm,
//      csr_bad_signature, csr_bad_subject, bad_validity, root_unreadable,
//      wrong_passphrase, validity_exceeds_root, internal_error
bool signDeviceIntermediate(const std::filesystem::path& root_dir,
                            std::string_view passphrase,
                            std::string_view csr_pem,
                            int validity_days,
                            std::string& cert_pem,
                            std::string& err,
                            std::int64_t now_unix = 0);

}  // namespace liveqx::tls
