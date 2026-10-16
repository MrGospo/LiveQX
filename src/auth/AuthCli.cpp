// fix22 commit 10/24 — recovery-CLI helpers (см. AuthCli.h).
//
// Не использует JwtIssuer (т.е. не требует jwt_secret в конфиге) —
// CLI не выпускает токены. AuthService тут нужен только для
// bootstrapInitialAdmin / adminResetPassword, оба из них работают
// без grants_resolver (мы передаём пустой callback). Это сделано
// сознательно: recovery-CLI должен запускаться даже когда RBAC-конфиг
// сломан или auth.db только что разрушен.

#include "auth/AuthCli.h"

#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/MasterKey.h"
#include "mounts/MountsDb.h"
#include "utils/CredentialStore.h"
#include "utils/DeviceCa.h"
#include "utils/FileOwnership.h"
#include "utils/RuntimeLock.h"

#include "utils/Log.h"

namespace liveqx::auth {

namespace {

// JwtIssuer требует ≥32 байт secret. Для CLI он не используется по
// делу, но AuthService::ctor требует ссылку. Берём dummy-32-zero string —
// CLI не выпустит токенов, секрет в БД не утечёт.
constexpr const char* kCliDummySecret =
    "00000000000000000000000000000000";

}  // namespace

int printBootstrap(const std::filesystem::path& auth_db_path) {
    AuthDb db(auth_db_path);
    if (!db.open()) {
        std::cerr << "ERROR: cannot open auth.db at " << auth_db_path << "\n";
        return 1;
    }
    JwtIssuer jwt(std::string{kCliDummySecret});
    AuthService svc(db, jwt,
        [](std::int64_t) { return std::vector<ChannelGrant>{}; });

    auto out = svc.bootstrapInitialAdmin();
    if (!out.created) {
        // Уже есть admin — это не ошибка для CLI; печатаем NOTICE и
        // возвращаем 0, чтобы скрипты не падали на повторных запусках.
        std::cerr << "NOTICE: admin user already exists; bootstrap skipped\n";
        return 0;
    }
    // stdout — это тот единственный момент, когда plaintext-пароль
    // материализуется; пишем именно туда, чтобы оператор мог подхватить
    // через pipe/CI.
    std::cout << "username: " << out.username << "\n";
    std::cout << "password: " << out.plaintext_password << "\n";
    std::cout << "must_change_password: true\n";
    std::cout.flush();
    return 0;
}

int resetAdmin(const std::filesystem::path& auth_db_path,
               std::string_view username) {
    AuthDb db(auth_db_path);
    if (!db.open()) {
        std::cerr << "ERROR: cannot open auth.db at " << auth_db_path << "\n";
        return 1;
    }
    JwtIssuer jwt(std::string{kCliDummySecret});
    AuthService svc(db, jwt,
        [](std::int64_t) { return std::vector<ChannelGrant>{}; });

    auto user = db.findUserByUsername(username);
    if (!user) {
        std::cerr << "ERROR: user '" << username << "' not found\n";
        return 1;
    }
    // Защита от случайной смены пароля через CLI обычному viewer'у:
    // CLI задумано как admin-recovery. Если оператору нужно сменить
    // пароль viewer'у — пусть делает это через REST (там есть admin
    // RBAC и audit log).
    if (user->role != Role::Admin) {
        std::cerr << "ERROR: user '" << username << "' is not an admin "
                  << "(role=" << roleName(user->role) << "). "
                  << "CLI reset допускается только для admin'а.\n";
        return 1;
    }
    if (user->source != Source::Local) {
        std::cerr << "ERROR: user '" << username << "' authenticates via "
                  << sourceName(user->source) << ", "
                  << "пароль управляется внешней системой и не может быть сброшен через CLI.\n";
        return 1;
    }

    auto plaintext = svc.adminResetPassword(user->id);
    if (!plaintext) {
        std::cerr << "ERROR: reset failed (db error)\n";
        return 1;
    }
    std::cout << "username: " << user->username << "\n";
    std::cout << "password: " << *plaintext << "\n";
    std::cout << "must_change_password: true\n";
    std::cout.flush();
    return 0;
}

namespace detail {

bool reencryptAll(const std::vector<RotationEntry>& entries,
                  const MasterKey&                  new_key,
                  std::string&                      err,
                  std::vector<std::string>*         done_labels) {
    if (done_labels) done_labels->clear();
    std::vector<std::size_t> stored;   // indices already written
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const auto blob = new_key.encrypt(e.plaintext);
        if (blob.empty() || !e.store(blob)) {
            err = "cannot store re-encrypted " + e.label;
            // Put back whatever was already rotated, newest first, so a
            // failure in the middle never leaves storage half-rotated.
            std::string not_restored;
            for (auto it = stored.rbegin(); it != stored.rend(); ++it) {
                const auto& done = entries[*it];
                if (!done.store(done.old_blob)) {
                    if (!not_restored.empty()) not_restored += ", ";
                    not_restored += done.label;
                }
            }
            if (!not_restored.empty()) {
                err += "; ROLLBACK FAILED for: " + not_restored +
                       " (these are now encrypted with the NEW key; "
                       "restore the database from backup or re-enter them)";
            }
            if (done_labels) done_labels->clear();
            return false;
        }
        stored.push_back(i);
        if (done_labels) done_labels->push_back(e.label);
    }
    return true;
}

}  // namespace detail

