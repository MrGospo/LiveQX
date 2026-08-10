#pragma once
//
// fix43 — plaintext credential writer for CIFS mounts in /run.
//
// Backstory (fix41→fix42→fix43):
//   • fix41 stored CIFS creds encrypted via `systemd-creds encrypt` in
//     /etc/credstore.encrypted/ and referenced them from .mount units via
//     `LoadCredentialEncrypted=cifs-creds:<path>` + `credentials=%d/cifs-creds`.
//   • That works for [Service] units but NOT for [Mount] units — systemd
//     accepts the directive without error yet never populates
//     `/run/credentials/<unit>/` for mount-type units. mount.cifs then
//     fails with `mount error(2): No such file or directory` because
//     %d expands to an empty path. Verified on systemd 255 (Ubuntu 24.04).
//   • fix43 drops `LoadCredentialEncrypted=` and switches to plaintext
//     cred files under `/run/liveqx/creds/` (tmpfs, mode 0700
//     root:root). `Options=credentials=<absolute-path>` lets mount.cifs
//     read username/password directly. Plaintext on tmpfs disappears on
//     reboot, never hits disk backups; permissions ensure only root can
//     read them.
//
// ICredsHelper is an abstraction so MountdHandlers tests stay free of
// real filesystem I/O.

#include <filesystem>
#include <string>

namespace liveqx::mounts {

class ICredsHelper {
public:
    virtual ~ICredsHelper() = default;

    // Writes payload to out_path atomically (tmp+rename), mode 0600
    // root:root. Creates parent directory if missing. Returns false on
    // I/O failure; err carries a human-readable reason.
    virtual bool writeFile(const std::string&           payload,
                           const std::filesystem::path& out_path,
                           std::string&                 err) = 0;

    // Removes a credential file. true and noop if file does not exist.
    virtual bool removeFile(const std::filesystem::path& path,
                            std::string&                 err) = 0;
};

// Default production impl: atomic open(O_CREAT|O_TRUNC, 0600) → write →
// fsync → rename. No subprocess, no TPM, no external binary. The cred
// file is intended to live under a tmpfs RuntimeDirectory so it never
// survives a reboot.
class PlaintextCredsHelper final : public ICredsHelper {
public:
    PlaintextCredsHelper() = default;

    bool writeFile (const std::string&           payload,
                    const std::filesystem::path& out_path,
                    std::string&                 err) override;
    bool removeFile(const std::filesystem::path& path,
                    std::string&                 err) override;
};

}  // namespace liveqx::mounts
