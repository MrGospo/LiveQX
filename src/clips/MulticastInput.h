#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "clips/LiveInputClip.h"
#include "clips/MulticastInputCfg.h"
#include "core/Frame.h"
#include "core/AudioFrame.h"

// Live input for multicast UDP streams (MPEG-TS or RTP).
// Implementation strategy:
//   - FFmpeg's libavformat owns IGMP join + UDP receive + demux. We open
//     `udp://addr:port?fifo_size=...&localaddr=...` and let it deal with
//     kernel multicast plumbing. This is broadcast-tested code; we don't
//     reinvent it.
//   - One decode thread pumps av_read_frame → video/audio decoders →
//     last_frame_ + audio_buf_. NUMA-pinned to the channel's node.
//   - One watchdog thread monitors wall-clock since the last received
//     packet; on silence > reconnect_on_silence_sec it tears down and
//     re-opens (re-joins IGMP). Disabled when reconnect_on_silence_sec=0.
//
// Thread-safety: getFrame/getTailFrame/getAudio are called from the
// channel's render loop; the decode thread updates last_frame_ and
// audio_buf_ under last_frame_mtx_ / audio_mtx_.
class MulticastInput : public LiveInputClip {
public:
    MulticastInput(liveqx::multicast::InputCfg cfg,
                   int out_width, int out_height);
    ~MulticastInput() override;

    // Live IClip surface.
    Frame      getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    bool       hasAudio() const override   { return has_audio_.load(std::memory_order_acquire); }
    bool       isPrepared() const override { return prepared_.load(std::memory_order_acquire); }
    void       prepare() override;
    void       release() override;

    // Diagnostics for /channel/{id}/status.
    nlohmann::json statusJson() const override;

    // ---- fix13 c1: ILiveInput surface ----
    // Healthy iff prepared and not stalled (watchdog hasn't flagged a
    // silence window past reconnect_on_silence_sec).
    bool         isHealthy() const override {
        return prepared_.load(std::memory_order_acquire)
            && !stalled_.load(std::memory_order_acquire);
    }
    std::int64_t lastPacketNs() const noexcept override {
        return last_packet_ns_.load(std::memory_order_acquire);
    }

    // NUMA hint for the decode + watchdog threads (-1 = OS scheduler).
    void setNumaNode(int node) noexcept override { numa_node_ = node; }

private:
    void decodeLoop(std::stop_token st);
    void watchdogLoop(std::stop_token st);

    bool openContext();   // returns false on failure (logged)
    void closeContext();

    liveqx::multicast::InputCfg cfg_;
    const int out_width_;
    const int out_height_;
    int numa_node_ = -1;

    // Hidden FFmpeg state — keeps this header free of <libav*>.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> prepared_  { false };
    std::atomic<bool> has_audio_ { false };

    // Audio sample buffer. Producer = decode thread, consumer = render loop
    // via getAudio(). Bounded — drains kJitterMaxSamples ahead, drops oldest
    // on overflow to avoid drift.
    std::mutex            audio_mtx_;
    std::deque<float>     audio_buf_;
    static constexpr int  kOutRate = 48000;
    static constexpr int  kOutCh   = 2;

    // Tail frame protection. last_frame_ in the base class is updated under
    // this mutex so concurrent getFrame/getTailFrame readers see a consistent
    // shared_ptr swap rather than a torn write.
    mutable std::mutex    last_frame_mtx_;

    // Watchdog: monotonic time of the last successful av_read_frame.
    std::atomic<int64_t>  last_packet_ns_ { 0 };
    // Counters surfaced via statusJson().
    std::atomic<uint64_t> packets_recv_   { 0 };
    std::atomic<uint64_t> reconnect_count_{ 0 };
    std::atomic<bool>     stalled_        { false };

    std::jthread decode_thread_;
    std::jthread watchdog_thread_;
};