namespace {

namespace fs = std::filesystem;
using FreshKey = std::array<std::uint8_t, MasterKey::kKeyBytes>;
using PassphraseFn = std::function<std::optional<std::string>(std::string_view)>;

// Renames `live` to a timestamped backup and `staged` into its place. If the
// second step fails the databases are already on the new key, so say exactly
// where everything is.
bool commitReplace(const fs::path& live, const fs::path& staged, std::string& backup) {
    const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path bak_path = live.string() + ".bak." + std::to_string(unix_ns);
    backup = bak_path.string();

    std::error_code ec;
    fs::rename(live, bak_path, ec);
    if (ec) {
        std::cerr << "ERROR: cannot back up the current key to " << bak_path
                  << ": " << ec.message() << "\n";
        // The staged file is NOT removed: it holds the key the secrets in the
        // databases are already encrypted with.
        std::cerr << "NOTE: the new key is at " << staged
                  << ". The secrets are already encrypted with it - move it into "
                     "place by hand: mv " << staged << " " << live << "\n";
        return false;
    }
    fs::rename(staged, live, ec);
    if (ec) {
        std::cerr << "ERROR: cannot move the new key into place: " << ec.message() << "\n";
        std::cerr << "NOTE: the backup is at " << bak_path << ", the new key at "
                  << staged << ". The secrets are already encrypted with the new "
                     "key - do NOT restore the backup without re-encrypting them.\n";
        return false;
    }
    return true;
}

// Where the master key lives and how a rotated one takes its place. The
// rotation itself (collect and check every secret, stage the new key, re-seal
// and re-encrypt, commit) is the same for every kind of storage.
class RotationKeyStore {
public:
    virtual ~RotationKeyStore() = default;
    virtual std::string where() const = 0;
    // Opens the current key. Prints the reason and returns nullptr on failure.
    virtual std::unique_ptr<MasterKey> openCurrent() = 0;
    // Writes the new key next to the current one without putting it in effect.
    virtual bool stage(const FreshKey& fresh) = 0;
    // Opens the staged key, proving it reads back.
    virtual std::unique_ptr<MasterKey> openStaged() = 0;
    virtual void discardStaged() = 0;
    // Backs the current key up and puts the staged one in its place.
    virtual bool commit(std::string& backup) = 0;
};

// A key file: bare, or wrapped with a passphrase (LQXKEY1).
class FileKeyStore final : public RotationKeyStore {
public:
    FileKeyStore(fs::path key_file, PassphraseFn passphrase, keywrap::KdfParams kdf)
        : key_file_(std::move(key_file)), tmp_(key_file_.string() + ".new"),
          passphrase_(std::move(passphrase)), kdf_(kdf) {}
    ~FileKeyStore() override { wipePass(); }

    std::string where() const override { return key_file_.string(); }

