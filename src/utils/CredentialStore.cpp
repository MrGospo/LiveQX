// systemd-creds access — see utils/CredentialStore.h.

#include "utils/CredentialStore.h"

#include "mounts/Subprocess.h"

#include <sodium.h>

#include <sys/stat.h>

#include <cstring>

namespace fs = std::filesystem;

namespace liveqx::util {
namespace {

std::string findSystemdCreds() {
    for (const char* p : {"/usr/bin/systemd-creds", "/bin/systemd-creds"}) {
        struct stat st{};
        if (::stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) return p;
    }
    return {};
}

class SystemdCredentialStore final : public CredentialStore {
public:
    bool decrypt(const fs::path& file, const std::string& name, Key& out,
                 std::string& err) override {
        const std::string bin = findSystemdCreds();
        if (bin.empty()) { err = "systemd-creds not found (needs systemd 250 or newer)"; return false; }

        liveqx::mounts::Subprocess sp;
        sp.argv = {bin, "decrypt", "--name=" + name, file.string(), "-"};
        sp.timeout_ms = std::chrono::milliseconds(20000);
        auto r = sp.run();
        const bool ok = r.exit_code == 0 && !r.timed_out && r.stdout_data.size() == out.size();
        if (ok) std::memcpy(out.data(), r.stdout_data.data(), out.size());
        if (!r.stdout_data.empty()) sodium_memzero(r.stdout_data.data(), r.stdout_data.size());
        if (!ok) {
            err = "systemd-creds decrypt failed";
            if (r.timed_out) err += " (timed out)";
            else if (r.exit_code == 0) err += ": the credential is not a raw 32-byte key";
            else if (!r.stderr_data.empty()) err += ": " + r.stderr_data;
            return false;
        }
        return true;
    }

    bool encrypt(const Key& key, const fs::path& file, const std::string& name,
                 std::string& err) override {
        const std::string bin = findSystemdCreds();
        if (bin.empty()) { err = "systemd-creds not found (needs systemd 250 or newer)"; return false; }

        // TPM2 as well, when this machine has one (exit 0 = fully usable).
        liveqx::mounts::Subprocess probe;
        probe.argv = {bin, "has-tpm2"};
        probe.timeout_ms = std::chrono::milliseconds(10000);
        const bool tpm = probe.run().exit_code == 0;

        liveqx::mounts::Subprocess sp;
        sp.argv = {bin, "encrypt", std::string("--with-key=") + (tpm ? "host+tpm2" : "host"),
                   "--name=" + name, "-", file.string()};
        sp.stdin_data.assign(reinterpret_cast<const char*>(key.data()), key.size());
        sp.timeout_ms = std::chrono::milliseconds(20000);
        auto r = sp.run();
        if (!sp.stdin_data.empty()) sodium_memzero(sp.stdin_data.data(), sp.stdin_data.size());
        if (r.exit_code != 0 || r.timed_out) {
            err = "systemd-creds encrypt failed";
            if (r.timed_out) err += " (timed out)";
            else if (!r.stderr_data.empty()) err += ": " + r.stderr_data;
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<CredentialStore> makeSystemdCredentialStore() {
    return std::make_unique<SystemdCredentialStore>();
}

}  // namespace liveqx::util
