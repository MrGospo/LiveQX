// Passphrase protection commands — see auth/KeyProtectCli.h.

#include "auth/KeyProtectCli.h"

#include "auth/MasterKey.h"
#include "utils/FileOwnership.h"
#include "utils/RuntimeLock.h"

#include <sodium.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

namespace liveqx::auth {
namespace {

constexpr std::size_t kMaxKeyFileBytes = 16 * 1024;

// A string that wipes itself.
struct Secret {
    std::string s;
    Secret() = default;
    explicit Secret(std::string v) : s(std::move(v)) {}
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret() { if (!s.empty()) sodium_memzero(s.data(), s.size()); }
};

int fail(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
    return 1;
}

// Common refusals shared by all three commands.
bool preflight(const fs::path& key_file, const fs::path& state_dir,
               const std::string& key_store, std::string& err) {
    if (key_store != "file") {
        err = "master_key_store is \"" + key_store + "\": the key is delivered "
              "from outside and cannot be rewritten here";
        return false;
    }
    if (std::getenv("LIVEQX_MASTER_KEY") != nullptr) {
        err = "LIVEQX_MASTER_KEY is set: the key comes from the environment "
              "and cannot be rewritten here";
        return false;
    }
    if (!state_dir.empty()) {
        std::string probe_err;
        switch (util::RuntimeLock::probe(state_dir / "liveqx.lock", probe_err)) {
            case util::RuntimeLock::Probe::NotRunning: break;
            case util::RuntimeLock::Probe::Running:
                err = "LiveQX is running. Stop the service first";
                return false;
            case util::RuntimeLock::Probe::Error:
                err = "cannot check whether LiveQX is running: " + probe_err;
                return false;
        }
    }
    std::error_code ec;
    if (!fs::exists(key_file, ec)) {
        err = "master key file not found at " + key_file.string();
        return false;
    }
    return true;
}

bool readKeyFile(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot open " + p.string(); return false; }
    out.assign(kMaxKeyFileBytes + 1, '\0');
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(f.gcount()));
    if (out.size() > kMaxKeyFileBytes) { err = "key file is too large"; return false; }
    return true;
}

// Writes `bytes` next to `dest` with mode 0600, checks it with `verify`, and
// renames it over `dest`. The tmp file never exists with other permissions.
bool replaceFile(const fs::path& dest, const std::string& bytes,
                 const std::function<bool(const std::string&)>& verify,
                 std::string& err) {
    const fs::path tmp = dest.string() + ".new";
    std::error_code ec;
    fs::remove(tmp, ec);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { err = "cannot create " + tmp.string() + ": " + std::strerror(errno); return false; }
    util::copyOwnerAndMode(fd, dest);   // keep who may read the key
    const auto n = ::write(fd, bytes.data(), bytes.size());
    const bool wrote = (n == static_cast<ssize_t>(bytes.size())) && ::fsync(fd) == 0;
    ::close(fd);
    if (!wrote) {
        fs::remove(tmp, ec);
        err = "short write to " + tmp.string();
        return false;
    }
    std::string back, rerr;
    if (!readKeyFile(tmp, back, rerr) || back != bytes || !verify(back)) {
        fs::remove(tmp, ec);
        err = "the new key file did not verify; the old one is untouched";
        return false;
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "cannot replace " + dest.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Asks for a new passphrase twice and applies the length rule.
bool askNewPassphrase(const PassphraseSource& src, Secret& out, std::string& err) {
    auto p1 = src("New passphrase");
    if (!p1) { err = "no passphrase available"; return false; }
    Secret a(std::move(*p1));
    auto p2 = src("Repeat the new passphrase");
    if (!p2) { err = "no passphrase available"; return false; }
    Secret b(std::move(*p2));
    if (a.s != b.s) { err = "the two passphrases do not match"; return false; }
    if (a.s.size() < kMinPassphraseLength) {
        err = "the passphrase must be at least " +
              std::to_string(kMinPassphraseLength) + " characters long";
        return false;
    }
    out.s = a.s;
    return true;
}

// Opens a wrapped key file with a passphrase from the source.
bool openWrapped(const std::string& file, const PassphraseSource& src,
                 keywrap::Key& key, std::string& err) {
    auto p = src("Current passphrase");
    if (!p) { err = "no passphrase available"; return false; }
    Secret pass(std::move(*p));
    switch (keywrap::unwrap(file, pass.s, key)) {
        case keywrap::UnwrapStatus::Ok: return true;
        case keywrap::UnwrapStatus::WrongPassphrase:
            err = "wrong passphrase (or the key file is damaged)";
            return false;
        case keywrap::UnwrapStatus::BadParams:
            err = "the key file has unacceptable key-derivation parameters";
            return false;
        default:
            err = "the key file is malformed";
            return false;
    }
}

bool sameKeyAfter(const std::string& file, const std::string& pass,
                  const keywrap::Key& expected) {
    keywrap::Key k{};
    const bool ok = keywrap::unwrap(file, pass, k) == keywrap::UnwrapStatus::Ok &&
                    k == expected;
    sodium_memzero(k.data(), k.size());
    return ok;
}

}  // namespace

std::optional<std::string> promptNewPassphrase(const PassphraseSource& passphrase,
                                               std::string& err) {
    Secret s;
    if (!askNewPassphrase(passphrase, s, err)) return std::nullopt;
    std::string out = s.s;
    return out;
}

PassphraseSource makeStreamPassphraseSource(std::istream& in) {
    return [&in](std::string_view) -> std::optional<std::string> {
        std::string line;
        if (!std::getline(in, line)) return std::nullopt;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    };
}

PassphraseSource makeTerminalPassphraseSource() {
    return [](std::string_view prompt) -> std::optional<std::string> {
        const int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd < 0) return std::nullopt;
        termios old{};
        if (::tcgetattr(fd, &old) != 0) { ::close(fd); return std::nullopt; }
        termios noecho = old;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(fd, TCSAFLUSH, &noecho);
        const std::string p = std::string(prompt) + ": ";
        (void)!::write(fd, p.data(), p.size());

        std::string line;
        char c;
        while (::read(fd, &c, 1) == 1 && c != '\n') line.push_back(c);
        ::tcsetattr(fd, TCSAFLUSH, &old);
        (void)!::write(fd, "\n", 1);
        ::close(fd);
        return line;
    };
}

int protectMasterKey(const fs::path& key_file, const fs::path& state_dir,
                     const std::string& key_store, const PassphraseSource& passphrase,
                     const keywrap::KdfParams& kdf) {
    std::string err;
    if (!preflight(key_file, state_dir, key_store, err)) return fail(err);

    std::string current;
    if (!readKeyFile(key_file, current, err)) return fail(err);
    if (keywrap::isWrapped(current))
        return fail("the master key is already passphrase protected");
    if (current.size() != keywrap::kKeyBytes)
        return fail("the key file is neither a bare 32-byte key nor a wrapped key");

    keywrap::Key key{};
    std::memcpy(key.data(), current.data(), key.size());
    sodium_memzero(current.data(), current.size());

    Secret pass;
    if (!askNewPassphrase(passphrase, pass, err)) {
        sodium_memzero(key.data(), key.size());
        return fail(err);
    }
    const std::string wrapped = keywrap::wrap(key, pass.s, kdf);
    if (wrapped.empty()) {
        sodium_memzero(key.data(), key.size());
        return fail("cannot wrap the key");
    }
    const bool ok = replaceFile(key_file, wrapped,
        [&](const std::string& f) { return sameKeyAfter(f, pass.s, key); }, err);
    const std::string fp = keywrap::fingerprintOf(key);
    sodium_memzero(key.data(), key.size());
    if (!ok) return fail(err);

    std::cout << "master_key_protected: true\n"
              << "fingerprint: " << fp << "\n"
              << "key_file: " << key_file << "\n"
              << "The service now starts sealed and waits for the passphrase.\n";
    return 0;
}

int unprotectMasterKey(const fs::path& key_file, const fs::path& state_dir,
                       const std::string& key_store, const PassphraseSource& passphrase) {
    std::string err;
    if (!preflight(key_file, state_dir, key_store, err)) return fail(err);

    std::string current;
    if (!readKeyFile(key_file, current, err)) return fail(err);
    if (!keywrap::isWrapped(current))
        return fail("the master key is not passphrase protected");

    keywrap::Key key{};
    if (!openWrapped(current, passphrase, key, err)) return fail(err);

    const std::string bare(reinterpret_cast<const char*>(key.data()), key.size());
    const bool ok = replaceFile(key_file, bare,
        [&](const std::string& f) {
            return f.size() == key.size() && std::memcmp(f.data(), key.data(), key.size()) == 0;
        }, err);
    const std::string fp = keywrap::fingerprintOf(key);
    sodium_memzero(key.data(), key.size());
    if (!ok) return fail(err);

    std::cout << "master_key_protected: false\n"
              << "fingerprint: " << fp << "\n"
              << "key_file: " << key_file << "\n";
    return 0;
}

int changeMasterPassphrase(const fs::path& key_file, const fs::path& state_dir,
                           const std::string& key_store, const PassphraseSource& passphrase,
                           const keywrap::KdfParams& kdf) {
    std::string err;
    if (!preflight(key_file, state_dir, key_store, err)) return fail(err);

    std::string current;
    if (!readKeyFile(key_file, current, err)) return fail(err);
    if (!keywrap::isWrapped(current))
        return fail("the master key is not passphrase protected");

    keywrap::Key key{};
    if (!openWrapped(current, passphrase, key, err)) return fail(err);

    Secret pass;
    if (!askNewPassphrase(passphrase, pass, err)) {
        sodium_memzero(key.data(), key.size());
        return fail(err);
    }
    const std::string wrapped = keywrap::wrap(key, pass.s, kdf);
    if (wrapped.empty()) {
        sodium_memzero(key.data(), key.size());
        return fail("cannot wrap the key");
    }
    const bool ok = replaceFile(key_file, wrapped,
        [&](const std::string& f) { return sameKeyAfter(f, pass.s, key); }, err);
    const std::string fp = keywrap::fingerprintOf(key);
    sodium_memzero(key.data(), key.size());
    if (!ok) return fail(err);

    std::cout << "master_passphrase_changed: true\n"
              << "fingerprint: " << fp << "\n"
              << "key_file: " << key_file << "\n";
    return 0;
}

}  // namespace liveqx::auth
