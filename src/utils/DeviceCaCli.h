#pragma once
//
// Operator commands for the two-tier device CA ceremony (see utils/DeviceCa.h
// and utils/DeviceRootCa.h):
//
//   --device-ca-root-init   --out DIR                       (offline machine)
//   --device-ca-request                                      (server)
//   --device-ca-root-sign   --root-dir DIR --csr FILE --out FILE [--days N]
//                                                            (offline machine)
//   --device-ca-import      --cert FILE --root FILE          (server)
//
// Return values are process exit codes (0 = done). Passphrases come from the
// given source (terminal without echo, or stdin), never from arguments.

#include <filesystem>
#include <string>

#include "auth/KeyProtectCli.h"
#include "auth/KeyWrap.h"

namespace liveqx::tls {

int deviceCaRootInit(const std::filesystem::path& out_dir,
                     const liveqx::auth::PassphraseSource& passphrase,
                     const liveqx::auth::keywrap::KdfParams& kdf =
                         liveqx::auth::keywrap::KdfParams::moderate());

int deviceCaRequest(const std::filesystem::path& master_key_file,
                    const std::filesystem::path& device_ca_dir,
                    const std::string& key_store,
                    const liveqx::auth::PassphraseSource& passphrase);

int deviceCaRootSign(const std::filesystem::path& root_dir,
                     const std::filesystem::path& csr_file,
                     const std::filesystem::path& out_file,
                     int validity_days,
                     const liveqx::auth::PassphraseSource& passphrase);

int deviceCaImport(const std::filesystem::path& master_key_file,
                   const std::filesystem::path& device_ca_dir,
                   const std::string& key_store,
                   const std::filesystem::path& cert_file,
                   const std::filesystem::path& root_file,
                   const liveqx::auth::PassphraseSource& passphrase);

}  // namespace liveqx::tls
