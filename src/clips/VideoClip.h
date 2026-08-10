#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <spdlog/logger.h>
#include <string>
#include <thread>
#include "clips/IClip.h"
#include "clips/IPacketReader.h"
#include "clips/PacketTypes.h"
#include "core/FramePool.h"
#include "metrics/ChannelMetrics.h"
#include "utils/SpscQueue.h"

struct AVPacket;

// Broadcast-model VideoClip: plays once from t=0 to EOF, then idles.
// Looping is handled at the Timeline level; reset() rewinds the clip back
// to t=0 (called by Preloader when the playlist wraps).
class VideoClip : public IClip {
public:
    // output_fps drives the dynamic pop timeout in decode loops.
    // reader — DI seam for tests; nullptr → default av_read_frame implementation.
    VideoClip(const std::string& path, int out_width, int out_height,
              std::shared_ptr<FramePool> pool = nullptr,
              int output_fps = 25,
              std::unique_ptr<IPacketReader> reader = nullptr);
    ~VideoClip();

    void setMetrics(std::shared_ptr<ChannelMetrics> m) { metrics_ = std::move(m); }
    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    void setLogger(std::shared_ptr<spdlog::logger> lg) { logger_ = std::move(lg); }
    // Channel id label used as thread-name prefix (e.g. "ch7-vdec").
    void setChannelId(std::string id) { channel_id_ = std::move(id); }
    std::string clipType() const noexcept override { return "video"; }
    // NUMA hint applied at the start of each decode loop via numa::bind_*.
    // -1 (default) leaves affinity to the OS. ClipFactory wires this from
    // PlaylistItem::numa_node so callers don't touch ctor signature.
    void setNumaNode(int node) noexcept { numa_node_ = node; }
    int  numaNode() const noexcept { return numa_node_; }

    Frame      getFrame()                 override;
    AudioFrame getAudio(int num_samples)  override;
    double     getDuration()  const override;
    bool       hasAudio()     const override;
    bool       isPrepared()   const override;
    void       prepare()            override;
    void       release()            override;

    Frame      getTailFrame()                 override;
    AudioFrame getTailAudio(int num_samples)  override;
    void       reset()                        override;
    void       setFreezeTail(bool freeze)     override {
        freeze_tail_.store(freeze, std::memory_order_release);
    }
    void       setHeadBufferSeconds(double s) override { head_buffer_sec_ = s; }
    void       setHeadPlayback(bool on)       override;

private:
    void packetReaderLoop();
    void videoDecodeLoop();
    void audioDecodeLoop();
    spdlog::logger& lg() noexcept;

    // Pre-decode head_buffer_sec_ of frames+audio into head_video_ /
    // head_audio_samples_, then rewind decoder to t=0 so live consumers
    // start from the head as usual. No-op if head_buffer_sec_ <= 0.
    void captureHeadBuffer();

    // Spin up decoder + reader threads against the already-open impl_.
    // Used by both prepare() (after openContainer) and reset() (after seek).
    void startThreads();
    // Stop and join all threads; drain queues. impl_ stays alive.
    void stopThreads();
    // Block (with timeout) until the first decoded frame has been seeded
    // into tail_frame_ — guarantees getTailFrame() returns the head snapshot
    // for FreezeFade transitions.
    void waitForFirstFrameSeed();

    std::string path_;
    int         out_width_;
    int         out_height_;
    int         output_fps_;
    double      duration_sec_ = 0.0;
    bool        has_audio_    = false;

    SpscQueue<PacketOrSentinel, 32> video_pkt_queue_;
    SpscQueue<PacketOrSentinel, 64> audio_pkt_queue_;

    SpscQueue<Frame,       8> video_queue_;
    SpscQueue<AudioFrame, 64> audio_queue_;

    std::deque<float> audio_sample_buf_;

    std::atomic<bool> decoding_{false};

