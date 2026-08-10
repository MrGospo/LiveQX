#include "preview/WebRtcSession.h"

#include <chrono>
#include <condition_variable>
#include <cstring>

#include "utils/Log.h"

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
#include <rtc/rtc.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#endif

namespace liveqx::preview {

// ─── Impl ────────────────────────────────────────────────────────────────────

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW

struct WebRtcSession::Impl {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track>          track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_cfg;

    // ICE gathering complete signal — setRemoteOffer ждёт его перед
    // отдачей local SDP, чтобы answer уже содержал ICE candidates
    // (trickle-ICE мы пока не реализуем, см. fix34-decisions.md).
    std::mutex                            gather_mu;
    std::condition_variable               gather_cv;
    bool                                  gather_done = false;

    // Кэш remote-адреса для statsJson (заполняется при connection state).
    std::mutex                            addr_mu;
    std::string                           remote_addr;
};

#else

// Под флагом OFF — пустая структура, чтобы unique_ptr не падал на
// incomplete type в дтор/ctor шаблонах.
struct WebRtcSession::Impl {};

#endif  // LIVEQX_ENABLE_WEBRTC_PREVIEW

// ─── Lifecycle ───────────────────────────────────────────────────────────────

WebRtcSession::WebRtcSession(int channel_id, std::string session_id, Config cfg)
    : impl_(std::make_unique<Impl>()),
      channel_id_(channel_id),
      session_id_(std::move(session_id)),
      cfg_(std::move(cfg)) {
    started_at_unix_ = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

WebRtcSession::~WebRtcSession() {
    // Гарантия: к моменту дтора close() уже вызвали (PreviewManager
    // делает это перед erase из map). На всякий случай дублируем —
    // close() идемпотентен.
    close();
}

// ─── PreviewSession interface ────────────────────────────────────────────────

std::string WebRtcSession::remoteAddr() const {
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    std::lock_guard<std::mutex> lk(impl_->addr_mu);
    return impl_->remote_addr;
#else
    return {};
#endif
}

nlohmann::json WebRtcSession::statsJson() const {
    return {
        {"id",                       session_id_},
        {"channel_id",               channel_id_},
        {"alive",                    alive_.load()},
        {"started_at_unix",          started_at_unix_},
        {"rtp_packets_sent",         rtp_packets_sent_.load()},
        {"rtp_bytes_sent",           rtp_bytes_sent_.load()},
        {"nals_dropped_after_close", nals_dropped_after_close_.load()},
        {"remote_addr",              remoteAddr()},
    };
}

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW

void WebRtcSession::pushNal(const std::uint8_t* data, std::size_t size,
                            bool /*is_keyframe*/, std::int64_t /*pts_us*/) {
    if (!alive_.load(std::memory_order_acquire)) {
        nals_dropped_after_close_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Передаём весь access unit (он уже в Annex-B). Packetizer в цепочке
    // нарежет на RTP-пакеты. Защищаемся от close()/peer destroy через
    // send_mu_ — close() ставит alive_=false ПЕРЕД захватом mutex,
    // поэтому мы либо успеваем под старый peer connection, либо видим
    // alive_=false на следующей итерации.
    std::lock_guard<std::mutex> lk(send_mu_);
    if (!alive_.load(std::memory_order_acquire) || !impl_->track) return;
    try {
        impl_->track->send(reinterpret_cast<const std::byte*>(data), size);
        rtp_packets_sent_.fetch_add(1, std::memory_order_relaxed);
        rtp_bytes_sent_.fetch_add(size, std::memory_order_relaxed);
    } catch (const std::exception& e) {
        // libdatachannel может бросить, если track закрыт между нашей
        // проверкой alive_ и send(). Это нормально — гасим и помечаем
        // сессию мёртвой, manager подберёт через idle-tick.
        LOG_WARN("WebRtcSession[{}/{}]: track->send() threw: {}",
                 channel_id_, session_id_, e.what());
        alive_.store(false, std::memory_order_release);
    }
}

void WebRtcSession::close() {
    // Step 1: гасим hot-path флаг — render-тред сразу перестаёт пушить.
    // Делаем безусловно (даже если alive_ уже был false), потому что
    // сценарий "setRemoteOffer создал pc + повесил callbacks → бросил
    // ДО alive_=true" оставляет pc в impl_ и нам всё равно нужно его
    // снести; иначе callback на onStateChange выстрелит из ~PeerConnection
    // когда WebRtcSession уже dead → segfault.
    alive_.store(false, std::memory_order_release);

    // Step 2: лочим send_mu_, чтобы дождаться завершения in-flight pushNal.
    // После этого можно безопасно разбирать peer connection.
    std::lock_guard<std::mutex> lk(send_mu_);
    if (!impl_->pc) return;  // идемпотентность

    // Сначала отцепляем callbacks — они захватили `this`, а pc->close()
    // в libdatachannel асинхронный (через ThreadPool), и без detach
    // лямбда может выстрелить уже после ~WebRtcSession → segfault.
    try {
        impl_->pc->onStateChange(nullptr);
        impl_->pc->onGatheringStateChange(nullptr);
        impl_->pc->onSignalingStateChange(nullptr);
        impl_->pc->onLocalDescription(nullptr);
        impl_->pc->onLocalCandidate(nullptr);
    } catch (...) { /* старая версия без некоторых API — игнорируем */ }

    try { impl_->pc->close(); }
    catch (const std::exception& e) {
        LOG_WARN("WebRtcSession[{}/{}]: pc.close() threw: {}",
                 channel_id_, session_id_, e.what());
    }
    impl_->track.reset();
    impl_->pc.reset();
}

WebRtcSession::OfferResult
WebRtcSession::setRemoteOffer(const std::string& offer_sdp,
                              std::string* out_sdp_answer) {
    if (offer_sdp.empty()) return OfferResult::BadOffer;

    rtc::Configuration rtc_cfg;
    for (const auto& s : cfg_.ice_servers) {
        try { rtc_cfg.iceServers.emplace_back(s); }
        catch (const std::exception& e) {
            LOG_WARN("WebRtcSession[{}/{}]: bad ICE server '{}': {}",
                     channel_id_, session_id_, s, e.what());
        }
    }

    try {
        impl_->pc = std::make_shared<rtc::PeerConnection>(rtc_cfg);

        // ── ICE gathering complete сигналим через CV. ───────────────
        impl_->pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                std::lock_guard<std::mutex> lk(impl_->gather_mu);
                impl_->gather_done = true;
                impl_->gather_cv.notify_all();
            }
        });

        impl_->pc->onStateChange([this](rtc::PeerConnection::State s) {
            using S = rtc::PeerConnection::State;
            // alive_ ставим ИМЕННО здесь, а не в конце setRemoteOffer:
            // после возврата из setRemoteOffer DTLS ещё не установлен,
            // и любой track->send() из render-треда вылетает с
            // "Track is closed" → ловим в catch → alive_=false → сессия
            // мертворождённая, все NAL'ы первой пачки уходят в drop.
            // На Connected заполняем кэш remote_addr — пригождается в
            // statsJson() и в preview-debug UI.
            if (s == S::Connected) {
                if (auto addr = impl_->pc->remoteAddress(); addr.has_value()) {
                    std::lock_guard<std::mutex> lk(impl_->addr_mu);
                    impl_->remote_addr = *addr;
                }
                alive_.store(true, std::memory_order_release);
                // Сразу просим IDR. Без этого браузер первым получает
                // P-frame из текущего GOP и не может декодировать до
                // следующего планового IDR (gop=60 → до 2.4s @ 25fps
                // чёрного экрана). С force-keyframe декодер видит IDR
                // на ближайшем render-tick'е.
                if (cfg_.on_keyframe_request) {
                    try { cfg_.on_keyframe_request(); }
                    catch (...) { /* never let user callback poison libdc */ }
                }
            } else if (s == S::Disconnected || s == S::Failed || s == S::Closed) {
                alive_.store(false, std::memory_order_release);
            }
            LOG_DEBUG("WebRtcSession[{}/{}]: pc state {}",
                      channel_id_, session_id_, static_cast<int>(s));
        });

        // ── Video track (sendonly H.264, payload 96). ───────────────
        const std::uint32_t ssrc = static_cast<std::uint32_t>(
            std::chrono::system_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
        const std::string  cname = "preview-" + session_id_;

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(cfg_.payload_pt);
        video.addSSRC(ssrc, cname);

        impl_->track = impl_->pc->addTrack(video);

        impl_->rtp_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, cname, cfg_.payload_pt,
            rtc::H264RtpPacketizer::defaultClockRate);

        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, impl_->rtp_cfg);
        auto sr_reporter   = std::make_shared<rtc::RtcpSrReporter>(impl_->rtp_cfg);
        auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();
        // PLI/FIR от браузера → enкодер выдаёт IDR. Без этого после
        // burst-потерь декодер на стороне браузера «застревает» до
        // следующего планового IDR через gop=60. PliHandler вызывает
        // callback из сетевого треда libdatachannel — внутри forceKeyframe()
        // только atomic.store, лок-free.
        auto pli_handler = std::make_shared<rtc::PliHandler>(
            [cb = cfg_.on_keyframe_request] {
                if (cb) {
                    try { cb(); } catch (...) { /* shield libdc */ }
                }
            });
        packetizer->addToChain(sr_reporter);
        packetizer->addToChain(nack_responder);
        packetizer->addToChain(pli_handler);
        impl_->track->setMediaHandler(packetizer);