    std::unique_ptr<MasterKey> openCurrent() override {
        // The old key is the starting point: there must be a file to move
        // away from, not an empty source. The operator starts the server once
        // (auto-generation) or provides the key by hand.
        if (!fs::exists(key_file_)) {
            std::cerr << "ERROR: master key file not found at " << key_file_ << "\n";
            return nullptr;
        }
        auto old_key = std::make_unique<MasterKey>(key_file_.string());
        // A passphrase-wrapped key is opened with the operator's passphrase;
        // the new key is wrapped under the same one.
        wrapped_ = !old_key->load() && old_key->sealed();
        if (wrapped_) {
            if (!passphrase_) {
                std::cerr << "ERROR: the master key at " << key_file_
                          << " is passphrase-protected; run the rotation from a "
                             "terminal or with --passphrase-stdin\n";
                return nullptr;
            }
            auto p = passphrase_("Current passphrase");
            if (!p) {
                std::cerr << "ERROR: the master key is passphrase-protected and no "
                             "passphrase was supplied\n";
                return nullptr;
            }
            pass_ = std::move(*p);
            std::string unlock_err;
            if (!old_key->unlock(pass_, unlock_err)) {
                std::cerr << "ERROR: " << unlock_err << "\n";
                wipePass();
                return nullptr;
            }
        }
        if (!old_key->loaded()) {
            std::cerr << "ERROR: cannot load current master key from " << key_file_ << "\n";
            return nullptr;
        }

        // The new key must land where the service reads it. An environment
        // variable and a systemd credential are delivered from outside.
        switch (old_key->source()) {
            case MasterKey::Source::Env:
                std::cerr << "ERROR: the key comes from LIVEQX_MASTER_KEY; it cannot "
                             "be rotated here. Change the variable yourself and "
                             "re-encrypt the secrets.\n";
                return nullptr;
            case MasterKey::Source::EnvFile: {
                if (old_key->getInfo().protection == "systemd_credential") {
                    std::cerr << "ERROR: the key is a systemd credential delivered to "
                                 "this process; run the rotation with master_key_store "
                                 "set to \"creds\" in the config instead.\n";
                    return nullptr;
                }
                std::error_code eq_ec;
                if (!fs::equivalent(old_key->envFilePath(), key_file_, eq_ec)) {
                    std::cerr << "ERROR: the service reads its key from "
                              << old_key->envFilePath() << " (LIVEQX_MASTER_KEY_FILE), "
                              << "not from " << key_file_ << ".\n";
                    return nullptr;
                }
                break;
            }
            default:
                break;
        }
        return old_key;
    }

    bool stage(const FreshKey& fresh) override {
        std::error_code ec;
        fs::remove(tmp_, ec);   // leftovers of somebody else's unfinished rotation

        // What goes into the file: the bare key, or - for a passphrase-protected
        // key - the new key wrapped under the same passphrase.
        std::string bytes(reinterpret_cast<const char*>(fresh.data()), fresh.size());
        if (wrapped_) {
            keywrap::Key k{};
            std::memcpy(k.data(), fresh.data(), k.size());
            bytes = keywrap::wrap(k, pass_, kdf_);
            sodium_memzero(k.data(), k.size());
            if (bytes.empty()) {
                std::cerr << "ERROR: cannot wrap the new master key\n";
                return false;
            }
        }

        // O_EXCL so we never write through somebody else's file.
        const int fd = ::open(tmp_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            std::cerr << "ERROR: cannot create " << tmp_ << ": " << std::strerror(errno) << "\n";
            sodium_memzero(bytes.data(), bytes.size());
            return false;
        }
        const auto written = ::write(fd, bytes.data(), bytes.size());
        const bool short_write =
            written <= 0 || (!wrapped_ && written != static_cast<ssize_t>(MasterKey::kKeyBytes));
        sodium_memzero(bytes.data(), bytes.size());
        if (short_write) {
            std::cerr << "ERROR: short write to " << tmp_ << "\n";
            ::close(fd);
            fs::remove(tmp_, ec);
            return false;
        }
        if (::fsync(fd) != 0) {
            LOG_WARN("rotateMasterKey: fsync failed on {}: {}", tmp_.string(), std::strerror(errno));
        }
        // Same owner and mode as the key file being replaced (0600 stays 0600;
        // root:liveqx 0640 in /etc/liveqx stays readable by the service).
        util::copyOwnerAndMode(fd, key_file_);
        ::close(fd);
        return true;
    }

