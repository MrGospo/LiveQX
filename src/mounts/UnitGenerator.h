#pragma once
//
// fix41 — генерация systemd .mount + .automount юнитов из MountSpec.
//
// Pure-функции, без I/O — полностью детерминированы и тестируются
// без systemd. Запись на диск делает MountdHandlers (commit 4).
//
// Конвенции:
//   • Имя .mount/.automount-юнита строится из target через
//     systemd-escape(5): strip leading/trailing '/', '/' → '-',
//     все символы вне [A-Za-z0-9_.-] → \xHH (UTF-8 байтами). Это
//     ЖЁСТКОЕ требование systemd: иначе при load'е юнит отклоняется с
//     "Where= setting doesn't match unit name. Refusing.".
//     Пример: target=/mnt/liveqx/lib1 → юнит
//     "mnt-liveqx-lib1.mount".
//     fix42 — раньше имя было id-based ("liveqx-mnt-<id>"), что
//     никогда не проходило валидацию systemd; bug словили только при
//     первой попытке монтирования в проде.
//   • cred-файл живёт по id-based-имени ("liveqx-mnt-<id>.cred")
//     в /run/liveqx/creds/ (tmpfs, mode 0700 root:root). Имя
//     systemd не валидирует, ASCII-id хватает.
//   • fix43 — CIFS-креды передаются через `Options=credentials=<abs path>`
//     (mount.cifs читает файл напрямую). `LoadCredentialEncrypted=`
//     умышленно НЕ используется: systemd 255 принимает директиву в [Mount]
//     без ошибки, но никогда не создаёт `/run/credentials/<unit>/` — что
//     приводит к `mount error(2)` при попытке монтирования.
//     Плейн-текст на tmpfs не попадает в бэкапы (нет на диске) и виден
//     только root'у через права parent-каталога.

#include <cstdint>
#include <filesystem>
#include <string>

#include "mounts/MountSpec.h"

namespace liveqx::mounts {

struct GeneratedUnits {
    // Полный текст <unit_basename>.mount (имя выводится из target через
    // systemd-escape — см. unitBasenameByTarget).
    std::string mount_unit;

    // Полный текст <unit_basename>.automount.
    std::string automount_unit;

    // Полный текст credentials-файла в формате, который mount.cifs
    // читает по `credentials=` опции: username=...\npassword=...\ndomain=...
    // Пуст если creds не нужны (NFS / CIFS guest без password).
    std::string cred_payload;

    // Имя бэйз-юнитов (без суффикса) для записи .mount/.automount файлов
    // и для вызовов systemctl. Строится через unitBasenameByTarget(target).
    // Пример: "mnt-liveqx-lib1".
    std::string unit_basename;

    // Полный путь к cred-файлу под tmpfs (см. credFilename + ctx.cred_dir).
    // Пуст если cred_payload пуст. Этот же путь подставляется в
    // `Options=credentials=<cred_path>` для mount.cifs.
    std::filesystem::path cred_path;

    bool needs_credentials() const noexcept { return !cred_payload.empty(); }
};

// Опции генерации. systemd_unit_dir и cred_dir определяют, КУДА
// MountdHandlers потом будет писать сгенерированный текст. Сами
// функции работают только со строками.
//
// fix43: cred_dir по умолчанию — tmpfs /run/liveqx/creds (0700
// root:root). Это не /etc/credstore.encrypted/ потому что фикс уходит
// от systemd-creds к plaintext (см. CredsHelper.h за бэкстори).
struct UnitGenContext {
    std::filesystem::path systemd_unit_dir = "/etc/systemd/system";
    std::filesystem::path cred_dir         = "/run/liveqx/creds";

    // Если true — добавляем `noauto,x-systemd.automount` в Options
    // и генерим .automount юнит. По умолчанию ОН: для библиотеки
    // контента lazy-mount предпочтителен.
    bool generate_automount = true;

