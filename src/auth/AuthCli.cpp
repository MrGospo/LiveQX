// fix22 commit 10/24 — recovery-CLI helpers (см. AuthCli.h).
//
// Не использует JwtIssuer (т.е. не требует jwt_secret в конфиге) —
// CLI не выпускает токены. AuthService тут нужен только для
// bootstrapInitialAdmin / adminResetPassword, оба из них работают
// без grants_resolver (мы передаём пустой callback). Это сделано
// сознательно: recovery-CLI должен запускаться даже когда RBAC-конфиг
// сломан или auth.db только что разрушен.

#include "auth/AuthCli.h"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/MasterKey.h"

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

int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path) {
    namespace fs = std::filesystem;

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

    // (2) Открываем DB. AuthDb::open() создаст файл, если его нет —
    // это ок, пустая БД — допустимый случай (нет блобов для re-encrypt'а).
    AuthDb db(auth_db_path);
    if (!db.open()) {
        std::cerr << "ERROR: cannot open auth.db at " << auth_db_path << "\n";
        return 1;
    }

    // (3) Расшифровываем все известные blob'ы старым ключом. Сейчас
    // таких блобов один — jwt_secret. ldap.bind_password / smtp.password
    // добавятся когда соответствующие фичи появятся (commits 17, 23).
    std::optional<std::string> jwt_plain;
    if (auto blob = db.readJwtSecret(); blob.has_value()) {
        jwt_plain = old_key.decrypt(*blob);
        if (!jwt_plain) {
            std::cerr << "ERROR: existing jwt_secret blob cannot be decrypted "
                         "with current master key — refusing to rotate (DB "
                         "would become unreadable). Возможно, ключ уже был "
                         "ротирован, а DB — нет.\n";
            return 1;
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

    // (6) Перешифровываем все blob'ы новым ключом и UPSERT'им в БД.
    // ВАЖНО: между этим шагом и rename'ом DB указывает на новый ключ,
    // а файл — на старый. Параллельный server в этом окне сломается на
    // decrypt'е. Поэтому rotation — out-of-band procedure при stop'нутом
    // сервере (как printBootstrap / resetAdmin).
    if (jwt_plain) {
        auto new_blob = new_key.encrypt(*jwt_plain);
        if (new_blob.empty()) {
            std::cerr << "ERROR: re-encrypt of jwt_secret failed\n";
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return 1;
        }
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!db.storeJwtSecret(new_blob, now)) {
            std::cerr << "ERROR: cannot UPSERT jwt_secret with new ciphertext\n";
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return 1;
        }
    }

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
        // НЕ удаляем `.new` — он содержит ключ, которым уже зашифрован
        // jwt_secret в БД. Operator должен вручную перенести `.new`
        // на место (mv key.new key) либо откатить DB.
        std::cerr << "NOTE: new key file is at " << tmp_path
                  << ". DB jwt_secret уже зашифрован новым ключом — "
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
    std::cout << "blobs_re_encrypted: "
              << (jwt_plain ? "jwt_secret" : "(none)") << "\n";
    std::cout.flush();
    return 0;
}

}  // namespace liveqx::auth
