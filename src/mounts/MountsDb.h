#pragma once
//
// fix41 — state/mounts.db: persisted CIFS/NFS mount catalog.
//
// liveqx владеет «source-of-truth» для mounts (UI пишет сюда,
// reconcile-on-boot читает); mountd просто зеркалит решения через
// systemd .mount-юниты. Поэтому schema живёт здесь, не в helper'е.
//
// Поля password хранятся в `cifs_password_encrypted` BLOB'ом, шифрованным
// тем же master_key, что и jwt_secret/ldap.bind_password (см. fix22).
// Plaintext password не выходит из MountManager: слой выше (REST/UI)
// получает MountSpec, у которого cifs->password обнулён, плюс boolean
// `has_password` через MountSpecPublic. Для edit-flow UI шлёт пустой
// password = «оставить без изменений», непустой = «заменить».
//
// SQLite settings: WAL, BEGIN IMMEDIATE для writes (как AuthDb).
// Все мутации сериализованы под std::mutex (низкочастотный CRUD).

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mounts/MountSpec.h"

struct sqlite3;

namespace liveqx::mounts {

// On-disk row mountов. Шифрованный пароль — BLOB; вытаскивать
// plaintext должна вышестоящая логика через MasterKey.decrypt().
struct MountRow {
    std::int64_t              id          = 0;
    std::string               fs_type;            // "cifs" / "nfs"
    std::string               source;
    std::string               target;
    std::string               options;
    bool                      ro          = true;

    std::string               cifs_username;       // "" если не задан
    std::vector<std::uint8_t> cifs_password_blob;  // пусто → нет пароля
    std::string               cifs_domain;

    bool                      enabled     = true;  // false = строка есть, но
                                                   // helper не материализует
    std::int64_t              created_at  = 0;
    std::int64_t              updated_at  = 0;

    std::string               last_active_state;   // "active"/"inactive"/...
    std::int64_t              last_status_at = 0;

    // True если в строке есть пароль; используется UI для рендера
    // «password set / cleared».
    bool hasPassword() const noexcept { return !cifs_password_blob.empty(); }
};

class MountsDb {
public:
    explicit MountsDb(std::filesystem::path path);
    ~MountsDb();

    MountsDb(const MountsDb&)            = delete;
    MountsDb& operator=(const MountsDb&) = delete;

    bool open();
    bool ok() const noexcept { return db_ != nullptr; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // ── CRUD ─────────────────────────────────────────────────────────
    std::vector<MountRow>          listAll();
    std::optional<MountRow>        findById(std::int64_t id);
    std::optional<MountRow>        findByTarget(std::string_view target);

    // Returns assigned id или nullopt при UNIQUE-violation на target.
    // updated_at/created_at outsides не передавать — мы ставим now().
    // Поле id игнорируется (SQLite присвоит сам).
    std::optional<std::int64_t>    insert(const MountRow& row);

    // Обновляет все «mutable» поля. id не меняется. enabled-флаг и
    // creds (cifs_password_blob) перезаписываются как есть; вызывающий
    // отвечает за «leave password alone if empty» semantics.
    bool                           update(const MountRow& row);

    // Удаляет строку. Возвращает true даже если id уже отсутствовал
    // (DELETE'у похуй сколько rows aff'нуло).
    bool                           deleteById(std::int64_t id);

    // Обновляет only last_active_state + last_status_at. Используется
    // background-pull'ом status'а. Не меняет updated_at (это «состояние»,
    // а не «конфиг»).
    bool                           updateStatus(std::int64_t id,
                                                std::string_view state,
                                                std::int64_t ts);

private:
    bool exec(const char* sql);
    bool runMigrations();

    std::filesystem::path path_;
    sqlite3*              db_{nullptr};
    std::mutex            mu_;
};

}  // namespace liveqx::mounts
