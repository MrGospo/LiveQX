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
#include "utils/DeviceCa.h"
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

int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path,
                    const std::filesystem::path& mounts_db_path,
                    const std::filesystem::path& device_ca_dir,
                    const std::filesystem::path& state_dir,
                    const std::string& key_store) {
    namespace fs = std::filesystem;

    if (key_store != "file") {
        std::cerr << "ERROR: master_key_store is \"" << key_store
                  << "\": the key is delivered from outside (systemd credential "
                     "or environment) and cannot be rotated here. Create the new "
                     "key in its store yourself and re-encrypt the secrets.\n";
        return 1;
    }

    // Rotating under a running service breaks it: for a moment the DB
    // points at the new key while the process holds the old one.
    if (!state_dir.empty()) {
        auto lock = liveqx::util::RuntimeLock::tryAcquire(state_dir / "liveqx.lock");
        if (lock.held()) {
            lock.release();
        } else if (lock.error().find("another instance") != std::string::npos) {
            std::cerr << "ERROR: LiveQX is running. Stop the service before "
                         "rotating the master key.\n";
            return 1;
        } else {
            std::cerr << "ERROR: cannot check whether LiveQX is running: "
                      << lock.error() << "\n";
            return 1;
        }
    }

    // (1) Загружаем старый ключ. Если файла нет — отказ: ротация
    // означает «переехать со старого на новый», и пустого источника
    // быть не должно. Operator должен сначала запустить server один раз
    // (auto-gen) или подложить ключ руками.
    if (!fs::exists(key_file_path)) {
        std::cerr << "ERROR: master key file not found at "
                  << key_file_path << "\n";
        return 1;
    }
    MasterKey old_key(key_file_path.string());
    if (!old_key.load() || !old_key.loaded()) {
        std::cerr << "ERROR: cannot load current master key from "
                  << key_file_path << "\n";
        return 1;
    }

    // The new key must land where the service reads it. Environment
    // variable and systemd credential are delivered from outside.
    switch (old_key.source()) {
        case MasterKey::Source::Env:
            std::cerr << "ERROR: the key comes from LIVEQX_MASTER_KEY; it cannot "
                         "be rotated here. Change the variable yourself and "
                         "re-encrypt the secrets.\n";
            return 1;
        case MasterKey::Source::EnvFile: {
            if (old_key.getInfo().protection == "systemd_credential") {
                std::cerr << "ERROR: the key is a systemd credential; it cannot "
                             "be rotated here. Re-create the credential with "
                             "systemd-creds and re-encrypt the secrets.\n";
                return 1;
            }
            std::error_code eq_ec;
            if (!fs::equivalent(old_key.envFilePath(), key_file_path, eq_ec)) {
                std::cerr << "ERROR: the service reads its key from "
                          << old_key.envFilePath() << " (LIVEQX_MASTER_KEY_FILE), "
                          << "not from " << key_file_path << ".\n";
                return 1;
            }
            break;
        }
        default:
            break;
    }

    // (2) Открываем DB. AuthDb::open() создаст файл, если его нет —
    // это ок, пустая БД — допустимый случай (нет блобов для re-encrypt'а).
    AuthDb db(auth_db_path);
    if (!db.open()) {
        std::cerr << "ERROR: cannot open auth.db at " << auth_db_path << "\n";
        return 1;
    }

    // (3) Собираем ВСЕ секреты, зашифрованные мастер-ключом, и
    // расшифровываем их старым ключом ДО любых изменений. Если хоть один
    // не расшифровывается, отказываем: ротация сделала бы его нечитаемым.
    // Новый вид секрета добавляется сюда одной записью (и в
    // MasterKeyReset.cpp — тоже одной).
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

    // (4) Генерируем новый ключ, пишем во временный файл рядом со старым.
    // O_EXCL чтобы не споткнуться на остатках от предыдущей попытки.
    const fs::path tmp_path = key_file_path.string() + ".new";
    {
        std::error_code ec;
        fs::remove(tmp_path, ec);  // на случай чужого недоведённого rotate
    }
    std::array<std::uint8_t, MasterKey::kKeyBytes> fresh{};
    randombytes_buf(fresh.data(), fresh.size());

    int fd = ::open(tmp_path.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        std::cerr << "ERROR: cannot create " << tmp_path
                  << ": " << std::strerror(errno) << "\n";
        sodium_memzero(fresh.data(), fresh.size());
        return 1;
    }
    const auto written = ::write(fd, fresh.data(), fresh.size());
    if (written != static_cast<ssize_t>(MasterKey::kKeyBytes)) {
        std::cerr << "ERROR: short write to " << tmp_path << "\n";
        ::close(fd);
        std::error_code ec;
        fs::remove(tmp_path, ec);
        sodium_memzero(fresh.data(), fresh.size());
        return 1;
    }
    if (::fsync(fd) != 0) {
        LOG_WARN("rotateMasterKey: fsync failed on {}: {}",
                 tmp_path.string(), std::strerror(errno));
    }
    ::close(fd);
    ::chmod(tmp_path.c_str(), 0600);
    sodium_memzero(fresh.data(), fresh.size());

    // (5) Загружаем новый ключ через MasterKey API (re-read file) — это
    // даёт нам тот же encrypt/decrypt-pipeline и проверяет, что файл
    // читается корректно (страховка от silent corruption).
    MasterKey new_key(tmp_path.string());
    if (!new_key.load() || !new_key.loaded()) {
        std::cerr << "ERROR: cannot reload freshly-written master key at "
                  << tmp_path << "\n";
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return 1;
    }

    // (6) Перешифровываем все секреты новым ключом и записываем. При сбое
    // посреди записи reencryptAll возвращает уже записанное к старому
    // шифртексту, поэтому БД не остаётся наполовину перешифрованной.
    // ВАЖНО: между этим шагом и rename'ом БД указывает на новый ключ, а
    // файл — на старый. Параллельный server в этом окне сломается на
    // decrypt'е. Поэтому rotation — out-of-band procedure при stop'нутом
    // сервере (как printBootstrap / resetAdmin).
    // Ключ CA центра устройств перепечатывается первым: resealAll сначала
    // расшифровывает всё старым ключом и при отказе ничего не меняет, а БД
    // к этому моменту ещё не тронута.
    std::vector<std::string> rotated;
    std::string rotate_err;
    const bool has_device_ca = !device_ca_dir.empty() && fs::exists(device_ca_dir);
    if (has_device_ca &&
        !liveqx::tls::DeviceCa::resealAll(device_ca_dir, old_key, new_key, rotate_err)) {
        std::cerr << "ERROR: " << rotate_err << " — refusing to rotate\n";
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return 1;
    }
    if (!detail::reencryptAll(entries, new_key, rotate_err, &rotated)) {
        std::cerr << "ERROR: " << rotate_err << "\n";
        if (has_device_ca) {
            std::string back_err;
            if (!liveqx::tls::DeviceCa::resealAll(device_ca_dir, new_key, old_key, back_err))
                std::cerr << "ERROR: device CA could not be put back: " << back_err << "\n";
        }
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return 1;
    }
    if (has_device_ca) rotated.push_back("device_ca");

    // (7) Backup'им старый ключ и перемещаем `.new` на место. Если этот
    // блок упадёт между rename'ом backup'а и rename'ом `.new`, то file
    // отсутствует — operator увидит файл `.bak.<ts>` и `.new` рядом, может
    // вручную восстановить.
    const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path bak_path =
        key_file_path.string() + ".bak." + std::to_string(unix_ns);

    std::error_code ec;
    fs::rename(key_file_path, bak_path, ec);
    if (ec) {
        std::cerr << "ERROR: cannot backup current key to " << bak_path
                  << ": " << ec.message() << "\n";
        // НЕ удаляем `.new` — он содержит ключ, которым уже зашифрованы
        // секреты в БД. Operator должен вручную перенести `.new`
        // на место (mv key.new key) либо откатить DB.
        std::cerr << "NOTE: new key file is at " << tmp_path
                  << ". Секреты в БД уже зашифрованы новым ключом — "
                     "переместите файл вручную: mv " << tmp_path << " "
                  << key_file_path << "\n";
        return 1;
    }
    fs::rename(tmp_path, key_file_path, ec);
    if (ec) {
        std::cerr << "ERROR: cannot move new key into place: "
                  << ec.message() << "\n";
        std::cerr << "NOTE: backup is at " << bak_path
                  << ", new key at " << tmp_path
                  << ". DB blobs зашифрованы новым ключом — НЕ восстанавливайте "
                     "backup без ручного re-decrypt'а.\n";
        return 1;
    }

    std::cout << "master_key_rotated: true\n";
    std::cout << "key_file: " << key_file_path << "\n";
    std::cout << "backup_file: " << bak_path << "\n";
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

}  // namespace liveqx::auth
