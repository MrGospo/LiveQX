#pragma once
//
// Access to a master key stored as a systemd credential
// (/etc/credstore.encrypted/<name>, see packaging/install.sh).
//
// The service gets such a key through LoadCredentialEncrypted=; the recovery
// CLI needs to read the current one and write a rotated one. The interface
// exists so the rotation logic can be tested without a host key or TPM.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace liveqx::util {

class CredentialStore {
public:
    using Key = std::array<std::uint8_t, 32>;

    virtual ~CredentialStore() = default;

    // Decrypts the credential file `file`. `name` is the credential name it
    // was encrypted under (systemd refuses a credential presented under another).
    virtual bool decrypt(const std::filesystem::path& file, const std::string& name,
                         Key& out, std::string& err) = 0;

    // Encrypts `key` into `file` under `name`. Overwrites `file`.
    virtual bool encrypt(const Key& key, const std::filesystem::path& file,
                         const std::string& name, std::string& err) = 0;
};

// `systemd-creds` through execve (no shell). Encrypts with the host key and,
// when the machine has one, the TPM2 as well — the same choice the installer makes.
std::unique_ptr<CredentialStore> makeSystemdCredentialStore();

}  // namespace liveqx::util
