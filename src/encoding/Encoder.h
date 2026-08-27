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
        // GOP length in frames. 0 = per-backend auto default (fps for
        // x264/NVENC/QSV/VAAPI, max(fps/2, 6) for mpeg2video). Positive
        // values are honored verbatim — no silent clamp. Broadcast/IPTV
        // usually picks 25..50, OTT/HLS 2×fps for seek granularity.
        int         gop_size       = 0;

        // H.264 stream constraint hints. Only H.264 backends
        // (x264/NVENC/QSV/VAAPI) read these — mpeg2video ignores them.
        //   h264_profile: "" (auto), "baseline", "main", "high",
        //     "high10", "high422", "high444".
        //   h264_level:   0 (auto) or integer 10..62 encoding X.Y as
        //     major*10+minor (3.1 → 31, 4.0 → 40, 5.1 → 51).
        // Setting these is critical for interop with strict hardware
        // decoders — hospitality set-tops often require Main@3.1 or
        // High@4.0 and will refuse anything else.
        std::string h264_profile   = "";
        int         h264_level     = 0;

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

        // Video codec selection. Orthogonal to encoder_mode.
        //   "h264"       — H.264 (default). Uses encoder_mode to pick
        //                  x264/nvenc/qsv/vaapi.
        //   "mpeg2video" — MPEG-2 Video. CPU-only in this build (any GPU
        //                  encoder_mode gets logged and downgraded). For
        //                  legacy hospitality set-tops that don't decode
        //                  H.264 reliably.
        std::string video_codec    = "h264";

        // Audio codec selection.
        //   "aac" — AAC-LC (default). Modern set-tops and OTT decode this.
        //   "mp2" — MPEG-1 Layer II. The DVB-standard audio codec for
        //           broadcast/IPTV; some hospitality middleware and older
        //           set-tops require it because they either lack an AAC
        //           license or map the PMT stream_type strictly.
        std::string audio_codec    = "aac";

        // Broadcast/IPTV middleware knobs. Middleware like Otrum and
        // hospitality set-tops (LG Pro:Centric etc.) identify the service
        // by these fields and are strict about PSI/SI shape. Defaults
        // match a typical single-service DVB-IP transport stream and are
        // backwards-compatible: leaving them alone reproduces the pre-fix
        // behaviour, except SDT is now always emitted (see mpegts_flags
        // in Encoder.cpp).
        std::string service_name        = "LiveQX Channel";
        std::string service_provider    = "LiveQX";
        int         service_id          = 1;   // program_number in PAT
        int         transport_stream_id = 1;
        int         original_network_id = 1;
        // 0 = VBR (default, pre-fix behaviour). >0 = constant mux rate in
        // bit/s with null-packet stuffing. For strict middleware set this
        // to video_bitrate + audio_bitrate + ~15 % headroom.
        int64_t     mux_rate            = 0;
        // 0 = FFmpeg defaults (SDT=500 ms, PAT/PMT=100 ms). Some strict
        // middleware prefers ~2000 / 40 ms.
        int         sdt_period_ms       = 0;
        int         pat_period_ms       = 0;
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
