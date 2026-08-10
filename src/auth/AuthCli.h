#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace liveqx::auth {

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
//     - Читает все encrypted-blob'ы (commit 15/24 — пока только jwt_secret;
//       ldap.bind_password / smtp.password добавятся в commits 17 и 23).
//     - Расшифровывает каждый старым ключом, генерирует новый ключ,
//       перешифровывает блобы новым ключом.
//     - Пишет новый ключ во временный файл `<key>.new` mode 0600,
//       UPSERT'ит новые ciphertext'ы в БД, бэкапит старый ключ как
//       `<key>.bak.<unix_ns>` и атомарно перемещает `.new` на место.
//     - Должно запускаться при остановленном сервере: между UPSERT'ом
//       и rename'ом DB-блобы зашифрованы новым ключом, а файл всё ещё
//       старый — параллельный server failed бы decrypt.
//
// Возвращаемое значение — process exit code (0 success, !=0 error).

int printBootstrap(const std::filesystem::path& auth_db_path);

int resetAdmin(const std::filesystem::path& auth_db_path,
               std::string_view username);

int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path);

}  // namespace liveqx::auth
