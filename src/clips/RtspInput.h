#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "clips/LiveInputClip.h"
#include "clips/RtspInputCfg.h"
#include "core/AudioFrame.h"
#include "core/Frame.h"

// Live input for RTSP cameras / pro broadcast equipment (fix15). Mirrors
// RtmpInput's pimpl shape:
//   - cfg + Impl (FFmpeg state) live behind a unique_ptr so this header
//     stays free of <libav*>;
//   - the decode jthread is owned by the public class so release() can
//     join it deterministically;
//   - prepare/release are idempotent (atomic prepared_ flag).
//
// c2 wires the real avformat_open_input path with rtsp_transport (tcp|udp)
// and pumps packets through video/audio decoders + sws/swr into the same
// last_frame_ + audio_buf_ pattern RtmpInput uses. Codec validation,
// credential sanitization, exponential backoff, loss detection, TLS, and
// camera quirks ride in c3..c8.
class RtspInput : public LiveInputClip {
public:
    RtspInput(liveqx::rtsp::InputCfg cfg,
              int out_width, int out_height);
    ~RtspInput() override;

    Frame      getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    bool       hasAudio() const override   { return has_audio_.load(std::memory_order_acquire); }
    bool       isPrepared() const override { return prepared_.load(std::memory_order_acquire); }
    void       prepare() override;
    void       release() override;

    // Diagnostics surfaced via REST /live-status. c2 adds connected /
    // stalled — URL still absent (c4 lands the rtsp-shape sanitizer).
    nlohmann::json statusJson() const override;

    // ── ILiveInput surface (fix13 c1) ──
    // RTSP can silently stall on UDP transport: the session stays open,
    // RTCP keeps flowing, but video RTP packets never arrive (camera
    // hung, half-network-failure). We can't rely on av_read_frame
    // returning an error in that case — the rw_timeout takes ~5s to
    // trip — so isHealthy() also fails fast on packet age > 2s.
    // LiveClip (fix13) sees this and flips into Lost state immediately.
    // When a packet finally lands, last_packet_ns_ is bumped on the
    // decode thread → isHealthy flips back to true on the next poll.
    static constexpr std::int64_t kLossThresholdNs = 2'000'000'000;  // 2 s
    bool         isHealthy() const override {
        if (!prepared_.load(std::memory_order_acquire)) return false;
        if (!connected_.load(std::memory_order_acquire)) return false;
        if (stalled_.load(std::memory_order_acquire)) return false;
        const auto last = last_packet_ns_.load(std::memory_order_acquire);
        if (last == 0) return false;   // not received the first packet yet
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return (now - last) <= kLossThresholdNs;
    }
    std::int64_t lastPacketNs() const noexcept override {
        return last_packet_ns_.load(std::memory_order_acquire);
    }

    void setNumaNode(int node) noexcept override { numa_node_ = node; }

private:
    void decodeLoop(std::stop_token st);
    bool openContext();    // false = open failed (logged); caller backs off.
    void closeContext();

    // Returns the next wait duration on a failed open and advances the
    // internal ladder (geometric x2, capped at cfg_.reconnect_max_backoff_sec).
    // Reset to 1s on each successful connect. Encapsulated so the c9
    // unit test can pin the curve without spinning real I/O.
    std::chrono::milliseconds nextBackoff();

    // Tripped on release() — read by FFmpeg's interrupt_callback so a
    // blocked avformat_open_input or av_read_frame returns AVERROR_EXIT
    // promptly instead of hanging until the camera ACKs.
    std::atomic<bool> stop_io_ { false };

    liveqx::rtsp::InputCfg cfg_;
    const int out_width_;
    const int out_height_;
    int       numa_node_ = -1;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool>    prepared_   { false };
    std::atomic<bool>    has_audio_  { false };
    std::atomic<bool>    connected_  { false };
    std::atomic<bool>    stalled_    { false };
    std::atomic<int64_t> last_packet_ns_ { 0 };

    // Audio jitter buffer. Bounded — drops oldest on overflow to keep
    // the live source from drifting if the consumer pauses (matches
    // RtmpInput's policy).
    std::mutex            audio_mtx_;
    std::deque<float>     audio_buf_;
    static constexpr int  kOutRate = 48000;
    static constexpr int  kOutCh   = 2;

    // Last decoded frame; updated under last_frame_mtx_ on the decode
    // thread, read on the render thread via getFrame().
    mutable std::mutex    last_frame_mtx_;

    // Counters surfaced via statusJson().
    std::atomic<uint64_t> packets_recv_    { 0 };
    std::atomic<uint64_t> bytes_recv_      { 0 };
    std::atomic<uint64_t> reconnect_count_ { 0 };

    // Backoff ladder state. Touched only on the decode thread.
    int current_backoff_ms_ = 1000;
    static constexpr int kInitialBackoffMs = 1000;

    std::jthread decode_thread_;
};