    std::unique_ptr<MasterKey> openStaged() override {
        // Read it back through the MasterKey API: the same encrypt/decrypt
        // pipeline, and proof that the file reads correctly (silent corruption).
        auto key = std::make_unique<MasterKey>(tmp_.string());
        bool ok = false;
        if (wrapped_) {
            std::string unlock_err;
            key->load();   // false by design: the file is wrapped
            ok = key->sealed() && key->unlock(pass_, unlock_err);
        } else {
            ok = key->load() && key->loaded();
        }
        wipePass();
        if (!ok) {
            std::cerr << "ERROR: cannot reload freshly-written master key at " << tmp_ << "\n";
            return nullptr;
        }
        return key;
    }

    void discardStaged() override {
        std::error_code ec;
        fs::remove(tmp_, ec);
    }

    bool commit(std::string& backup) override { return commitReplace(key_file_, tmp_, backup); }

private:
    void wipePass() {
        if (!pass_.empty()) sodium_memzero(pass_.data(), pass_.size());
        pass_.clear();
    }

    fs::path             key_file_;
    fs::path             tmp_;
    PassphraseFn         passphrase_;
    keywrap::KdfParams   kdf_;
    std::string          pass_;
    bool                 wrapped_{false};
};

// A systemd credential file (master_key_store = "creds").
class CredentialKeyStore final : public RotationKeyStore {
public:
    CredentialKeyStore(fs::path file, std::string name, util::CredentialStore& store)
        : file_(std::move(file)), staged_(file_.string() + ".new"),
          name_(std::move(name)), store_(store) {}

    std::string where() const override { return file_.string(); }

    std::unique_ptr<MasterKey> openCurrent() override {
        if (!fs::exists(file_)) {
            std::cerr << "ERROR: master key credential not found at " << file_ << "\n";
            return nullptr;
        }
        return readKey(file_, "cannot decrypt the master key credential");
    }

    bool stage(const FreshKey& fresh) override {
        std::error_code ec;
        fs::remove(staged_, ec);
        std::string err;
        if (!store_.encrypt(fresh, staged_, name_, err)) {
            std::cerr << "ERROR: cannot encrypt the new master key: " << err << "\n";
            fs::remove(staged_, ec);
            return false;
        }
        util::copyOwnerAndModePath(file_, staged_);   // keep who may read it
        return true;
    }

    std::unique_ptr<MasterKey> openStaged() override {
        return readKey(staged_, "cannot read back the new master key credential");
    }

    void discardStaged() override {
        std::error_code ec;
        fs::remove(staged_, ec);
    }

    bool commit(std::string& backup) override { return commitReplace(file_, staged_, backup); }

private:
    std::unique_ptr<MasterKey> readKey(const fs::path& file, const char* what) {
        util::CredentialStore::Key k{};
        std::string err;
        if (!store_.decrypt(file, name_, k, err)) {
            std::cerr << "ERROR: " << what << " " << file << ": " << err << "\n";
            return nullptr;
        }
        auto key = std::make_unique<MasterKey>("");
        key->loadRaw(k);
        sodium_memzero(k.data(), k.size());
        return key;
    }