        // ── Apply offer + сгенерировать local answer. ───────────────
        rtc::Description offer(offer_sdp, "offer");
        impl_->pc->setRemoteDescription(offer);

        // Ждём ICE gathering complete (3s timeout) — без trickle-ICE
        // браузер должен получить self-contained SDP сразу.
        {
            std::unique_lock<std::mutex> lk(impl_->gather_mu);
            impl_->gather_cv.wait_for(lk, std::chrono::seconds(3),
                                       [this] { return impl_->gather_done; });
        }

        if (auto local = impl_->pc->localDescription(); local.has_value()) {
            if (out_sdp_answer) *out_sdp_answer = std::string(local.value());
        } else {
            LOG_ERROR("WebRtcSession[{}/{}]: localDescription() empty after gather",
                      channel_id_, session_id_);
            return OfferResult::InternalError;
        }

        // alive_ выставит onStateChange когда DTLS реально установится.
        // До этого момента pushNal() видит alive_=false и кидает NAL'ы
        // в drop-счётчик — это норма, ничего полезного отправить нельзя
        // пока peer-connection не в Connected.
        return OfferResult::Ok;
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession[{}/{}]: setRemoteOffer threw: {}",
                  channel_id_, session_id_, e.what());
        // На bad-offer pc уже создан + callbacks повешены. Если не
        // снести его сейчас, в ~PeerConnection libdatachannel может
        // вызвать onStateChange из ThreadPool ПОСЛЕ ~WebRtcSession →
        // segfault при exit'е процесса. Сносим явно через close().
        close();
        return OfferResult::InternalError;
    }
}

#else  // LIVEQX_ENABLE_WEBRTC_PREVIEW

void WebRtcSession::pushNal(const std::uint8_t*, std::size_t,
                            bool, std::int64_t) {
    nals_dropped_after_close_.fetch_add(1, std::memory_order_relaxed);
}

void WebRtcSession::close() {
    alive_.store(false, std::memory_order_release);
}

WebRtcSession::OfferResult
WebRtcSession::setRemoteOffer(const std::string& offer_sdp, std::string*) {
    if (offer_sdp.empty()) return OfferResult::BadOffer;
    return OfferResult::FeatureDisabled;
}

#endif  // LIVEQX_ENABLE_WEBRTC_PREVIEW

}  // namespace liveqx::preview
