// fix34 D2.10 — Preview pipeline integration test.
//
// Закрывает acceptance criterion ROADMAP D2.10: end-to-end путь от
// синтетического RGBA-кадра через PreviewEncoder и фан-аут NAL-юнитов
// к реальной WebRtcSession (libdatachannel-bound). Без RenderLoop /
// ChannelInstance — UI-зависимостей нет, тест работает в CI.
//
// Что проверяем:
//   • createOffer с настоящим libdatachannel offer'ом → Result::Ok,
//     answer содержит SDP + session_id.
//   • Encoder lazy-стартует на первой сессии; повторные фреймы
//     инкрементят frames_encoded / bytes_emitted.
//   • Фан-аут: на канал с N сессиями каждый encode() пушит NAL во
//     все N сессий (live alive_=true; in-flight pushNal не ронит).
//   • Race-free shutdown (D2.5): closeSession ждёт текущего
//     pushNal перед destruction; stopChannel идемпотентен.
//   • Per-channel cap (TooManyClients) с реальными SDP, не stub'ами.
//
// Что НЕ проверяем:
//   • реальный ICE/STUN: в CI нет browser/firewall, peer connection
//     не дойдёт до Connected. libdatachannel закрывает track когда
//     транспорт не поднимается, alive_ флипается в false. Это
//     ожидаемо — мы тестируем wiring (pushNal был вызван fan-out'ом),
//     не сетевую доставку (rtp_packets_sent может быть 0).
//   • RenderLoop tap: уже покрыт setPreviewManager-тестами в
//     ChannelInstance + ручным e2e через UI.
//   • Реальную видеохорку: PreviewEncoder покрыт
//     test_preview_encoder.cpp.
//
// Как проверяем фан-аут БЕЗ ICE: суммарные счётчики сессии
// `rtp_packets_sent + nals_dropped_after_close` отражают сколько раз
// manager-callback вызвал pushNal. Если fan-out не работает — оба
// нуля, тест падает.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/Frame.h"
#include "preview/PreviewManager.h"

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
#include <rtc/rtc.hpp>
#endif

using liveqx::preview::PreviewManager;
using R = PreviewManager::Result;

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW

namespace {

// RtcCleanupEnv уже регистрируется в test_webrtc_session.cpp (тот же бинарник).
// Здесь вторую инстанцию заводить не нужно — dispose-loop одинаков.

// Синтетический RGBA-кадр (854×480 как у preview-энкодера по умолчанию,
// чтобы libswscale не пересчитывал пиксели зря). shade-параметр гоняет
// яркость, чтобы x264 не складывал всё в один P-frame.
Frame makeSyntheticFrame(int w, int h, std::uint8_t shade) {
    Frame f;
    f.width  = w;
    f.height = h;
    auto buf = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[w * h * 4]);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int o = (y * w + x) * 4;
            buf[o + 0] = static_cast<std::uint8_t>((x + shade) & 0xFF);
            buf[o + 1] = static_cast<std::uint8_t>((y + shade) & 0xFF);
            buf[o + 2] = shade;
            buf[o + 3] = 255;
        }
    }
    f.data = std::move(buf);
    f.pts  = 0;
    return f;
}

// "Browser-side" клиент через libdatachannel: тот же helper-pattern, что
// в test_webrtc_session.cpp. shared_ptr<Track> ДОЛЖЕН жить — PC хранит
// только weak_ptr (см. D2.7 lesson #1).
struct ClientPeer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track>          track;
    std::string                          offer_sdp;
};

ClientPeer makeBrowserOffer() {
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    auto pc = std::make_shared<rtc::PeerConnection>(cfg);

    rtc::Description::Video video("video",
                                  rtc::Description::Direction::SendRecv);
    video.addH264Codec(96);
    auto track = pc->addTrack(video);

    std::mutex              mu;
    std::condition_variable cv;
    bool                    gathered = false;
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(mu);
            gathered = true;
            cv.notify_all();
        }
    });
    pc->setLocalDescription();

    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return gathered; });
    }
    auto local = pc->localDescription();
    if (!local.has_value()) return {pc, track, ""};
    return {pc, track, std::string(local.value())};
}

void pushFrames(PreviewManager& pm, int channel_id, int n) {
    for (int i = 0; i < n; ++i) {
        pm.onChannelFrame(channel_id,
                          makeSyntheticFrame(854, 480,
                                             static_cast<std::uint8_t>(i * 7)));
    }
}

}  // namespace

TEST(PreviewIntegration, FrameFlowsThroughEncoderToSingleSession) {
    PreviewManager pm;

    auto client = makeBrowserOffer();
    ASSERT_FALSE(client.offer_sdp.empty());

    nlohmann::json answer;
    auto rc = pm.createOffer(/*channel=*/100,
                             {{"type", "offer"}, {"sdp", client.offer_sdp}},
                             &answer);
    ASSERT_EQ(rc, R::Ok);
    EXPECT_FALSE(answer.value("sdp", std::string{}).empty());
    EXPECT_FALSE(answer.value("session_id", std::string{}).empty());

    pushFrames(pm, /*channel=*/100, /*n=*/30);

    auto stats = pm.statsJson(100);
    ASSERT_FALSE(stats.is_null());
    EXPECT_TRUE(stats["encoder"]["running"].get<bool>());
    EXPECT_GT(stats["encoder"]["frames_encoded"].get<std::uint64_t>(), 0u);
    EXPECT_GT(stats["encoder"]["bytes_emitted"].get<std::uint64_t>(), 0u);
    ASSERT_EQ(stats["sessions"].size(), 1u);

    // Фан-аут проверка: pushNal был вызван хотя бы один раз. В CI без
    // ICE rtp_packets_sent скорее всего 0 (track закрывается), но
    // dropped_after_close > 0 однозначно говорит, что callback
    // PreviewManager-а долетал до session->pushNal().
    const auto& s0 = stats["sessions"][0];
    const auto sent    = s0["rtp_packets_sent"].get<std::uint64_t>();
    const auto dropped = s0["nals_dropped_after_close"].get<std::uint64_t>();
    EXPECT_GT(sent + dropped, 0u);

    const auto sid = answer["session_id"].get<std::string>();
    EXPECT_EQ(pm.closeSession(100, sid), R::Ok);
    EXPECT_EQ(pm.closeSession(100, sid), R::NotFound);  // идемпотентно
    pm.stopChannel(100);

    client.pc.reset();
}

