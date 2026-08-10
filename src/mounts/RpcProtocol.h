#pragma once
//
// fix41 — формат RPC между liveqx и liveqx-mountd.
//
// Wire format:
//   ┌──────────────────┬─────────────────────────────────────┐
//   │ uint32 BE length │ JSON body                           │
//   └──────────────────┴─────────────────────────────────────┘
//
// Одна пара request/response на соединение, после чего сокет закрывается.
// Это упрощает state machine у обоих сторон и убирает необходимость в
// keep-alive / id-matching.
//
// Почему JSON, а не protobuf / собственный binary: эта поверхность
// низкочастотная (CRUD оператора, не data plane), читаемость дампа
// сокета бьёт компактность, а зависимость от nlohmann/json уже есть.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "mounts/MountSpec.h"

namespace liveqx::mounts {

// 4-байт big-endian header. Размер тела ограничен — иначе
// злонамеренный/глюканутый клиент мог бы нас OOM-нуть.
inline constexpr std::uint32_t kMaxFrameBytes = 256 * 1024;

enum class RpcOp {
    ApplyMount,
    RemoveMount,
    TestMount,
    Status,
};

const char* toString(RpcOp op) noexcept;
bool        rpcOpFromString(std::string_view s, RpcOp& out) noexcept;

// Описание одного юнита, как его видит systemd. Для Status-ответа.
struct UnitState {
    std::int64_t id          = 0;
    std::string  load_state;     // "loaded" / "not-found" / ...
    std::string  active_state;   // "active" / "inactive" / "failed" / ...
    std::string  sub_state;      // "mounted" / "dead" / "running" / ...
    bool         active() const noexcept { return active_state == "active"; }
};

// Все ответы наследуются: ok=false влечёт error непустым; ok=true для
// специфичных полей кладёт их в `extra`.
struct RpcResponse {
    bool           ok = false;
    std::string    status;       // "mounted" / "failed" / "not_mounted" / ...
    std::string    error;        // не пусто при ok=false
    nlohmann::json extra;        // поле-зависимая нагрузка (file_count и т.п.)

    static RpcResponse okWith(std::string status,
                              nlohmann::json extra = {});
    static RpcResponse fail (std::string error,
                             std::string status = "failed");

    nlohmann::json toJson() const;
    static RpcResponse fromJson(const nlohmann::json& j);
};

// Frame helpers. encodeFrame(): добавляет 4-байт BE header. decodeFrame():
// валидирует header, читает тело. Оба сводят сериализацию JSON в
// один buffer без лишних копий.
std::vector<std::uint8_t> encodeFrame(const nlohmann::json& body);

// Чтение из FD (blocking). Возвращает true + body при успехе. При
// EOF / error возвращает false (out_error populated). Для использования
// и сервером, и клиентом. Контракт: только cap'нутые до kMaxFrameBytes
// чтения, чтобы single connection не повалил процесс.
bool readFrame(int fd, nlohmann::json& out_body, std::string& out_error);
bool writeFrame(int fd, const nlohmann::json& body, std::string& out_error);

}  // namespace liveqx::mounts
