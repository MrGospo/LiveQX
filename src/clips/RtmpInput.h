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
#include "clips/RtmpInputCfg.h"
#include "core/AudioFrame.h"
#include "core/Frame.h"

// Live input for RTMP sources (fix11 step 1: client-mode pull from an
// external publisher; step 2 will add server-mode listen). Mirrors
// MulticastInput's structure: a single decode jthread pumps
// av_read_frame → video/audio decoders → tail frame + audio jitter
// buffer; on connect failure or stream loss we sleep with exponential
// backoff and re-open. Unlike multicast, RTMP runs over TCP so a
// disconnect is a hard event — there's no "join group and wait", we
// have to reconnect explicitly.
class RtmpInput : public LiveInputClip {
public:
    RtmpInput(liveqx::rtmp::InputCfg cfg,
              int out_width, int out_height);
    ~RtmpInput() override;

    Frame      getFrame() override;
    AudioFrame getAudio(int num_samples) override;
    bool       hasAudio() const override   { return has_audio_.load(std::memory_order_acquire); }
    bool       isPrepared() const override { return prepared_.load(std::memory_order_acquire); }
    void       prepare() override;
    void       release() override;

    // Diagnostics for /channels/{id}/status. Sanitizes the URL so the
    // stream key embedded in the path never reaches a JSON consumer.
    nlohmann::json statusJson() const override;

    // ---- fix13 c1: ILiveInput surface ----
    // Healthy iff prepared, TCP connected, and not stalled. Multicast
    // can survive without a peer (UDP is connectionless), but RTMP cannot —
    // a TCP disconnect immediately makes the stream unhealthy until the
    // backoff loop re-establishes the session.
    bool         isHealthy() const override {
        return prepared_.load(std::memory_order_acquire)
            && connected_.load(std::memory_order_acquire)
            && !stalled_.load(std::memory_order_acquire);
    }
    std::int64_t lastPacketNs() const noexcept override {
        return last_packet_ns_.load(std::memory_order_acquire);
    }

    // NUMA hint applied to the decode thread (-1 = OS scheduler).
    void setNumaNode(int node) noexcept override { numa_node_ = node; }

private:
    void decodeLoop(std::stop_token st);

    bool openContext();    // returns false on failure (logged, sanitized)
    void closeContext();

    // Set true on release(); read by FFmpeg's interrupt_callback so a
    // blocked avformat_open_input (server-mode listen) or av_read_frame
    // returns AVERROR_EXIT promptly instead of hanging until the publisher
    // appears or TCP times out.
    std::atomic<bool> stop_io_ { false };

    // Computes the next backoff duration given the current step. Encapsulated
    // for the unit test that pins backoff growth without spinning real I/O.
    std::chrono::milliseconds nextBackoff();

    liveqx::rtmp::InputCfg cfg_;
    const int out_width_;
    const int out_height_;
    int       numa_node_ = -1;

    // Hidden FFmpeg state — this header stays free of <libav*>.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> prepared_  { false };
    std::atomic<bool> has_audio_ { false };
    std::atomic<bool> connected_ { false };

    // Audio jitter buffer. Producer: decode thread, consumer: render loop
    // via getAudio(). Bounded — drops oldest on overflow to avoid drift.
    std::mutex            audio_mtx_;
    std::deque<float>     audio_buf_;
    static constexpr int  kOutRate = 48000;
    static constexpr int  kOutCh   = 2;

    // Last decoded frame; updated under last_frame_mtx_ so concurrent
    // tail accessors see a consistent shared_ptr swap.
    mutable std::mutex    last_frame_mtx_;

    // Watchdog/backoff bookkeeping. last_packet_ns_ feeds statusJson; the
    // backoff state lives on the decode thread and is reset to
    // reconnect_initial_ms on each successful open.
    std::atomic<int64_t>  last_packet_ns_  { 0 };
    int                   current_backoff_ms_ = 0;

    // Counters surfaced via statusJson().
    std::atomic<uint64_t> packets_recv_    { 0 };
    std::atomic<uint64_t> reconnect_count_ { 0 };
    std::atomic<bool>     stalled_         { false };

    std::jthread decode_thread_;
};
