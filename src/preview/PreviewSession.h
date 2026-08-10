#pragma once

// fix34 D2.2 — abstract preview session.
//
// Базовый интерфейс для одной WebRTC-сессии (один peer connection).
// Конкретная реализация — `WebRtcSession` (см. WebRtcSession.h),
// собирается только под `LIVEQX_ENABLE_WEBRTC_PREVIEW`.
//
// `PreviewManager` хранит `shared_ptr<PreviewSession>` в session-map и
// дёргает только методы этого интерфейса; ничего libdatachannel-специфи-
// ческого не торчит наружу — если флаг выключен, manager просто не
// инстанциирует ни одной сессии.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::preview {

class PreviewSession {
public:
    PreviewSession()          = default;
    virtual ~PreviewSession() = default;

    PreviewSession(const PreviewSession&)            = delete;
    PreviewSession& operator=(const PreviewSession&) = delete;

    // ── Идентичность / диагностика ──────────────────────────────────
    virtual std::string    id()         const = 0;
    virtual std::string    remoteAddr() const = 0;
    virtual nlohmann::json statsJson()  const = 0;

    // ── Hot path — Annex-B NAL access unit от PreviewEncoder ─────────
    // Реализация должна:
    //  • быть thread-safe (зовётся с render-треда канала),
    //  • быть no-op после close() — без падений и race с peer destroy,
    //  • не блокировать render-тред дольше O(microseconds).
    virtual void pushNal(const std::uint8_t* data,
                         std::size_t size,
                         bool is_keyframe,
                         std::int64_t pts_us) = 0;

    // ── Lifecycle ────────────────────────────────────────────────────
    // Идемпотентный shutdown. После close() pushNal() = no-op.
    // Реализация должна гарантировать, что все in-flight pushNal-вызовы
    // завершились до возврата (см. fix34-decisions.md, D2.5).
    virtual void close() = 0;

    // True пока peer connection в состоянии Connected/Connecting.
    // Manager использует флаг для сборки idle-таймера и для отчёта.
    virtual bool isAlive() const noexcept = 0;
};

}  // namespace liveqx::preview
