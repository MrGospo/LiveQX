#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/KeyWrap.h"

namespace liveqx::auth {

class MasterKey;
}  // namespace liveqx::auth
namespace liveqx::util { class CredentialStore; }
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
//   rotateMasterKey (auth_db_path, key_file_path, ...):
//     - Loads the current MasterKey from key_file_path.
//     - Reads EVERY blob encrypted with the master key: jwt_secret, the LDAP
//       bind password, the SMTP password (auth.db) and the CIFS passwords of
//       mounts (mounts.db, if the file exists).
//     - Decrypts each with the old key BEFORE changing anything: if even one
//       blob cannot be decrypted, the rotation refuses and changes nothing.
//       Then it generates a new key and re-encrypts the blobs.
//     - Writes the new key to `<key>.new` (mode and owner of the old file),
//       stores the new ciphertexts (a failure half way puts back those
//       already written), backs the old key up as `<key>.bak.<unix_ns>` and
//       atomically moves `.new` into place.
//     - Must run with the service stopped: between the UPSERTs and the rename
//       the DB blobs are encrypted with the new key while the file still holds
//       the old one, so a running server would fail to decrypt.
//
// Return value: the process exit code (0 = success, non-zero = error).

int printBootstrap(const std::filesystem::path& auth_db_path);

int resetAdmin(const std::filesystem::path& auth_db_path,
               std::string_view username);

// `mounts_db_path`: mounts.db with the CIFS passwords; empty or missing file
//   means the mounts are skipped (the database is not created).
// `device_ca_dir`: directory of the device CA (its key is sealed with the
//   master key); empty or missing means it is skipped (not created).
// `state_dir`: when set, the rotation refuses while the service is running.
// `key_store`: master_key_store from the config; anything but "file" is
//   refused (the key is delivered from outside and is not rewritten here).
// `passphrase`: source of the passphrase for a passphrase-wrapped key
//   (LQXKEY1). It is needed to open the old key; the new data key is wrapped
//   under the same passphrase with the parameters `kdf`. Without a source such
//   a key is not rotated.
int rotateMasterKey(const std::filesystem::path& auth_db_path,
                    const std::filesystem::path& key_file_path,
                    const std::filesystem::path& mounts_db_path = {},
                    const std::filesystem::path& device_ca_dir = {},
                    const std::filesystem::path& state_dir = {},
                    const std::string& key_store = "file",
                    const std::function<std::optional<std::string>(std::string_view)>&
                        passphrase = nullptr,
                    const keywrap::KdfParams& kdf = keywrap::KdfParams::moderate());

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

// Rotation of a master key stored as a systemd credential (master_key_store =
// "creds"). Same rules as rotateMasterKey: every secret is checked first, the
// service must be stopped, the old credential is kept as `<file>.bak.<unix_ns>`.
// `credential_name` is the name the credential was encrypted under.
int rotateMasterKeyCredential(const std::filesystem::path& auth_db_path,
                              const std::filesystem::path& credential_path,
                              const std::string& credential_name,
                              liveqx::util::CredentialStore& credentials,
                              const std::filesystem::path& mounts_db_path = {},
                              const std::filesystem::path& device_ca_dir = {},
                              const std::filesystem::path& state_dir = {});

}  // namespace liveqx::auth
