#pragma once
//
// LiveInputClip-совместимый driver для NDI 5.x receive. Тонкая обёртка над
// NDIlib_recv_create_v3 + recv_capture_v2:
//   • prepare() — open libndi (через NdiLib singleton), создать receiver,
//                 запустить decode-thread, который тащит видео/аудио через
//                 recv_capture_v2 в last_frame_ + audio_buf_.
//   • release() — request_stop, join, recv_destroy.
//   • getFrame()/getAudio() — render-thread accessors, отдают свежий
//                              кадр / pull семплов из jitter buffer (то же,
//                              что MulticastInput).
//
// NDI receive внутри сам гарантирует mDNS resolve по cfg_.source_name —
// если источника пока нет в сети, recv_capture_v2 возвращает frame_type_none
// и мы просто крутимся в ожидании. Watchdog опционально флагает stalled
// если silence > reconnect_on_silence_sec; при этом recv не пересоздаётся
// (NDI handles reconnect internally).
//
// Если libndi не загружается — prepare() оставляет prepared_=true, но
// receiver_=nullptr; channel остаётся жив, просто отдаёт пустой кадр и
// silence. Это сознательно: lack of libndi не должен валить плейлист.
//

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "clips/LiveInputClip.h"
#include "clips/NdiInputCfg.h"
#include "core/AudioFrame.h"
#include "core/Frame.h"
#include "output/NdiAbi.h"

namespace liveqx::ndi {

class NdiLib;

class NdiInput : public LiveInputClip {
public:
    NdiInput(InputCfg cfg, int out_width, int out_height);
    ~NdiInput() override;

    // IClip surface.
    Frame      getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    bool       hasAudio() const override   { return has_audio_.load(std::memory_order_acquire); }
    bool       isPrepared() const override { return prepared_.load(std::memory_order_acquire); }
    void       prepare() override;
    void       release() override;

    // Diagnostics.
    nlohmann::json statusJson() const override;

    // ---- LiveInputClip surface ----
    bool isHealthy() const override {
        return prepared_.load(std::memory_order_acquire)
            && receiver_ != nullptr
            && !stalled_.load(std::memory_order_acquire);
    }
    std::int64_t lastPacketNs() const noexcept override {
        return last_packet_ns_.load(std::memory_order_acquire);
    }
    void setNumaNode(int node) noexcept override { numa_node_ = node; }

private:
    void decodeLoop(std::stop_token st);
    void watchdogLoop(std::stop_token st);

    bool openReceiver();    // returns false on libndi/recv failure
    void closeReceiver();

    // Pulls a video frame_v2 returned by recv_capture_v2 into last_frame_.
    // Owns the BGRA→RGBA swizzle and (if needed) bilinear scale to
    // out_width × out_height.
    void ingestVideo(const abi::video_frame_v2_t& v);
    void ingestAudio(const abi::audio_frame_v2_t& a);

    InputCfg     cfg_;
    const int    out_width_;
    const int    out_height_;
    int          numa_node_ = -1;

    std::shared_ptr<NdiLib>   lib_;
    abi::recv_instance_t      receiver_ = nullptr;

    std::atomic<bool> prepared_  { false };
    std::atomic<bool> has_audio_ { false };

    static constexpr int kOutRate = 48000;
    static constexpr int kOutCh   = 2;
    static constexpr int kJitterMaxFloats = 48000 * 2;  // 1s stereo @ 48k

    std::mutex            audio_mtx_;
    std::deque<float>     audio_buf_;

    mutable std::mutex    last_frame_mtx_;

    std::atomic<std::int64_t> last_packet_ns_  { 0 };
    std::atomic<std::uint64_t> packets_recv_   { 0 };
    std::atomic<std::uint64_t> reconnect_count_{ 0 };
    std::atomic<bool>          stalled_        { false };
    std::atomic<int>           src_width_      { 0 };
    std::atomic<int>           src_height_     { 0 };

    std::jthread decode_thread_;
    std::jthread watchdog_thread_;
};

}  // namespace liveqx::ndi
