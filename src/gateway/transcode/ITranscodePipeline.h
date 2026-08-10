#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>

#include <nlohmann/json.hpp>

#include "gateway/ts/PesAssembler.h"

namespace liveqx::gateway::transcode {

// fix40 A6 step 9 — abstraction the TranscodeGateway uses to drive the
// codec-heavy decode/encode pipeline. The gateway owns PSI parsing, the input
// socket, output buffering, and the CC counters; the pipeline owns codec
// contexts, the loss FSM (freeze/silence/fallback), and PES↔ES packetisation.
//
// Why an interface: TranscodeGateway.cpp is built into both the unit-test
// target (no FFmpeg) and the integration-test target (FFmpeg-linked). The
// interface lets the unit-test build run pass-through with the pipeline_
// pointer left null, while production and itests inject a real
// FfmpegTranscodePipeline that depends on libavcodec/libswresample/libswscale.
//
// Threading: all methods are called from the gateway's IO thread. No internal
// locking is required.
class ITranscodePipeline {
public:
    // The gateway sets this before any feed*() call. Each TS packet handed to
    // the sink is exactly 188 bytes and already has the correct output PID
    // and continuity counter — the gateway just buffers and ships it.
    using TsPacketSink = std::function<void(std::span<const std::uint8_t>)>;

    virtual ~ITranscodePipeline() = default;

    // Called every time refreshInputBindings() picks up a new program. PMT
    // stream_type values: 0x1B/0x24/0x02 video, 0x0F/0x11 audio (see
    // ITU-T H.222.0 Table 2-34). 0 means "not present" — used when the input
    // is missing one of V/A streams. The pipeline must (re)open decoders to
    // match. Encoder configuration is fixed by TranscodeCfg and is opened
    // lazily on the first decoded frame.
    virtual void onInputStreamTypes(std::uint8_t video_stream_type,
                                    std::uint8_t audio_stream_type) = 0;

    // Feed an assembled video PES packet (one access unit when the upstream
    // is well-formed). The pipeline drives decode → re-encode → re-PES → TS
    // packetise and emits via the registered TsPacketSink.
    virtual void feedVideoPes(ts::PesPacket pes) = 0;

    // Feed an assembled audio PES packet. May contain one or more ADTS
    // frames; the pipeline parses out individual frames before decoding.
    virtual void feedAudioPes(ts::PesPacket pes) = 0;

    // Heartbeat from the IO thread. Drives the loss FSM:
    // freeze→silence→fallback transitions, retry-backoff timers, and any
    // continuous-cadence emit (freeze frame at 25 fps even when input is
    // starved). Called frequently — once per input datagram and once per
    // recv timeout — so implementations should be cheap when no work is due.
    virtual void tick(std::chrono::steady_clock::time_point now) = 0;

    // EOF / stop drain. Call once on shutdown. Drains decoder + encoder
    // residual frames through the sink before returning.
    virtual void flush() = 0;

    virtual void setOutputSink(TsPacketSink sink) = 0;

    // Snapshot of internal counters for the gateway's status JSON. Format
    // is implementation-defined but should at least include frames_in/out,
    // freeze/silence/fallback counters, and current loss-FSM state.
    virtual nlohmann::json statsJson() const = 0;
};

}  // namespace liveqx::gateway::transcode