    fs::path                 file_;
    fs::path                 staged_;
    std::string              name_;
    util::CredentialStore&   store_;
};

// The rotation, independent of where the key lives.
int rotateImpl(RotationKeyStore& keys,
               const fs::path& auth_db_path,
               const fs::path& mounts_db_path,
               const fs::path& device_ca_dir,
               const fs::path& state_dir) {
    // Rotating under a running service breaks it: for a moment the DB
    // points at the new key while the process holds the old one.
    if (!state_dir.empty()) {
        std::string probe_err;
        switch (liveqx::util::RuntimeLock::probe(state_dir / "liveqx.lock", probe_err)) {
            case liveqx::util::RuntimeLock::Probe::NotRunning: break;
            case liveqx::util::RuntimeLock::Probe::Running:
                std::cerr << "ERROR: LiveQX is running. Stop the service before "
                             "rotating the master key.\n";
                return 1;
            case liveqx::util::RuntimeLock::Probe::Error:
                std::cerr << "ERROR: cannot check whether LiveQX is running: "
                          << probe_err << "\n";
                return 1;
        }
    }

    // (1) Load the current key.
    auto old_key_p = keys.openCurrent();
    if (!old_key_p) return 1;
    MasterKey& old_key = *old_key_p;

    // (2) Open the DB. AuthDb::open() creates the file if it is missing - fine,
    // an empty database is valid (no blobs to re-encrypt).
    AuthDb db(auth_db_path);
    if (!db.open()) {
        std::cerr << "ERROR: cannot open auth.db at " << auth_db_path << "\n";
        return 1;
    }

    // (3) Collect EVERY secret encrypted with the master key and decrypt it
    // with the old key BEFORE changing anything. If even one cannot be
    // decrypted we refuse: the rotation would make it unreadable. A new kind
    // of secret is added here with one entry (and in MasterKeyReset.cpp with
    // one more).
    std::vector<detail::RotationEntry> entries;
    const auto refuse = [](const std::string& label) {
        std::cerr << "ERROR: existing " << label << " blob cannot be decrypted "
                     "with current master key — refusing to rotate (the secret "
                     "would become unreadable). Возможно, ключ уже был "
                     "ротирован, а DB — нет.\n";
        return 1;
    };

    // jwt_secret.
    if (auto blob = db.readJwtSecret(); blob.has_value()) {
        auto plain = old_key.decrypt(*blob);
        if (!plain) return refuse("jwt_secret");
        detail::RotationEntry e;
        e.label     = "jwt_secret";
        e.old_blob  = *blob;
        e.plaintext = std::move(*plain);
        e.store = [&db](const std::vector<std::uint8_t>& b) {
            const std::int64_t now =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            return db.storeJwtSecret(b, now);
        };
        entries.push_back(std::move(e));
    }

    // LDAP bind password. Read-modify-write of the singleton row so every
    // other column (updated_at/updated_by included) stays exactly as it was.
    if (auto row = db.readLdapConfigRow();
        row && !row->bind_password_encrypted.empty()) {
        auto plain = old_key.decrypt(row->bind_password_encrypted);
        if (!plain) return refuse("ldap_config.bind_password");
        detail::RotationEntry e;
        e.label     = "ldap_config.bind_password";
        e.old_blob  = row->bind_password_encrypted;
        e.plaintext = std::move(*plain);
        e.store = [&db](const std::vector<std::uint8_t>& b) {
            auto r = db.readLdapConfigRow();
            if (!r) return false;
            r->bind_password_encrypted = b;
            return db.writeLdapConfigRow(*r);
        };
        entries.push_back(std::move(e));
    }

    // SMTP password.
    if (auto row = db.readSmtpConfigRow();
        row && !row->password_encrypted.empty()) {
        auto plain = old_key.decrypt(row->password_encrypted);
        if (!plain) return refuse("smtp_config.password");
        detail::RotationEntry e;
        e.label     = "smtp_config.password";
        e.old_blob  = row->password_encrypted;
        e.plaintext = std::move(*plain);
        e.store = [&db](const std::vector<std::uint8_t>& b) {
            auto r = db.readSmtpConfigRow();
            if (!r) return false;
            r->password_encrypted = b;
            return db.writeSmtpConfigRow(*r);
        };
        entries.push_back(std::move(e));
    }

    // CIFS passwords of mounts (separate mounts.db). Only if the file is
    // already there: rotation must never create an empty database.
    std::unique_ptr<liveqx::mounts::MountsDb> mounts;
    if (!mounts_db_path.empty() && fs::exists(mounts_db_path)) {
        mounts = std::make_unique<liveqx::mounts::MountsDb>(mounts_db_path);
        if (!mounts->open()) {
            std::cerr << "ERROR: cannot open mounts.db at " << mounts_db_path
                      << "\n";
            return 1;
        }
        for (const auto& row : mounts->listAll()) {
            if (row.cifs_password_blob.empty()) continue;
            const std::string label =
                "mounts.cifs_password#" + std::to_string(row.id);
            auto plain = old_key.decrypt(row.cifs_password_blob);
            if (!plain) return refuse(label);
            detail::RotationEntry e;
            e.label     = label;
            e.old_blob  = row.cifs_password_blob;
            e.plaintext = std::move(*plain);
            const std::int64_t id = row.id;
            auto* mdb = mounts.get();
            e.store = [mdb, id](const std::vector<std::uint8_t>& b) {
                return mdb->updateCifsPasswordBlob(id, b);
            };
            entries.push_back(std::move(e));
        }
    }

    // (4) Generate the new key and stage it next to the current one.
    FreshKey fresh{};
    randombytes_buf(fresh.data(), fresh.size());
    const bool staged = keys.stage(fresh);
    sodium_memzero(fresh.data(), fresh.size());
    if (!staged) return 1;

    // (5) Read the staged key back.
    auto new_key_p = keys.openStaged();
    if (!new_key_p) {
        keys.discardStaged();
        return 1;
    }
    MasterKey& new_key = *new_key_p;

    // (6) Re-encrypt every secret with the new key and store it. If a write
    // fails half way, reencryptAll puts back what was already written, so the
    // DB is never left half re-encrypted.
    // IMPORTANT: between this step and the commit the DB points at the new
    // key while the old one is still in effect; a running server would fail
    // to decrypt in that window. So the rotation is an out-of-band procedure
    // with the service stopped (like printBootstrap / resetAdmin).
    // The device CA key is re-sealed first: resealAll decrypts everything with
    // the old key before writing and changes nothing if one fails, and the DB
    // has not been touched yet at that point.
    std::vector<std::string> rotated;
    std::string rotate_err;
    const bool has_device_ca = !device_ca_dir.empty() && fs::exists(device_ca_dir);
    if (has_device_ca &&
        !liveqx::tls::DeviceCa::resealAll(device_ca_dir, old_key, new_key, rotate_err)) {
        std::cerr << "ERROR: " << rotate_err << " — refusing to rotate\n";
        keys.discardStaged();
        return 1;
    }
    if (!detail::reencryptAll(entries, new_key, rotate_err, &rotated)) {
        std::cerr << "ERROR: " << rotate_err << "\n";
        if (has_device_ca) {
            std::string back_err;
            if (!liveqx::tls::DeviceCa::resealAll(device_ca_dir, new_key, old_key, back_err))
                std::cerr << "ERROR: device CA could not be put back: " << back_err << "\n";
        }
        keys.discardStaged();
        return 1;
    }
    if (has_device_ca) rotated.push_back("device_ca");

    // (7) Back the old key up and put the new one in its place. If this fails
    // half way, the messages say where the backup and the new key are so the
    // operator can finish by hand.
    std::string backup;
    if (!keys.commit(backup)) return 1;

    std::cout << "master_key_rotated: true\n";
    std::cout << "key_file: " << keys.where() << "\n";
    std::cout << "backup_file: " << backup << "\n";
    std::cout << "blobs_re_encrypted: ";
    if (rotated.empty()) {
        std::cout << "(none)";
    } else {
        for (std::size_t i = 0; i < rotated.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << rotated[i];
        }
    }
    std::cout << "\n";
    std::cout.flush();
    return 0;
}

}  // namespace

