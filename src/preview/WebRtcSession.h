#pragma once

// fix34 D2.2 — WebRTC peer connection wrapper (libdatachannel-bound).
//
// Один экземпляр = один browser-peer для preview-канала. Жизненный цикл:
//
//   PreviewManager::createOffer() ─▶ make_shared<WebRtcSession>(...)
//                                     ├▶ setRemoteOffer(offer)        — REST поток
//                                     │   возвращает SDP answer
//                                     ├▶ pushNal(...) (×N)            — render-поток
//                                     └▶ close()                       — REST поток (DELETE)
//                                         или idle-shutdown / peer-disconnect
//
// Сессия PUBLIC-only выставляет `PreviewSession` интерфейс (см.
// PreviewSession.h) — libdatachannel API не утекает за пределы .cpp,
// чтобы прод-сборки без флага `LIVEQX_ENABLE_WEBRTC_PREVIEW`
// никогда не парсили `<rtc/...>` заголовки.
//
// Race-safety (D2.5): `close()` атомарно ставит shutdown_flag, дальше
// peer connection уничтожается ПОСЛЕ того, как все in-flight `pushNal()`
// завершились. shared_ptr<WebRtcSession> в map'е PreviewManager даёт
// нижнюю границу времени жизни на время render-итерации.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "preview/PreviewSession.h"

namespace liveqx::preview {

class WebRtcSession : public PreviewSession {
public:
    struct Config {
        std::vector<std::string> ice_servers;   // напр. "stun:stun.l.google.com:19302"
        int   width      = 854;
        int   height     = 480;
        int   payload_pt = 96;       // dynamic H.264 PT
        int   clock_rate = 90'000;   // H.264 clock

        // Колбек «дайте мне keyframe прямо сейчас». Вызывается:
        //  • Один раз на переходе peer connection в Connected — чтобы
        //    браузер увидел декодируемый IDR сразу, а не через gop=60.
        //  • На каждом RTCP PLI/FIR от получателя (libdatachannel
        //    PliHandler). Получатель шлёт PLI после потерь, чтобы
        //    переинициализировать декодер.
        // Вызывается из libdatachannel-тредов; реализация должна быть
        // быстрой и lock-free (мы дёргаем PreviewEncoder::forceKeyframe()
        // — atomic-флаг, безопасно).
        std::function<void()> on_keyframe_request;
    };

    enum class OfferResult {
        Ok,
        BadOffer,             // SDP не валиден / setRemoteDescription упал
        InternalError,        // libdatachannel не смог собрать peer connection
        FeatureDisabled,      // компиляция без LIVEQX_ENABLE_WEBRTC_PREVIEW
    };

    WebRtcSession(int channel_id, std::string session_id, Config cfg);
    ~WebRtcSession() override;

    // ── PreviewSession interface ────────────────────────────────────
    std::string    id()         const override { return session_id_; }
    std::string    remoteAddr() const override;
    nlohmann::json statsJson()  const override;
    void           pushNal(const std::uint8_t* data, std::size_t size,
                           bool is_keyframe, std::int64_t pts_us) override;
    void           close() override;
    bool           isAlive()   const noexcept override { return alive_.load(); }

    // ── Offer / Answer ─────────────────────────────────────────────
    // Создаёт peer connection, добавляет H.264 video track, ставит
    // remote offer и собирает local answer. Блокирующий, ждёт ICE
    // gathering complete (или timeout 3s).  `out_sdp` заполняется при
    // OfferResult::Ok.
    OfferResult setRemoteOffer(const std::string& offer_sdp,
                               std::string* out_sdp_answer);

    // Channel id, к которому привязана сессия — для логов и stats.
    int channelId() const noexcept { return channel_id_; }

private:
    // Pimpl, чтобы libdatachannel-заголовки не утекали наружу.
    struct Impl;
    std::unique_ptr<Impl>     impl_;

    int                       channel_id_;
    std::string               session_id_;
    Config                    cfg_;

    // Hot-path флаг: true пока peer connection активен.
    std::atomic<bool>         alive_{false};
    // Защищает close()/pushNal() от race с peer destroy. close() сначала
    // ставит alive_=false, потом берёт mutex — pushNal() видит флаг и
    // возвращается раньше, чем close() начинает разбор peer connection.
    mutable std::mutex        send_mu_;

    // Метрики (atomic — читаются из statsJson() с другого потока).
    std::atomic<std::uint64_t> rtp_packets_sent_{0};
    std::atomic<std::uint64_t> rtp_bytes_sent_{0};
    std::atomic<std::uint64_t> nals_dropped_after_close_{0};
    std::int64_t               started_at_unix_ = 0;
};

}  // namespace liveqx::preview
