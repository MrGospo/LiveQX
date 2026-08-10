#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <spdlog/logger.h>
#include <string>
#include "core/AudioFrame.h"
#include "core/Frame.h"
#include "output/IOutput.h"

// H.264 + AAC → MPEG-TS encoder.
// Call open() once, push frames from a single thread, close() to flush.
class Encoder {
public:
    using PacketCallback   = std::function<void(const Packet&)>;
    // Pre-encode tap. Fires at the top of pushFrame() with the same Frame
    // that's about to be handed to libx264/NVENC. Multiple subscribers
    // (e.g. several NDI outputs) are supported. Order is registration-order.
    using RawFrameCallback = std::function<void(const Frame&)>;

    struct Config {
        int         width          = 1280;
        int         height         = 720;
        int         fps            = 25;
        int64_t     video_bitrate  = 4'000'000;
        int64_t     audio_bitrate  = 128'000;
        int         sample_rate    = 48000;
        std::string preset         = "medium";
        // 0 = no B-frames (the live-streaming default; lowest latency,
        // pairs with tune=zerolatency). >0 enables B-frames for better
        // compression at the cost of ~max_b_frames frames of decoder
        // latency. When non-zero, Encoder drops tune=zerolatency so libx264
        // doesn't silently override the count back to 0.
        int         max_b_frames   = 0;

        // fix29: pluggable video backend selection.
        //   "cpu" / "x264"       — libx264 (always available, default).
        //   "nvenc" / "qsv" /    — explicit GPU backend; channel fails to
        //   "vaapi"                start if that backend can't open.
        //   "auto" / ""          — try GPU backends in order, fall back to
        //                          x264 if none open.
        std::string encoder_mode   = "cpu";
        // GPU device index for the selected backend; ignored by CPU.
        // 0 = first GPU on the host, 1 = second, etc.
        int         gpu_index      = 0;
    };

    explicit Encoder(const Config& cfg);
    ~Encoder();

    // Opens codec contexts and the MPEG-TS muxer.
    // Returns true on success; pushFrame() is a no-op if open() was not called.
    bool open();
    void close();

    // Encode one video frame + accompanying audio.
    // Calls the registered callback for each resulting TS chunk.
    void pushFrame(const Frame& video, const AudioFrame& audio);

    void onPacket(PacketCallback cb);

    // Subscribes to the pre-encode tap. Cb is invoked from the render
    // thread (same thread as pushFrame). Cb must be cheap — it runs in
    // the encode hot path. Cb should hold a weak reference if the
    // subscriber can outlive the encoder lifecycle is shorter.
    void addRawFrameCallback(RawFrameCallback cb);
    void clearRawFrameCallbacks();

    // Forces the next encoded frame to be an IDR keyframe.
    // Thread-safe: safe to call from SrtOutput's I/O thread.
    void forceKeyframe() noexcept { force_idr_.store(true, std::memory_order_relaxed); }

    // Forces an IDR on the next pushFrame() and flushes the audio accumulator.
    // Call on SRT client reconnect. PTS stays monotonic — no muxer discontinuity.
    // Thread-safe: applied atomically inside pushFrame() on the render thread.
    void resetOnReconnect() noexcept;

    // Per-channel logger. If unset, falls back to spdlog::default_logger().
    void setLogger(std::shared_ptr<spdlog::logger> lg) { logger_ = std::move(lg); }

private:
    spdlog::logger& lg() noexcept;

    std::atomic<bool> force_idr_{false};
    std::atomic<bool> pts_reset_pending_{false};
    struct Impl;
    friend int avioWriteCallback(void* opaque, const uint8_t* buf, int size);
    std::unique_ptr<Impl> impl_;
    Config cfg_;
    std::shared_ptr<spdlog::logger> logger_;
};
