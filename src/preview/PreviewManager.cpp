#include "preview/PreviewManager.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

#include "preview/WebRtcSession.h"
#include "utils/Log.h"

namespace liveqx::preview {

namespace {

// Случайный 16-hex-char ID для сессии (≈64 бита энтропии). Ничего
// crypto-критичного — это просто session handle, видимый в URL DELETE.
std::string makeSessionId() {
    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    const std::uint64_t v = rng();
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << v;
    return os.str();
}

}  // namespace

bool PreviewManager::isCompiled() noexcept {
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    return true;
#else
    return false;
#endif
}

PreviewManager::PreviewManager() = default;
PreviewManager::PreviewManager(Config cfg) : cfg_(std::move(cfg)) {}
PreviewManager::~PreviewManager() = default;

void PreviewManager::onChannelFrame(int channel_id, const Frame& frame) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return;
    if (auto* enc = it->second->encoder.get()) {
        enc->encode(frame);
    }
}

void PreviewManager::stopChannel(int channel_id) {
    std::unique_ptr<ChannelState> evicted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return;
        evicted = std::move(it->second);
        channels_.erase(it);
    }
    if (!evicted) return;
    // fix34 D2.5 — закрываем сессии явно ПЕРЕД encoder->stop(), чтобы
    // ни одна in-flight pushNal() не пыталась дёрнуть мёртвый track.
    // close() ставит alive_=false и блокируется до завершения текущего
    // send(); только потом стартует destruction peer connection.
    for (auto& [_, sp] : evicted->sessions) {
        if (sp) sp->close();
    }
    if (evicted->encoder) evicted->encoder->stop();
}

PreviewManager::Result
PreviewManager::createOffer(int channel_id, const nlohmann::json& body,
                            nlohmann::json* out_answer) {
    if (!isCompiled()) return Result::NotCompiled;

    if (!body.is_object()) return Result::BadOffer;
    const auto sdp = body.value("sdp",  std::string{});
    const auto t   = body.value("type", std::string{});
    if (sdp.empty() || t != "offer") return Result::BadOffer;

    std::lock_guard<std::mutex> lk(mu_);

    // Global cap: count channels with at least one running session.
    int active_channels = 0;
    for (const auto& [_, st] : channels_) {
        if (!st->sessions.empty()) ++active_channels;
    }

    auto& slot = channels_[channel_id];
    const bool fresh = !slot;
    if (fresh) slot = std::make_unique<ChannelState>();

    if (fresh && active_channels >= cfg_.max_active_channels) {
        channels_.erase(channel_id);
        return Result::CapacityExhausted;
    }

    if (static_cast<int>(slot->sessions.size())
        >= cfg_.max_clients_per_channel) {
        return Result::TooManyClients;
    }

    // ── ENABLE_WEBRTC_PREVIEW = ON path (fix34 D2.4) ────────────────
#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
    // Создаём peer connection + track. WebRtcSession.cpp отвечает за
    // ICE gather + SDP answer; на render-треде PreviewManager позже
    // вызовет session->pushNal() через encoder fan-out (D2.8+).
    WebRtcSession::Config wcfg;
    wcfg.ice_servers = cfg_.default_ice_servers;
    if (body.contains("ice_servers") && body["ice_servers"].is_array()) {
        wcfg.ice_servers.clear();
        for (const auto& s : body["ice_servers"]) {
            if (s.is_string()) wcfg.ice_servers.push_back(s.get<std::string>());
        }
    }

    // fix34 D2.8 — лениво поднимаем PreviewEncoder при первой сессии.
    // Делаем это ДО setRemoteOffer, чтобы wcfg.on_keyframe_request
    // указывал на уже стартовавший энкодер. NalCallback фанаутит
    // Annex-B access unit по всем живым сессиям канала. Колбек срабатывает
    // синхронно изнутри encode(), который зовётся из onChannelFrame() уже
    // под mu_ — повторный channels_.find под тем же mu_ безопасен.
    // SPS/PPS prepend на keyframe уже сделан самим x264 (repeat-headers=1,
    // см. PreviewEncoder::start).
    if (!slot->encoder) {
        PreviewEncoder::Config ecfg;  // дефолты: 854×480 25fps 500kbps
        slot->encoder = std::make_unique<PreviewEncoder>(ecfg);
        const bool ok = slot->encoder->start(
            [this, channel_id](const std::uint8_t* data, std::size_t size,
                               bool is_keyframe, std::int64_t pts_us) {
                // mu_ уже захвачен render-тредом через onChannelFrame.
                auto it = channels_.find(channel_id);
                if (it == channels_.end()) return;
                for (auto& [_, sp] : it->second->sessions) {
                    if (sp) sp->pushNal(data, size, is_keyframe, pts_us);
                }
            });
        if (!ok) {
            LOG_ERROR("PreviewManager::createOffer: ch={} encoder->start() "
                      "failed; aborting offer", channel_id);
            slot->encoder.reset();
            if (fresh) channels_.erase(channel_id);
            return Result::InternalError;
        }
    }

    // Колбек «дайте мне IDR прямо сейчас» — будет звать
    // requestKeyframeForChannel из libdatachannel-тредов (на Connected и
    // при RTCP PLI/FIR). Через канал-id, не через указатель на encoder —
    // safer при stopChannel(), который вынесет slot из map'а.
    wcfg.on_keyframe_request = [this, channel_id] {
        requestKeyframeForChannel(channel_id);
    };

    auto sid = makeSessionId();
    auto sess = std::make_shared<WebRtcSession>(channel_id, sid, wcfg);

    std::string answer_sdp;
    const auto r = sess->setRemoteOffer(sdp, &answer_sdp);
    if (r != WebRtcSession::OfferResult::Ok) {
        LOG_ERROR("PreviewManager::createOffer: ch={} session={} setRemoteOffer "
                  "failed (rc={})", channel_id, sid, static_cast<int>(r));
        // Если канал был fresh и encoder только что подняли — сносим всё,
        // чтобы не оставлять пустой slot с running encoder без сессий.
        if (fresh) {
            if (slot->encoder) slot->encoder->stop();
            channels_.erase(channel_id);
        }
        return r == WebRtcSession::OfferResult::BadOffer
                   ? Result::BadOffer
                   : Result::InternalError;
    }

    if (slot->started_at_unix == 0) {
        slot->started_at_unix = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
    slot->sessions.emplace(sid, sess);
    slot->last_client_unix = 0;  // активный клиент — idle-таймер сбрасываем

    if (out_answer) {
        *out_answer = {
            {"session_id", sid},
            {"sdp",        answer_sdp},
            {"type",       "answer"},
        };
    }
    return Result::Ok;
#else
    (void)out_answer;
    return Result::NotCompiled;
#endif
}

PreviewManager::Result
PreviewManager::closeSession(int channel_id, const std::string& session_id) {
    std::shared_ptr<PreviewSession> evicted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return Result::NotFound;
        auto sit = it->second->sessions.find(session_id);
        if (sit == it->second->sessions.end()) return Result::NotFound;
        evicted = std::move(sit->second);
        it->second->sessions.erase(sit);
        if (it->second->sessions.empty()) {
            it->second->last_client_unix = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }
    // fix34 D2.5 — close() вне лока: ждёт окончания текущего pushNal() и
    // тогда разбирает peer connection. Если сделать под mu_, render-тред
    // не сможет завершить send() (он же уже хочет mu_ через onChannelFrame).
    if (evicted) evicted->close();
    return Result::Ok;
}

nlohmann::json PreviewManager::statsJson(int channel_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return nullptr;
    const auto& st = *it->second;

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& [sid, sp] : st.sessions) {
        if (sp) sessions.push_back(sp->statsJson());
    }

    nlohmann::json encoder = nullptr;
    if (st.encoder) {
        encoder = {
            {"running",        st.encoder->running()},
            {"frames_encoded", st.encoder->framesEncoded()},
            {"bytes_emitted",  st.encoder->bytesEmitted()},
        };
    }
    return {
        {"channel_id",       channel_id},
        {"encoder",          encoder},
        {"sessions",         sessions},
        {"started_at_unix",  st.started_at_unix},
        {"last_client_unix", st.last_client_unix},
    };
}

