#pragma once

// fix23 commit 6 — H.264 preview encoder.
//
// Tactical compositor → x264 → NAL unit emitter for the WebRTC preview
// pipeline. Lives independently from the main streaming Encoder: each
// active preview channel runs a single PreviewEncoder, fed by a render
// loop "tap" that hands shared_ptr<Frame> at the channel's target_fps.
//
// Output is Annex-B NAL units (start-code prefixed). PreviewSession
// converts these into RTP packets via libdatachannel's H264RtpPacketizer
// in commit 7.
//
// Threading: encode() runs on the channel's render thread (same one that
// feeds the main encoder). The callback fires on that same thread, so
// the consumer (PreviewManager / PreviewSession) must not block.
//
// Codec choice:
//   ultrafast preset, zerolatency tune  → keep CPU low and avoid B-frames
//   baseline profile                    → maximum browser compatibility
//   500 kbps CBR (configurable)         → matches the design doc default
//   IDR every 60 frames (~2.4s @ 25fps) → fast re-attach on PLI/keyframe
//                                          requests from the browser

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "core/Frame.h"

namespace liveqx::preview {

class PreviewEncoder {
public:
    struct Config {
        int width       = 854;     // 16:9 480p
        int height      = 480;
        int fps         = 25;
        int bitrate_bps = 500'000; // 500 kbps CBR
        int gop         = 60;      // IDR every N frames
    };

    // pts_us — frame PTS in microseconds (mirrors Frame::pts).
    // is_keyframe — true for the first NAL of an IDR access unit (callers
    //              packetizing for RTP use this to set the marker bit).
    // data is Annex-B (00 00 00 01-prefixed); a single callback may carry
    // multiple concatenated NAL units (one access unit).
    using NalCallback =
        std::function<void(const std::uint8_t* data, std::size_t size,
                           bool is_keyframe, std::int64_t pts_us)>;

    explicit PreviewEncoder(Config cfg);
    ~PreviewEncoder();

    PreviewEncoder(const PreviewEncoder&)            = delete;
    PreviewEncoder& operator=(const PreviewEncoder&) = delete;

    // One-shot lifecycle. start() opens the codec + scaler; stop() flushes
    // and tears down. Not re-entrant: a stopped encoder must be destructed
    // (or replaced) rather than restarted, mirroring the main Encoder.
    bool start(NalCallback cb);
    void stop();

    // Push an RGBA frame. The encoder downscales to (width, height) via
    // libswscale and emits zero or more NAL units via the callback. False
    // if start() was not called or encoder is in a failed state.
    bool encode(const Frame& src);

    // Запрос немедленного IDR на следующий encode(). Lock-free: вызывается
    // из сетевого треда libdatachannel (PLI handler) и из render-треда
    // (на Connected), флаг подхватывается в encode() и сбрасывается
    // exchange'ем — гонок нет.
    void forceKeyframe() noexcept {
        force_idr_.store(true, std::memory_order_release);
    }

    // Stats — read by /api/channels/{id}/preview/stats (commit 8).
    std::int64_t framesEncoded() const noexcept { return frames_encoded_; }
    std::int64_t bytesEmitted()  const noexcept { return bytes_emitted_;  }
    bool         running()       const noexcept { return running_;        }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Config            cfg_;
    NalCallback       cb_;
    bool              running_         = false;
    std::int64_t      frames_encoded_  = 0;
    std::int64_t      bytes_emitted_   = 0;
    std::atomic<bool> force_idr_{false};
};

}  // namespace liveqx::preview