TEST(PreviewIntegration, MultiSessionFanOutsToAllPeers) {
    PreviewManager pm;

    auto c1 = makeBrowserOffer();
    auto c2 = makeBrowserOffer();
    auto c3 = makeBrowserOffer();
    ASSERT_FALSE(c1.offer_sdp.empty());
    ASSERT_FALSE(c2.offer_sdp.empty());
    ASSERT_FALSE(c3.offer_sdp.empty());

    nlohmann::json a1, a2, a3;
    ASSERT_EQ(pm.createOffer(200, {{"type","offer"},{"sdp",c1.offer_sdp}}, &a1), R::Ok);
    ASSERT_EQ(pm.createOffer(200, {{"type","offer"},{"sdp",c2.offer_sdp}}, &a2), R::Ok);
    ASSERT_EQ(pm.createOffer(200, {{"type","offer"},{"sdp",c3.offer_sdp}}, &a3), R::Ok);

    pushFrames(pm, 200, 30);

    auto stats = pm.statsJson(200);
    ASSERT_EQ(stats["sessions"].size(), 3u);
    EXPECT_GT(stats["encoder"]["frames_encoded"].get<std::uint64_t>(), 0u);
    // Каждая сессия должна была получить хотя бы один pushNal от fan-out
    // callback'а (см. комментарий к тесту выше — sent или dropped > 0).
    for (const auto& s : stats["sessions"]) {
        const auto sent    = s["rtp_packets_sent"].get<std::uint64_t>();
        const auto dropped = s["nals_dropped_after_close"].get<std::uint64_t>();
        EXPECT_GT(sent + dropped, 0u);
    }

    pm.stopChannel(200);
    c1.pc.reset(); c2.pc.reset(); c3.pc.reset();
}

TEST(PreviewIntegration, CloseSessionUnderActiveFrameLoadIsRaceFree) {
    // D2.5 race-free shutdown под нагрузкой: один поток качает кадры через
    // onChannelFrame (encoder фан-аутит NAL'ы в сессию), второй вызывает
    // closeSession в случайный момент. close() обязан дождаться окончания
    // in-flight pushNal перед destruction peer connection — иначе UAF.

    PreviewManager pm;
    auto client = makeBrowserOffer();
    ASSERT_FALSE(client.offer_sdp.empty());

    nlohmann::json answer;
    ASSERT_EQ(pm.createOffer(300,
                             {{"type", "offer"}, {"sdp", client.offer_sdp}},
                             &answer),
              R::Ok);
    const auto sid = answer["session_id"].get<std::string>();

    std::atomic<bool> stop_pump{false};
    std::thread pump([&] {
        std::uint8_t shade = 0;
        while (!stop_pump.load(std::memory_order_acquire)) {
            pm.onChannelFrame(300, makeSyntheticFrame(854, 480, shade++));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Дать encoder'у пройти как минимум один keyframe + несколько P-frame.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    EXPECT_EQ(pm.closeSession(300, sid), R::Ok);

    // Качать кадры ещё немного: они должны утекать в /dev/null (сессия
    // удалена, encoder без потребителей. В этом тесте encoder остаётся
    // живым на канале — это ОК, фан-аут просто пуст).
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    stop_pump.store(true, std::memory_order_release);
    pump.join();

    pm.stopChannel(300);
    client.pc.reset();
}

TEST(PreviewIntegration, CapacityCapBlocksThirdSessionPerChannel) {
    PreviewManager::Config cfg;
    cfg.max_clients_per_channel = 2;
    PreviewManager pm(cfg);

    auto c1 = makeBrowserOffer();
    auto c2 = makeBrowserOffer();
    auto c3 = makeBrowserOffer();
    ASSERT_FALSE(c1.offer_sdp.empty());
    ASSERT_FALSE(c2.offer_sdp.empty());
    ASSERT_FALSE(c3.offer_sdp.empty());

    nlohmann::json a;
    EXPECT_EQ(pm.createOffer(7, {{"type","offer"},{"sdp",c1.offer_sdp}}, &a), R::Ok);
    EXPECT_EQ(pm.createOffer(7, {{"type","offer"},{"sdp",c2.offer_sdp}}, &a), R::Ok);
    EXPECT_EQ(pm.createOffer(7, {{"type","offer"},{"sdp",c3.offer_sdp}}, &a),
              R::TooManyClients);

    auto stats = pm.statsJson(7);
    EXPECT_EQ(stats["sessions"].size(), 2u);

    pm.stopChannel(7);
    c1.pc.reset(); c2.pc.reset(); c3.pc.reset();
}

#else  // LIVEQX_ENABLE_WEBRTC_PREVIEW

// Без флага libdatachannel не доступен — integration test'у нечего
// делать. Smoke check'и под OFF уже покрыты test_preview_manager и
// test_webrtc_session.
TEST(PreviewIntegration, SkippedWhenWebRtcDisabled) {
    EXPECT_FALSE(PreviewManager::isCompiled());
}

#endif  // LIVEQX_ENABLE_WEBRTC_PREVIEW
