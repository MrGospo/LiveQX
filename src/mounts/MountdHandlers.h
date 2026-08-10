#pragma once
//
// fix41 — оркестратор RPC-операций mountd'а.
//
// MountdHandlers соединяет:
//   • UnitGenerator   — pure-функции, тексты юнитов
//   • ICredsHelper    — шифрование CIFS-креда (systemd-creds)
//   • ISystemctl      — daemon-reload / enable --now / show
//   • IMountTester    — dry-run mount(8)
// — и подаёт это RpcServer'у в виде std::function<RpcResponse(op, body)>.
//
// Все DI через интерфейсы — handler-набор полностью тестируется без
// systemd / mount / systemd-creds. Реальные импл-ы (SystemctlClient,
// SystemdCredsHelper, MountTester) подгружаются в `MountdMain`.
//
// Файловая запись:
//   • .mount / .automount → ctx.systemd_unit_dir/<basename>.{mount,automount}
//     (default /etc/systemd/system, mode 0644, root:root)
//   • plaintext creds → ctx.cred_dir/<credFilename(id)>.cred (0600 root:root)
//     fix43: tmpfs (/run/liveqx/creds) — не уходит на диск, виден
//     только root'у через права parent-каталога. См. CredsHelper.h.
// Запись атомарна через write+rename; на удалении — unlink.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "mounts/CredsHelper.h"
#include "mounts/MountSpec.h"
#include "mounts/MountTester.h"
#include "mounts/RpcProtocol.h"
#include "mounts/RpcServer.h"
#include "mounts/SystemctlClient.h"
#include "mounts/UnitGenerator.h"

namespace liveqx::mounts {

// Конфиг handler'ов. Хранит точки записи юнитов / кредов.
struct MountdHandlersConfig {
    UnitGenContext        unit_ctx;          // unit_dir + cred_dir + automount cfg

    // fix42: managed_prefix дропнут. Раньше использовался для list-by-prefix
    // и defensive-фильтра в RemoveMount, но после перехода на
    // systemd-escape-derived basename'ы общий префикс отсутствует. Доверие
    // к target/id обеспечивает peer-auth AF_UNIX-сокета (см. RpcServer).
};

class MountdHandlers {
public:
    MountdHandlers(MountdHandlersConfig         cfg,
                   std::shared_ptr<ISystemctl>  systemctl,
                   std::shared_ptr<ICredsHelper> creds,
                   std::shared_ptr<IMountTester> tester);

    // Удобный шорткат — отдаёт callable, готовый к вкладке в RpcServer.
    RpcHandler asRpcHandler();

    // Низкоуровневые методы — выставлены, чтобы тесты могли дёргать
    // напрямую без JSON-сериализации.
    RpcResponse handleApply (const MountSpec& spec);

    // fix42: для удаления нужен target — из него выводится basename юнита
    // (см. unitBasenameByTarget). id оставляем для имени cred-файла (он
    // id-based, см. credFilename) и для миграции легаси-юнитов.
    RpcResponse handleRemove(std::int64_t id, std::string_view target);

    RpcResponse handleTest  (const MountSpec& spec);

    // fix42: Status получает список {id, target} от клиента, который
    // знает все смонтированные ресурсы из mounts.db. Для каждой записи
    // helper опрашивает фактический ActiveState/LoadState/SubState через
    // systemctl show <unitBasenameByTarget(target)>.mount.
    struct StatusItem {
        std::int64_t id;
        std::string  target;
    };
    RpcResponse handleStatus(const std::vector<StatusItem>& items);

private:
    // Атомарная запись текста: пишем во временный <path>.tmp, rename.
    bool writeUnitFile(const std::filesystem::path& path,
                       const std::string&           content,
                       std::string&                 err);

    // Удаляет файл если есть; не ошибка, если уже отсутствует.
    bool removeIfExists(const std::filesystem::path& path,
                        std::string&                 err);

    MountdHandlersConfig          cfg_;
    std::shared_ptr<ISystemctl>   systemctl_;
    std::shared_ptr<ICredsHelper> creds_;
    std::shared_ptr<IMountTester> tester_;
};

}  // namespace liveqx::mounts