    // TimeoutIdleSec для .automount. После N секунд бездействия
    // systemd размонтирует. По умолчанию 600 (10 мин). 0 — отключает
    // размонтирование.
    int automount_idle_seconds = 600;

    // fix44: владелец и mode для CIFS-mount'а. mount.cifs по умолчанию
    // мапит все файлы на uid=0/gid=0 с file_mode/dir_mode=0755 (но фактическая
    // видимость зависит от opaque permissions сервера). В нашей конфигурации
    // liveqx.service работает под пользователем liveqx (uid≠0),
    // и Watcher этого пользователя НЕ видит файлы без явного `uid=`,`gid=`,
    // `file_mode=`,`dir_mode=` в Options=.
    //
    // При cifs_uid != 0 генератор добавит соответствующие токены в Options=
    // для fs_type=cifs, НО только если оператор сам не задал их в
    // MountSpec.options. NFS не трогаем — там uid mapping серверный.
    //
    // mountd резолвит cifs_uid/cifs_gid через getpwnam("liveqx") при
    // старте; если такого пользователя на хосте нет — оставляет нули и
    // инжекта не происходит (поведение pre-fix44).
    std::uint32_t cifs_uid = 0;
    std::uint32_t cifs_gid = 0;
    std::string   cifs_file_mode = "0644";
    std::string   cifs_dir_mode  = "0755";
};

// Производит юниты + cred-payload. spec предполагается прошедшим
// `validate()`. Возвращаемая структура ВСЕГДА заполнена; вызывающий
// решает, какие файлы реально писать.
GeneratedUnits generateUnits(const MountSpec& spec, const UnitGenContext& ctx);

// Канонизация target'а — то значение, что попадает в Where= юнитов.
// Гарантирует ведущий '/', снимает trailing '/'. "/" остаётся "/" (но
// валидатор MountSpec такой target отклонит). systemdEscapePath над
// этим же входом даёт совпадающий с Where= basename — это и есть
// контракт, на котором systemd валидирует .mount/.automount юниты.
std::string normalizedTargetPath(std::string_view target);

// systemd-escape(5) для путей: ведущий '/' срезаем, оставшиеся '/' →
// '-', все символы вне [A-Za-z0-9_.-] кодируются как \xHH UTF-8.
// Trailing '/' срезаем перед обработкой ("/mnt/x/" и "/mnt/x" дают
// один и тот же результат). Пустой или "/" → "-". Ведущая '.' →
// "\x2e" (systemd-конвенция). Pure, тестируется без systemd.
std::string systemdEscapePath(std::string_view target);

// Имя бэйз-юнита для target. Использует systemdEscapePath; результат
// matches тому, что systemd требует для .mount-файла, чей Where=
// — этот же target (после strip-trailing-slash).
std::string unitBasenameByTarget(std::string_view target);

// Имя cred-файла (без расширения) для id. id-based — стабильный
// ASCII-ключ для каталога cred_dir.
std::string credFilename(std::int64_t id);

// Старое id-based имя юнита. Сохранено для миграции — handleApply
// удаляет файлы legacy-имени, если они остались от pre-fix42 версии
// helper'а. Новый код для генерации юнитов использует
// unitBasenameByTarget.
std::string legacyUnitBasename(std::int64_t id);

// Готовый набор Options=, который попадёт в .mount юнит. Объединяет
// пользовательские options + ro-флаг + credentials=<abs path> для CIFS +
// (fix44) uid/gid/file_mode/dir_mode для CIFS, если ctx.cifs_uid задан.
// fix43: cred_path передаётся явно — это абсолютный путь к plaintext
// cred-файлу, mount.cifs прочитает его напрямую. Пустой cred_path при
// with_credentials=true даст пустой credentials= — вызывающий обязан
// передать валидный путь, если creds нужны.
std::string buildOptionsLine(const MountSpec&             spec,
                             bool                         with_credentials,
                             const std::filesystem::path& cred_path,
                             const UnitGenContext&        ctx = {});

}  // namespace liveqx::mounts
