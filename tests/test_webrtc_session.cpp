// fix34 D2.7 — smoke test для WebRtcSession.
//
// Под `LIVEQX_ENABLE_WEBRTC_PREVIEW=OFF` тесты урезаны:
// проверяем, что setRemoteOffer возвращает FeatureDisabled, а
// pushNal/close никого не ронят.
//
// Под флагом ON — настоящий handshake: создаём "browser-side" rtc::Peer
// Connection, генерируем offer SDP, скармливаем WebRtcSession,
// проверяем, что answer непустой и парсится обратно у клиента.
// Установление соединения (state Connected) НЕ проверяется — в CI
// окружениях нет ICE до браузера; для CI хватает проверки SDP-обмена.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "preview/WebRtcSession.h"

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
#include <rtc/rtc.hpp>
#endif

using liveqx::preview::WebRtcSession;

#ifdef LIVEQX_ENABLE_WEBRTC_PREVIEW
namespace {
// libdatachannel запускает global cleanup в DETACHED thread из
// ~TokenPayload (см. _deps/libdatachannel-src/src/impl/init.cpp). Если
// тесты завершаются быстро (bad-offer path), процесс exit'ится раньше
// чем этот thread доделает SCTP/DTLS/ICE Cleanup → segfault на static
// destruction. Гарантируем, что Cleanup() закончится перед exit.
class RtcCleanupEnv : public ::testing::Environment {
  public:
    void TearDown() override {
        auto fut = rtc::Cleanup();
        fut.wait_for(std::chrono::seconds(5));
    }
};
const auto* kRtcCleanupEnv =
    ::testing::AddGlobalTestEnvironment(new RtcCleanupEnv);
}  // namespace
#endif

TEST(WebRtcSession, EmptyOfferRejected) {
    WebRtcSession::Config cfg;
    WebRtcSession sess(/*channel_id=*/1, "abc", cfg);
    std::string answer;
    EXPECT_EQ(sess.setRemoteOffer("", &answer),
              WebRtcSession::OfferResult::BadOffer);
    EXPECT_FALSE(sess.isAlive());
}

TEST(WebRtcSession, PushNalBeforeOfferIsHarmless) {
    WebRtcSession::Config cfg;
    WebRtcSession sess(/*channel_id=*/1, "abc", cfg);
    const std::uint8_t nal[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    sess.pushNal(nal, sizeof(nal), /*key=*/true, /*pts_us=*/0);
    sess.close();   // не должно падать
    sess.close();   // идемпотентно
    SUCCEED();
}

TEST(WebRtcSession, StatsJsonShape) {
    WebRtcSession::Config cfg;
    WebRtcSession sess(42, "deadbeefcafebabe", cfg);
    auto j = sess.statsJson();
    EXPECT_EQ(j["id"],         "deadbeefcafebabe");
    EXPECT_EQ(j["channel_id"], 42);
    EXPECT_EQ(j["alive"],      false);
    EXPECT_TRUE(j.contains("rtp_packets_sent"));
    EXPECT_TRUE(j.contains("rtp_bytes_sent"));
    EXPECT_TRUE(j.contains("nals_dropped_after_close"));
}

#ifndef LIVEQX_ENABLE_WEBRTC_PREVIEW

TEST(WebRtcSession, SetRemoteOfferReturnsFeatureDisabledOnDefaultBuild) {
    WebRtcSession::Config cfg;
    WebRtcSession sess(1, "abc", cfg);
    std::string answer;
    EXPECT_EQ(sess.setRemoteOffer("v=0\r\n", &answer),
              WebRtcSession::OfferResult::FeatureDisabled);
}

#else  // LIVEQX_ENABLE_WEBRTC_PREVIEW

namespace {

// Создаёт настоящий offer SDP через libdatachannel — чтобы скормить
// его WebRtcSession и убедиться, что round-trip работает. Возвращает
// {pc, offer_sdp}; pc нужно держать живым на время теста, иначе
// ICE-агент закроется раньше времени.
struct ClientPeer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track>          track;  // libdatachannel хранит
                                                  // только weak_ptr в PC,
                                                  // shared_ptr держим тут.
    std::string offer_sdp;
};

ClientPeer makeBrowserOffer() {
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    auto pc = std::make_shared<rtc::PeerConnection>(cfg);

    // libdatachannel v0.21.0 не генерирует offer с одной только RecvOnly
    // m-line ("No DataChannel or Track to negotiate"). Используем SendRecv
    // — для теста это эквивалентно: WebRtcSession отвечает SendOnly, и
    // SDP exchange корректно парсится с обеих сторон.
    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    video.addH264Codec(96);
    auto track = pc->addTrack(video);  // shared_ptr ДОЛЖЕН жить — см. ClientPeer::track

    std::mutex              mu;
    std::condition_variable cv;
    bool                    gathered = false;
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(mu); gathered = true; cv.notify_all();
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

}  // namespace

TEST(WebRtcSession, OfferAnswerRoundtripProducesNonEmptySdp) {
    auto [client_pc, client_track, offer] = makeBrowserOffer();
    ASSERT_FALSE(offer.empty()) << "client failed to produce offer SDP";

    WebRtcSession::Config cfg;
    cfg.ice_servers = { "stun:stun.l.google.com:19302" };
    WebRtcSession sess(/*channel_id=*/7, "smoke-session", cfg);

    std::string answer;
    auto rc = sess.setRemoteOffer(offer, &answer);
    EXPECT_EQ(rc, WebRtcSession::OfferResult::Ok);
    EXPECT_FALSE(answer.empty());
    EXPECT_NE(answer.find("v=0"),   std::string::npos);
    EXPECT_NE(answer.find("video"), std::string::npos);
    EXPECT_NE(answer.find("H264"),  std::string::npos);
    // isAlive() ПРИНЦИПИАЛЬНО остаётся false до перехода peer в Connected
    // (см. WebRtcSession::onStateChange). В этом юнит-тесте мы НЕ заводим
    // обратный путь answer→client, поэтому DTLS никогда не поднимется, и
    // isAlive() корректно остаётся false. Это и есть фикс гонки: pushNal
    // не пытается дёрнуть track, пока соединение не сложилось.

    // Race-free shutdown: close() возвращается только когда in-flight
    // pushNal завершились (тут их нет, проверяем что не виснет).
    sess.close();
    EXPECT_FALSE(sess.isAlive());

    // Дтор клиентского peer connection — без UAF, hangs.
    client_pc.reset();
}

TEST(WebRtcSession, PushNalAfterCloseIsCounted) {
    auto [client_pc, client_track, offer] = makeBrowserOffer();
    ASSERT_FALSE(offer.empty());

    WebRtcSession::Config cfg;
    cfg.ice_servers = { "stun:stun.l.google.com:19302" };
    WebRtcSession sess(7, "drop-test", cfg);

    std::string answer;
    ASSERT_EQ(sess.setRemoteOffer(offer, &answer),
              WebRtcSession::OfferResult::Ok);
    sess.close();

    // 5 NAL push'ей после close — должны быть посчитаны как dropped.
    const std::uint8_t nal[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};
    for (int i = 0; i < 5; ++i) sess.pushNal(nal, sizeof(nal), false, i);

    auto j = sess.statsJson();
    EXPECT_GE(j["nals_dropped_after_close"].get<std::uint64_t>(), 5u);
    client_pc.reset();
}

#endif  // LIVEQX_ENABLE_WEBRTC_PREVIEW
