#pragma once

// fix23 commit 7 — PreviewManager.
//
// Process-wide registry of active WebRTC preview encoders + sessions.
// Wired into ChannelManager so each channel can spawn / shut down its
// preview pipeline on demand.
//
// Cap policy (defaults; override via global config "preview" object):
//   max_clients_per_channel = 10
//   max_active_channels     = 20
//   idle_shutdown_sec       = 30   (seconds without a connected client
//                                    before the encoder is torn down)
//
// libdatachannel-bound SDP / ICE / RTP packetisation lives behind the
// LIVEQX_ENABLE_WEBRTC_PREVIEW compile guard. Builds without
// the flag still get the manager + REST surface — `attachOffer()`
// returns NotCompiled so the REST layer can map it to 501.
//
// Threading: createOffer / closeSession may be called from REST worker
// threads; perChannelEncode() is called from the channel render loop.
// All public methods take an internal mutex; the encoder callback runs
// on the render thread without re-entering the manager (the session
// owns its own NAL buffer).

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Frame.h"
#include "preview/PreviewEncoder.h"
#include "preview/PreviewSession.h"

namespace liveqx::preview {

class PreviewManager {
public:
    struct Config {
        int max_clients_per_channel = 10;
        int max_active_channels     = 20;
        int idle_shutdown_sec       = 30;
        // Пусто по дефолту: liveqx и оператор обычно сидят в одной
        // LAN, host-кандидаты ICE договариваются напрямую, STUN бесполезен
        // и только создаёт мёртвую srflx-пару (см. fix45 — NAT hairpin для
        // public-IP кандидата ломает RTP). Для remote-оператора через
        // интернет нужен TURN; STUN сам по себе NAT-traversal не решает.
        std::vector<std::string> default_ice_servers = {};
    };

    enum class Result {
        Ok,
        NotFound,            // 404 — channel/session id absent
        TooManyClients,      // 429 — per-channel cap exceeded
        CapacityExhausted,   // 503 — global cap exceeded
        BadOffer,            // 400 — malformed SDP / json
        NotCompiled,         // 501 — built without ENABLE_WEBRTC_PREVIEW
        InternalError,       // 500 — encoder/session start failed
    };

    PreviewManager();
    explicit PreviewManager(Config cfg);
    ~PreviewManager();

    PreviewManager(const PreviewManager&)            = delete;
    PreviewManager& operator=(const PreviewManager&) = delete;

    // Push a rendered frame to this channel's preview encoder. No-op
    // when the channel has no active preview. Called from the render
    // loop tap registered by ChannelInstance.
    void onChannelFrame(int channel_id, const Frame& frame);

    // Shut down a channel's preview pipeline (encoder + all sessions).
    // Idempotent. Called by ChannelInstance::stop().
    void stopChannel(int channel_id);

    // Negotiate a new browser ↔ core peer connection. body is the
    // {sdp, type:"offer", ice_servers?} JSON from the REST handler. On
    // success out_answer is filled with {session_id, sdp, type:"answer"}.
    Result createOffer(int channel_id, const nlohmann::json& body,
                       nlohmann::json* out_answer);

    // Manual session teardown. Idempotent.
    Result closeSession(int channel_id, const std::string& session_id);

    // Stats for /api/channels/{id}/preview/stats. Returns null when the
    // channel has no preview pipeline.
    nlohmann::json statsJson(int channel_id) const;

    // Snapshot for /api/preview (admin-only diagnostic): per-channel
    // summary across the whole process.
    nlohmann::json globalSnapshotJson() const;

    // Tick the idle-shutdown timer. Called by Watchdog at 1Hz; tears
    // down channels with no clients for >= idle_shutdown_sec.
    void tickIdleShutdown(std::int64_t now_unix_sec);

    // Запросить IDR-кадр на ближайший encode() для указанного канала.
    // Вызывается из WebRtcSession при переходе peer в Connected и при
    // получении RTCP PLI/FIR от браузера. Lock-free на encoder-уровне:
    // мы только берём mu_ короткое время, чтобы безопасно найти канал.
    // Из render-треда никогда не вызывается, поэтому deadlock'а с
    // onChannelFrame() нет.
    void requestKeyframeForChannel(int channel_id);

    // Compile-time / runtime feature gate. False on default builds.
    static bool isCompiled() noexcept;

private:
    struct ChannelState {
        std::unique_ptr<PreviewEncoder>                  encoder;
        std::map<std::string, std::shared_ptr<PreviewSession>> sessions;
        std::int64_t last_client_unix = 0;
        std::int64_t started_at_unix  = 0;
    };

    Config                            cfg_;
    mutable std::mutex                mu_;
    std::map<int, std::unique_ptr<ChannelState>> channels_;
};

}  // namespace liveqx::preview