int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path,
                    const std::filesystem::path& mounts_db_path,
                    const std::filesystem::path& device_ca_dir,
                    const std::filesystem::path& state_dir,
                    const std::string& key_store,
                    const std::function<std::optional<std::string>(std::string_view)>& passphrase,
                    const keywrap::KdfParams& kdf) {
    if (key_store != "file") {
        std::cerr << "ERROR: master_key_store is \"" << key_store
                  << "\": the key is delivered from outside (systemd credential "
                     "or environment) and cannot be rotated as a file. Create the "
                     "new key in its store yourself and re-encrypt the secrets.\n";
        return 1;
    }
    FileKeyStore keys(key_file_path, passphrase, kdf);
    return rotateImpl(keys, auth_db_path, mounts_db_path, device_ca_dir, state_dir);
}

int rotateMasterKeyCredential(const std::filesystem::path& auth_db_path,
                              const std::filesystem::path& credential_path,
                              const std::string& credential_name,
                              util::CredentialStore& credentials,
                              const std::filesystem::path& mounts_db_path,
                              const std::filesystem::path& device_ca_dir,
                              const std::filesystem::path& state_dir) {
    CredentialKeyStore keys(credential_path, credential_name, credentials);
    return rotateImpl(keys, auth_db_path, mounts_db_path, device_ca_dir, state_dir);
}

}  // namespace liveqx::auth
