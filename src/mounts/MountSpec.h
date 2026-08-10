#pragma once
//
// fix41 — описание единицы монтирования.
//
// MountSpec — это всё, что нужно `liveqx-mountd`'у, чтобы
// материализовать одну сетевую шару:
//
//   • fs_type   — "cifs" или "nfs"
//   • source    — "//host/share" (CIFS) или "host:/path" (NFS)
//   • target    — абсолютный путь под /mnt/liveqx/<id>
//   • options   — comma-separated mount opts ("vers=3.0,iocharset=utf8")
//   • ro        — read-only (true для библиотеки, false для writable HLS)
//   • CIFS creds (опционально, plaintext до записи в credstore)
//
// Все строки валидируются на запрещённые символы (newline, NUL, точка
// с запятой, обратный апостроф, pipe). Это важно: и `target`, и
// `options` попадают в текстовый systemd-юнит, любая инъекция позволила
// бы вписать произвольные директивы.
//
// JSON-сериализация — для RPC между liveqx и mountd, и для
// REST API (откуда исходит payload оператора). `password` никогда не
// попадает в `toJson()` — он живёт только в RAM mountd'а, а в БД
// liveqx'а хранится зашифрованным под master_key.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace liveqx::mounts {

enum class FsType {
    Cifs,
    Nfs,
};

const char* toString(FsType fs) noexcept;
std::optional<FsType> fsTypeFromString(std::string_view s) noexcept;

// CIFS-специфичные креды. Для NFS sec=sys кред нет — поле остаётся
// std::nullopt. Для NFS sec=krb5 креды (keytab) поедут отдельным
// follow-up'ом, не в fix41.
struct CifsCreds {
    std::string username;        // не пусто, ≤256
    std::string password;        // ≤256, plaintext только в RAM mountd'а
    std::string domain;          // опционально, ≤64
};

struct MountSpec {
    std::int64_t          id      = 0;     // db row id; 0 для draft/test
    FsType                fs_type = FsType::Cifs;
    std::string           source;
    std::string           target;
    std::string           options;          // comma-separated, без 'ro'
    bool                  ro      = true;
    std::optional<CifsCreds> cifs;          // обязательно для CIFS с auth

    // True iff все обязательные поля выглядят корректно. Тяжёлая
    // проверка (filesystem'ы, creds существуют, путь не collision'ит
    // с другими mounts) — за пределами этой функции; здесь только
    // строковая безопасность и форматные требования.
    //
    // out_error: human-readable причина отказа (для REST 400 + audit).
    bool validate(std::string& out_error) const;

    // JSON helpers. fromJson() возвращает nullopt при отсутствующих/
    // невалидных полях; вызывающий должен дёрнуть validate() для
    // уточнённого сообщения.
    nlohmann::json toJson(bool include_password) const;
    static std::optional<MountSpec> fromJson(const nlohmann::json& j,
                                             std::string& out_error);
};

// Базовая директория, под которой mountd соглашается материализовать
// точки монтирования. Любой target вне этого дерева отклоняется.
// Производственный путь — на /mnt; тесты подменяют через --mount-root.
inline constexpr const char* kDefaultMountRoot = "/mnt/liveqx";

// Стандартное имя сокета (root:liveqx 0660). Тесты переопределяют.
inline constexpr const char* kDefaultSocketPath = "/run/liveqx/mountd.sock";

// Имя группы, члены которой допускаются к сокету. Должно совпадать с
// User'ом liveqx.service. install-script создаёт группу при
// деплое.
inline constexpr const char* kClientGroupName = "liveqx";

}  // namespace liveqx::mounts
