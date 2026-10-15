#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace liveqx::auth {

class MasterKey;

// fix22 commit 10/24 — recovery-CLI поверх AuthService.
//
// Эти команды нужны оператору для out-of-band-доступа: server упал
// до того, как показал bootstrap-пароль; админ забыл пароль и нет
// второго админа, чтобы reset через REST. Все они работают на закрытом
// сервере (process holds DB exclusively через open()), а не во время
// штатной работы.
//
// Контракт:
//   printBootstrap (auth_db_path):
//     - Открывает auth.db (создаёт, если нет).
//     - Если админа нет — bootstrap'ит "admin" и печатает пароль на stdout.
//     - Если админ уже есть — пишет на stderr "already exists" и выходит 0.
//   resetAdmin (auth_db_path, username):
//     - Находит юзера. Если нет — exit 1.
//     - Если юзер не role=admin — exit 1 (защита от случайной смены
//       пароля viewer'у через CLI).
//     - Reset password + must_change=true + revokeAllSessions.
//     - Печатает новый plaintext на stdout.
//   rotateMasterKey (auth_db_path, key_file_path):
//     - Загружает текущий MasterKey из key_file_path.
//     - Читает ВСЕ encrypted-blob'ы, зашифрованные мастер-ключом:
//       jwt_secret, пароль привязки LDAP, пароль SMTP (auth.db) и CIFS-пароли
//       монтирований (mounts.db, если файл существует).
//     - Расшифровывает каждый старым ключом ДО любых изменений: если хоть
//       один блоб не расшифровывается, ротация отказывается и ничего не
//       меняет. Затем генерирует новый ключ и перешифровывает блобы.
//     - Пишет новый ключ во временный файл `<key>.new` mode 0600,
//       записывает новые ciphertext'ы (при сбое посреди записи уже
//       записанные возвращаются к прежним), бэкапит старый ключ как
//       `<key>.bak.<unix_ns>` и атомарно перемещает `.new` на место.
//     - Должно запускаться при остановленном сервере: между UPSERT'ом
//       и rename'ом DB-блобы зашифрованы новым ключом, а файл всё ещё
//       старый — параллельный server failed бы decrypt.
//
// Возвращаемое значение — process exit code (0 success, !=0 error).

int printBootstrap(const std::filesystem::path& auth_db_path);

int resetAdmin(const std::filesystem::path& auth_db_path,
               std::string_view username);

// `mounts_db_path` — mounts.db с CIFS-паролями; пусто или файла нет —
// монтирования пропускаются (БД при этом не создаётся).
int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path,
                    const std::filesystem::path& mounts_db_path = {});

namespace detail {

// One secret protected by the master key, as seen by rotation.
struct RotationEntry {
    std::string                label;      // e.g. "ldap_config.bind_password"
    std::vector<std::uint8_t>  old_blob;   // ciphertext under the CURRENT key
    std::string                plaintext;  // decrypted with the current key
    // Persists `blob` for this secret; false on failure.
    std::function<bool(const std::vector<std::uint8_t>&)> store;
};

// Re-encrypts every entry with `new_key` and stores it. Stops at the first
// failure and puts the entries already stored back to their old_blob, so
// the storage is never left half-rotated. On success `done_labels` (if
// given) lists every entry in order; after a rollback it is empty.
bool reencryptAll(const std::vector<RotationEntry>& entries,
                  const MasterKey&                  new_key,
                  std::string&                      err,
                  std::vector<std::string>*         done_labels = nullptr);

}  // namespace detail

}  // namespace liveqx::auth
