#pragma once
//
// fix41 — AF_UNIX RPC server в `liveqx-mountd`.
//
// Слушает на указанном сокете, принимает соединения, валидирует peer
// credentials через SO_PEERCRED (только root или член группы
// liveqx допускаются), читает один frame, диспатчит в handler,
// пишет ответ, закрывает соединение.
//
// Sequential design: один обработчик на момент времени. Поверхность
// низкочастотная — оператор кликает «Apply» раз в N минут — а
// конкурентные ApplyMount'ы трогают одни и те же systemd-юниты,
// так что serialization выгоднее, чем worker pool.
//
// Безопасность:
//   • SO_PEERCRED: getsockopt с SOL_SOCKET. Linux-only; OS X / FreeBSD
//     потребовали бы getpeereid, но проект Linux-only.
//   • Каталог сокета /run/liveqx (mode 0750 root:liveqx)
//     создаётся install-скриптом. Если каталога нет — mountd падает на
//     старте с ясной ошибкой.
//   • На bind() сокет получает mode 0660 (chmod после bind, то есть
//     до listen). Группа сокета задаётся chown'ом; install-скрипт
//     гарантирует существование группы liveqx.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "mounts/RpcProtocol.h"

namespace liveqx::mounts {

// Handler принимает op + parsed body, возвращает RpcResponse.
// Body всегда — JSON-объект; op уже извлечён сервером.
using RpcHandler = std::function<
    RpcResponse(RpcOp op, const nlohmann::json& body)>;

class RpcServer {
public:
    struct Config {
        // Полный путь к unix-сокету. Должен лежать в каталоге, в который
        // только root + group=liveqx имеют write.
        std::filesystem::path socket_path = kDefaultSocketPath;

        // Имя группы, члены которой могут подключиться. Default —
        // kClientGroupName ("liveqx"). Тесты передают группу
        // самого пользователя, чтобы избежать chown.
        std::string client_group = kClientGroupName;

        // mode сокета после bind. 0660 — group write нужен клиенту.
        mode_t socket_mode = 0660;

        // Если true — пропускаем chown сокета на client_group.
        // Полезно в тестах, где у юзера нет прав делать chgrp.
        bool skip_chown = false;
    };

    RpcServer(Config cfg, RpcHandler handler);
    ~RpcServer();

    RpcServer(const RpcServer&)            = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // Создаёт сокет, делает bind/listen. Возвращает false при любом
    // системном провале (порт занят, нет каталога, нет группы); ошибка
    // пишется в out_error.
    bool start(std::string& out_error);

    // Блокирующий accept-loop. Завершается, когда `stop()` зовётся
    // другим тредом. Возвращает в момент закрытия слушающего сокета.
    void runForever();

    // Сигналим runForever() выйти. Безопасно из любого треда (в т.ч.
    // из обработчика SIGTERM).
    void stop();

    // Только для тестов.
    int  listenFd() const noexcept { return listen_fd_; }

private:
    // Принимает connection, валидирует peer, запускает handler. Любая
    // ошибка → лог + закрытие сокета, без рестарта (следующее accept
    // отдельное соединение).
    void handleOne(int conn_fd);

    // True если peer'у разрешён доступ. Логирует причины отказа.
    bool authorizePeer(int conn_fd, std::string& out_who);

    Config            cfg_;
    RpcHandler        handler_;
    int               listen_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace liveqx::mounts
