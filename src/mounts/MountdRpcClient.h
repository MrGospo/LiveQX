#pragma once
//
// fix41 — клиент к liveqx-mountd для liveqx'a.
//
// Per-call connect/send/recv/close (как в RpcServer на helper-стороне):
// сокет открывается, шлётся один frame с {op, body}, читается один
// frame ответа, сокет закрывается. Это исключает stale-соединения,
// упрощает state-machine и даёт честные тайм-ауты на всю операцию.
//
// IMountdRpcClient — абстракция, чтобы MountManager тестировался с
// in-process mock'ом без поднятия сокета.
// MountdRpcClient — production-импл; целит в /run/liveqx/mountd.sock.

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mounts/MountSpec.h"
#include "mounts/RpcProtocol.h"

namespace liveqx::mounts {

// {id, target} пара для Status-RPC. Клиент перечисляет все
// смонтированные ресурсы из mounts.db и передаёт helper'у — без этого
// helper не знает, какие юниты опрашивать (после fix42 имена юнитов
// выводятся из target, общего префикса нет).
struct StatusQueryItem {
    std::int64_t id;
    std::string  target;
};

class IMountdRpcClient {
public:
    virtual ~IMountdRpcClient() = default;

    // Все методы возвращают RpcResponse с ok=false при network/timeout
    // ошибках. error содержит первоисточник сбоя.
    virtual RpcResponse applyMount (const MountSpec& spec) = 0;

    // fix42: target обязателен — basename юнита выводится из него
    // (см. unitBasenameByTarget). id используется для имени cred-файла
    // и для миграции pre-fix42 юнитов.
    virtual RpcResponse removeMount(std::int64_t id,
                                    std::string_view target) = 0;

    virtual RpcResponse testMount  (const MountSpec& spec) = 0;

    // fix42: список {id, target} обязателен (см. StatusQueryItem).
    virtual RpcResponse status(const std::vector<StatusQueryItem>& items) = 0;
};

class MountdRpcClient final : public IMountdRpcClient {
public:
    struct Config {
        std::filesystem::path socket_path = kDefaultSocketPath;

        // Wall-clock тайм-аут одного RPC. Должен быть строго больше, чем
        // op_timeout helper'а (15s default), потому что enable --now на
        // CIFS handshake может занять до 15s. Берём 30s — два operator-
        // ожидания.
        std::chrono::milliseconds rpc_timeout{30000};
    };

    explicit MountdRpcClient(Config cfg);

    RpcResponse applyMount (const MountSpec& spec) override;
    RpcResponse removeMount(std::int64_t id, std::string_view target) override;
    RpcResponse testMount  (const MountSpec& spec) override;
    RpcResponse status(const std::vector<StatusQueryItem>& items) override;

private:
    // Один call: connect → write frame → read frame → close.
    RpcResponse roundTrip(RpcOp op, const nlohmann::json& body);

    Config cfg_;
};

}  // namespace liveqx::mounts