    // Set to true by video/audioDecodeLoop after the FLUSH-drain following
    // reader EOF. Tells getFrame()/getAudio() to short-circuit their CV waits
    // instead of stalling on samples that will never come — without this the
    // render loop slows to ~7fps (audio CV burns 100ms/frame) once a clip
    // finishes its content, so slot_pos lags wall time and the cursor never
    // reaches the next slot's transition window. Cleared in startThreads().
    std::atomic<bool> video_exhausted_{false};
    std::atomic<bool> audio_exhausted_{false};

    std::jthread packet_reader_thread_;
    std::jthread video_decode_thread_;
    std::jthread audio_decode_thread_;

    // Back-pressure for packet queues.
    std::mutex              pkt_drain_mtx_;
    std::condition_variable pkt_drain_cv_;

    // Back-pressure for decoded video queue.
    std::mutex              video_push_mtx_;
    std::condition_variable video_push_cv_;

    // Consumer notification for getFrame()/getAudio().
    std::mutex              frame_cv_mtx_;
    std::condition_variable frame_cv_;
    std::mutex              audio_cv_mtx_;
    std::condition_variable audio_cv_;

    std::shared_ptr<FramePool> pool_;

    // Last successfully served frame — used by getTailFrame() during
    // broadcast-model transition tail windows. Seeded with first decoded
    // frame in prepare() (head snapshot for FreezeFade).
    Frame              tail_frame_;
    std::mutex         tail_frame_mtx_;
    std::atomic<bool>  seeded_first_frame_{false};
    // When true, getFrame()/decode-loop seeding must NOT overwrite tail_frame_.
    // Set by Preloader before reset() in a single-clip transition loop so the
    // last-good frame stays visible as clipA's frozen tail while clipB (the
    // same instance, freshly reset) decodes from t=0.
    std::atomic<bool>  freeze_tail_{false};

    // Pre-decoded head buffer, captured once during prepare(). Lets the
    // single-clip self-loop serve clipB (head frames) without disrupting the
    // live decoder, which is reset() in parallel for the next cycle.
    double                  head_buffer_sec_ = 0.0;
    std::vector<Frame>      head_video_;
    std::vector<float>      head_audio_samples_;  // interleaved stereo, kOutRate
    std::atomic<bool>       head_playback_{false};
    std::atomic<size_t>     head_video_idx_{0};
    std::atomic<size_t>     head_audio_idx_{0};
    // True iff head_playback_ was switched on at least once since the last
    // reset(). Only then has the user *seen* the head frames via head_video_,
    // and only then must reset() skip those same frames in the live queue to
    // avoid duplication. For non-self-loop transitions head_playback_ is
    // never engaged, so head_video_ is captured-but-unused and the live
    // queue must serve the entire clip from frame 0.
    std::atomic<bool>       head_was_played_{false};

    // Set in reset() when the previous cycle consumed the head buffer (see
    // head_was_played_). The decoder, on resume from t=0, drops exactly the
    // frames already shown via head_video_/head_audio_samples_ — otherwise
    // the live queue would replay them right after the self-loop transition
    // (visible as "clip restarts").
    std::atomic<size_t>     skip_video_frames_{0};
    std::atomic<size_t>     skip_audio_floats_{0};
    std::condition_variable seed_cv_;
    std::mutex              seed_mtx_;

    // Single-slot pending packets for back-pressure (reader-side stash
    // when a queue is full).
    PacketOrSentinel pending_video_{};
    PacketOrSentinel pending_audio_{};
    bool             has_pending_video_ = false;
    bool             has_pending_audio_ = false;

    std::unique_ptr<IPacketReader> packet_reader_;
    std::shared_ptr<ChannelMetrics> metrics_;

    // NUMA node hint; -1 = no pinning. Read by each of the three decode
    // loops on entry via numa::bindCurrentThreadToNode.
    int numa_node_ = -1;
    std::shared_ptr<spdlog::logger> logger_;
    std::string channel_id_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
