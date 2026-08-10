#pragma once
//
// fix41 — обёртка над systemctl(1) для mountd.
//
// ISystemctl — абстракция, чтобы MountdHandlers тестировались
// без реального systemd. SystemctlClient — production-импл через
// Subprocess (fork+execve /usr/bin/systemctl).
//
// Решения:
//   • show с filter --property=ActiveState,LoadState,SubState — это
//     стабильный machine-readable формат systemctl, существует с
//     systemd 230+ и переживает локализацию.
//   • daemon-reload и enable --now изолированы методами, чтобы каждая
//     ошибка отдельно атрибутировалась.
//   • Bin-path резолвится на construct'е: пробуем /usr/bin/systemctl
//     потом /bin/systemctl. Если ничего нет — daemon стартует с явной
//     ошибкой.

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mounts/RpcProtocol.h"

namespace liveqx::mounts {

class ISystemctl {
public:
    virtual ~ISystemctl() = default;
    virtual bool daemonReload(std::string& err) = 0;
    virtual bool enableNow (std::string_view unit, std::string& err) = 0;
    virtual bool disableNow(std::string_view unit, std::string& err) = 0;

    // fix43: явный `systemctl start <unit>` (без enable). Нужен для
    // eager-trigger'а .mount после `enable --now .automount` — .automount
    // только регистрирует path-watcher и реально монтирует лишь при
    // первом обращении к Where=, a UI/пользователь ожидает «active»
    // сразу после сохранения.
    virtual bool start(std::string_view unit, std::string& err) = 0;

    // fix44: `systemctl reset-failed <unit>`. Снимает память systemd о
    // прошлом сбое: без этого второй apply после неудачного start
    // натыкается на хвост «failed» от предыдущей попытки даже после
    // перезаписи unit-файла и daemon-reload (особенно когда unit с тем
    // же Where= уже падал и был удалён с диска).
    // Юнит, которого нет в системе → не ошибка: нечего сбрасывать.
    virtual bool resetFailed(std::string_view unit, std::string& err) = 0;

    // Возвращает то, что systemctl называет ActiveState: "active",
    // "inactive", "failed", "activating", "deactivating", "reloading",
    // "maintenance". Пустая строка — юнит не найден.
    virtual std::string activeState(std::string_view unit) = 0;

    // Один pass: LoadState/ActiveState/SubState для конкретного юнита.
    // id выставляется вызывающим — мы здесь только заполняем три
    // load-bearing поля.
    // fix42: list-by-prefix дропнут — после перехода с id-based имён
    // на systemd-escape-derived basename'ы общий префикс у юнитов
    // отсутствует. Клиент знает все {id, target} из БД и опрашивает
    // юниты по конкретным именам.
    virtual UnitState unitState(std::string_view unit) = 0;
};

class SystemctlClient final : public ISystemctl {
public:
    struct Config {
        // Путь к systemctl. По умолчанию — стандартный посикс location.
        std::filesystem::path bin_path;

        // Тайм-аут одной операции. daemon-reload может занять 1-2с,
        // enable --now — несколько секунд если unit делает реальный
        // mount(2) к недоступному серверу.
        std::chrono::milliseconds op_timeout{15000};

        // По умолчанию true: операции targeting'ятся в `--system`. Тесты
        // могут гонять `--user` (никогда не нужно в проде).
        bool system_scope = true;
    };

    static Config defaultConfig();

    explicit SystemctlClient(Config cfg);

    bool daemonReload(std::string& err) override;
    bool enableNow (std::string_view unit, std::string& err) override;
    bool disableNow(std::string_view unit, std::string& err) override;
    bool start     (std::string_view unit, std::string& err) override;
    bool resetFailed(std::string_view unit, std::string& err) override;
    std::string activeState(std::string_view unit) override;
    UnitState   unitState  (std::string_view unit) override;

private:
    Config cfg_;
    std::vector<std::string> baseArgv() const;
};

}  // namespace liveqx::mounts
