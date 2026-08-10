#pragma once
//
// fix41 — оркестратор mounts на стороне liveqx.
//
// Связывает три зависимости:
//   • MountsDb       — persisted catalogue (state/mounts.db)
//   • MasterKey      — encrypt/decrypt CIFS-password
//   • IMountdRpcClient — переговорщик с привилегированным helper'ом
//
// API задумана под REST + reconcile-on-boot:
//
//   listAll() / getById(id, include_password=false)
//      → строки, у которых password отдаётся ТОЛЬКО при include_password
//        (REST никогда не отдаёт; CLI/доверенный consumer — может).
//
//   addMount(spec)   → INSERT в DB → applyMount в helper.
//   updateMount(id, spec, password_change)
//                    → UPDATE → applyMount.
//   removeMount(id)  → mountd.removeMount → DELETE.
//   testMount(spec)  → mountd.testMount, БД не трогаем.
//
//   syncStatusFromHelper() → status() с mountd, для каждого id
//      найти строку, обновить last_active_state. Низкочастотный pull
//      раз в 30 секунд по background-таймеру (заводится снаружи).
//
//   reconcileOnBoot() → для каждой enabled строки вызвать applyMount.
//      Идемпотентно (mountd просто перезапишет тот же текст и
//      enable --now повторно — это no-op, если уже active).
//
// Все методы НЕ блокируют рендер; вызовы проксируются в mountd, где
// уже стоят свои тайм-ауты. MountManager сам по себе stateless кроме
// зависимостей; thread-safe через std::mutex для рейс'ов между REST
// и фоновым sync'ом.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mounts/MountSpec.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/MountsDb.h"

namespace liveqx::auth { class MasterKey; }

namespace liveqx::mounts {

// Public-shape строки для REST/UI. Ровно как MountRow без password
// blob'а; добавлен has_password флаг и live ActiveState (если sync
// успел отработать до запроса). Статус — строкой («active»/«inactive»/...);
// «activating» означает, что enable --now прошёл, но mount(2) ещё не
// успел.
struct MountPublic {
    std::int64_t id           = 0;
    std::string  fs_type;
    std::string  source;
    std::string  target;
    std::string  options;
    bool         ro           = true;
    bool         enabled      = true;
    bool         has_password = false;
    std::string  cifs_username;
    std::string  cifs_domain;
    std::int64_t created_at   = 0;
    std::int64_t updated_at   = 0;
    std::string  active_state;     // last known
    std::int64_t last_status_at = 0;
};

// Результат любых mutating-операций. helper-error отдельно, чтобы UI
// мог показать «сохранено в БД, но монтирование не удалось».
struct MountOpResult {
    bool        ok               = false;
    std::int64_t id              = 0;
    std::string error;             // human-readable
    std::string error_code;        // "invalid", "duplicate", "rpc", ...
    std::string helper_status;     // ActiveState из helper, если был
    std::string helper_error;      // если helper отказал
};

class MountManager {
public:
    MountManager(MountsDb&                          db,
                 const auth::MasterKey&             master_key,
                 std::shared_ptr<IMountdRpcClient>  rpc);

    // Read.
    std::vector<MountPublic> listAll();
    std::optional<MountPublic> getById(std::int64_t id);

    // Read + decrypt password (для предзаполнения edit-форм оператора
    // под viewing-rights). Не выставлять в REST!
    std::optional<MountSpec> getDecryptedSpec(std::int64_t id);

    // Mutations. spec.id игнорируется при insert.
    MountOpResult addMount   (const MountSpec& spec);

    // password_change semantics:
    //   • spec.cifs.has_value() && password.empty() && existing has password
    //         → keep existing encrypted blob.
    //   • spec.cifs.has_value() && !password.empty()
    //         → re-encrypt, replace.
    //   • !spec.cifs.has_value()
    //         → wipe creds (CIFS → guest, или CIFS → NFS).
    MountOpResult updateMount(std::int64_t id, const MountSpec& spec);

    MountOpResult removeMount(std::int64_t id);
    MountOpResult testMount  (const MountSpec& spec);

    // Background-sync. Тянет status() с helper'а, мержит в DB. Без
    // ошибок — просто apply'ит результаты; ошибки логируются.
    void syncStatusFromHelper();

    // На startup: applyMount для каждой enabled строки. Не блокирует
    // если mountd вне сети — пишет warn в лог и движется дальше.
    void reconcileOnBoot();

    // Для тестов: количество известных строк.
    std::size_t size();

    // fix43: проверка, что новый target не пересекается с уже
    // существующими mount'ами по pathnesting'у. systemd для nested mount
    // path'а автоматически добавляет `Requires=<parent>.mount` и
    // `RequiresMountsFor=<parent>` — что приводит к загадочным
    // «Dependency failed» если parent.mount не существует или сломан.
    // Возвращает строку с описанием конфликта или пустую если OK.
    // ignore_id — id, чей target не считается конфликтом (для update).
    std::string checkOverlap(const std::string& target,
                             std::int64_t       ignore_id);

private:
    // Вспомогательно: row → publix. Ничего не расшифровывает.
    static MountPublic toPublic(const MountRow& r);

    // Берёт MountSpec и кладёт его в MountRow, шифруя пароль если есть.
    // Если existing!=nullptr — реализует password_change-семантику
    // выше.
    bool toRow(const MountSpec& spec,
               const MountRow*  existing,
               MountRow&        out_row,
               std::string&     err);

    // Расшифровывает пароль из row в out (пустая строка если нет).
    bool decryptPassword(const MountRow& row,
                         std::string&    out,
                         std::string&    err) const;

    MountsDb&                         db_;
    const auth::MasterKey&            master_key_;
    std::shared_ptr<IMountdRpcClient> rpc_;
    std::mutex                        mu_;        // serialises mutations
};

}  // namespace liveqx::mounts