nlohmann::json PreviewManager::globalSnapshotJson() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [cid, st] : channels_) {
        arr.push_back({
            {"channel_id",     cid},
            {"sessions",       st->sessions.size()},
            {"encoder_active", st->encoder && st->encoder->running()},
        });
    }
    return {
        {"compiled",    isCompiled()},
        {"max_per_ch",  cfg_.max_clients_per_channel},
        {"max_active",  cfg_.max_active_channels},
        {"idle_sec",    cfg_.idle_shutdown_sec},
        {"channels",    arr},
    };
}

void PreviewManager::requestKeyframeForChannel(int channel_id) {
    // Берём mu_ КОРОТКО — только чтобы безопасно дойти до encoder.get().
    // forceKeyframe() сам по себе lock-free (atomic.store), так что
    // удерживать mu_ ему не нужно. Из render-треда этот метод НЕ
    // вызывается (см. WebRtcSession), значит deadlock с onChannelFrame
    // невозможен.
    std::lock_guard<std::mutex> lk(mu_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second->encoder) return;
    it->second->encoder->forceKeyframe();
}

void PreviewManager::tickIdleShutdown(std::int64_t now_unix_sec) {
    std::vector<int> to_evict;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [cid, st] : channels_) {
            if (!st->sessions.empty()) continue;
            if (st->last_client_unix == 0) continue;     // never had clients yet
            if (now_unix_sec - st->last_client_unix
                  >= cfg_.idle_shutdown_sec) {
                to_evict.push_back(cid);
            }
        }
    }
    for (int cid : to_evict) {
        LOG_INFO("PreviewManager: idle shutdown channel={}", cid);
        stopChannel(cid);
    }
}

}  // namespace liveqx::preview
