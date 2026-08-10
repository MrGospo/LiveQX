#include "clips/RtspInput.h"

#include "utils/CpuAffinity.h"
#include "utils/Log.h"
#include "utils/UrlSanitize.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

using namespace liveqx::rtsp;
using liveqx::util::rtspUrlForLogs;

// ─── RAII wrappers (mirror RtmpInput) ────────────────────────────────────────

namespace {

struct FmtCtxDeleter   { void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter { void operator()(AVCodecContext*  p) const noexcept { if (p) avcodec_free_context(&p); } };
struct AvFrameDeleter  { void operator()(AVFrame*         p) const noexcept { if (p) av_frame_free(&p); } };
struct AvPacketDeleter { void operator()(AVPacket*        p) const noexcept { if (p) av_packet_free(&p); } };
struct SwsCtxDeleter   { void operator()(SwsContext*      p) const noexcept { if (p) sws_freeContext(p); } };
struct SwrCtxDeleter   { void operator()(SwrContext*      p) const noexcept { if (p) swr_free(&p); } };

using FmtCtxPtr   = std::unique_ptr<AVFormatContext, FmtCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using AvFramePtr  = std::unique_ptr<AVFrame,         AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket,        AvPacketDeleter>;
using SwsCtxPtr   = std::unique_ptr<SwsContext,      SwsCtxDeleter>;
using SwrCtxPtr   = std::unique_ptr<SwrContext,      SwrCtxDeleter>;

constexpr int kJitterMaxFloats = 48000 * 2;   // 1 s of stereo @ 48 kHz

// FFmpeg interrupt callback. Returning non-zero aborts a blocking I/O
// call (avformat_open_input or av_read_frame) with AVERROR_EXIT — that's
// how release() tears down the decode thread without waiting for a TCP
// timeout when the camera is unreachable.
int rtspInterruptCallback(void* opaque) noexcept {
    auto* flag = reinterpret_cast<std::atomic<bool>*>(opaque);
    return (flag && flag->load(std::memory_order_acquire)) ? 1 : 0;
}

// FFmpeg's RTSP demuxer doesn't expose username/password as AVOptions —
// they have to ride in the URL. We splice creds *only* at open time and
// never persist this back into cfg_ so the original URL stays clean for
// statusJson / reconnect (c4 will add the symmetric sanitizer for logs).
std::string buildRtspUrl(const InputCfg& cfg) {
    if (cfg.user.empty()) return cfg.url;
    // url has scheme:// prefix already (validated in parseInputCfg).
    const auto scheme_end = cfg.url.find("://");
    if (scheme_end == std::string::npos) return cfg.url;  // shouldn't happen
    const auto rest_off = scheme_end + 3;
    return cfg.url.substr(0, rest_off)
         + cfg.user + ":" + cfg.password + "@"
         + cfg.url.substr(rest_off);
}

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct RtspInput::Impl {
    FmtCtxPtr   fmt;
    CodecCtxPtr video_ctx;
    CodecCtxPtr audio_ctx;
    SwsCtxPtr   sws;
    SwrCtxPtr   swr;
    int         video_idx = -1;
    int         audio_idx = -1;

    std::mutex  open_mtx;
};

// ─── Construction / destruction ──────────────────────────────────────────────

RtspInput::RtspInput(InputCfg cfg, int out_width, int out_height)
    : cfg_(std::move(cfg)),
      out_width_(out_width),
      out_height_(out_height),
      impl_(std::make_unique<Impl>()) {}

RtspInput::~RtspInput() { release(); }

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void RtspInput::prepare() {
    if (prepared_.exchange(true)) return;

    if (logger_) logger_->info("RtspInput: prepare() transport={} url={}",
                               cfg_.transport, rtspUrlForLogs(cfg_.url));
    // Initial open is best-effort — the decode thread keeps trying with
    // a (currently fixed) backoff if the camera is offline. c5 turns
    // this into a proper exponential ladder.
    if (!openContext() && logger_)
        logger_->warn("RtspInput: initial connect failed; will retry");

    // Deliberately NOT initialising last_packet_ns_ to "now" here:
    // isHealthy() and statusJson() treat 0 as "no packet ever received"
    // and report unhealthy / -1 ms accordingly. Pre-stamping with now
    // would mask a never-connecting input as "healthy" for the loss
    // threshold window after prepare().

    decode_thread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });
}

std::chrono::milliseconds RtspInput::nextBackoff() {
    const int wait_ms = current_backoff_ms_;
    long long doubled = static_cast<long long>(wait_ms) * 2;
    const long long cap_ms = static_cast<long long>(cfg_.reconnect_max_backoff_sec) * 1000LL;
    if (doubled > cap_ms) doubled = cap_ms;
    current_backoff_ms_ = static_cast<int>(doubled);
    return std::chrono::milliseconds(wait_ms);
}

void RtspInput::release() {
    if (!prepared_.exchange(false)) return;

    // Trip interrupt before joining: a decoder blocked inside
    // avformat_open_input or av_read_frame will see this on its next
    // poll and exit with AVERROR_EXIT.
    stop_io_.store(true, std::memory_order_release);
    if (decode_thread_.joinable()) { decode_thread_.request_stop(); decode_thread_.join(); }

    closeContext();

    {
        std::lock_guard<std::mutex> lk(audio_mtx_);
        audio_buf_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(last_frame_mtx_);
        last_frame_ = Frame{};
    }
    has_audio_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    stalled_.store(false,   std::memory_order_release);
    stop_io_.store(false,   std::memory_order_release);   // re-armable
    last_packet_ns_.store(0, std::memory_order_release);  // reset "first packet" gate
    if (logger_) logger_->info("RtspInput: released");
}

// ─── FFmpeg open / close ─────────────────────────────────────────────────────

bool RtspInput::openContext() {
    std::lock_guard<std::mutex> lk(impl_->open_mtx);
    if (impl_->fmt) return true;

    const std::string url = buildRtspUrl(cfg_);

    AVDictionary* opts = nullptr;
    // The single most-important RTSP option: force TCP interleaving for
    // cameras (default — UDP is opt-in via cfg.transport). UDP is faster
    // on a clean LAN but loses packets on any congestion, and broadcast
    // pipelines can't tolerate that without a deep reorder buffer.
    av_dict_set(&opts, "rtsp_transport", cfg_.transport.c_str(), 0);
    // Bound the open and read calls so a flaky route can't hang the
    // decode thread forever; the interrupt_callback below covers
    // teardown on shutdown but timeouts are what catch silent drops.
    // Both timeouts are in microseconds; cfg has them in ms.
    const long long timeout_us = static_cast<long long>(cfg_.rw_timeout_ms) * 1000LL;
    av_dict_set_int(&opts, "stimeout",   timeout_us, 0);
    av_dict_set_int(&opts, "rw_timeout", timeout_us, 0);
    // UDP-only knob, no-op on TCP transport: how many out-of-order RTP
    // packets to hold before declaring loss. 2048 is the FFmpeg default
    // bumped up for noisy LANs; the cfg field exposes it because
    // wireless/WAN may need more.
    av_dict_set_int(&opts, "reorder_queue_size", cfg_.reorder_queue_size, 0);
    // user_agent: critical for some cameras that gate on it (Hikvision /
    // TP-Link Tapo refuse generic UAs and challenge-respond against a
    // VLC string). Default liveqx/1.0 is fine for most cameras;
    // operator overrides per-channel via cfg.
    av_dict_set(&opts, "user_agent", cfg_.user_agent.c_str(), 0);

    // TLS for rtsps:// — same approach as rtmps:// in RtmpInput. Options
    // are forwarded to FFmpeg's tls layer (openssl in our build);
    // tls_verify=false logs a one-line warning so it grep-able in prod.
    // No-op for plain rtsp://.
    if (cfg_.url.rfind("rtsps://", 0) == 0) {
        av_dict_set(&opts, "tls_verify", cfg_.tls_verify ? "1" : "0", 0);
        if (!cfg_.tls_ca_file.empty())
            av_dict_set(&opts, "ca_file", cfg_.tls_ca_file.c_str(), 0);
        if (!cfg_.tls_verify && logger_)
            logger_->warn("RtspInput: insecure rtsps — tls_verify=false ({})",
                          rtspUrlForLogs(cfg_.url));
    }

    // Pre-allocate so interrupt_callback is in place BEFORE the
    // (potentially blocking) open call.
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) {
        av_dict_free(&opts);
        if (logger_) logger_->error("RtspInput: avformat_alloc_context failed");
        return false;
    }
    raw->interrupt_callback.callback = &rtspInterruptCallback;
    raw->interrupt_callback.opaque   = &stop_io_;

    int rc = avformat_open_input(&raw, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        // raw is freed by avformat_open_input on failure.
        if (logger_) logger_->warn("RtspInput: connect failed (rc={}) url={}",
                                   rc, rtspUrlForLogs(cfg_.url));
        return false;
    }
    impl_->fmt.reset(raw);

    impl_->fmt->probesize            = 1 << 20;
    impl_->fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(impl_->fmt.get(), nullptr) < 0) {
        if (logger_) logger_->warn("RtspInput: find_stream_info failed url={}",
                                   rtspUrlForLogs(cfg_.url));
        impl_->fmt.reset();
        return false;
    }

    impl_->video_idx = -1;
    impl_->audio_idx = -1;
    for (unsigned i = 0; i < impl_->fmt->nb_streams; ++i) {
        const auto t = impl_->fmt->streams[i]->codecpar->codec_type;
        if (impl_->video_idx < 0 && t == AVMEDIA_TYPE_VIDEO) impl_->video_idx = static_cast<int>(i);
        if (impl_->audio_idx < 0 && t == AVMEDIA_TYPE_AUDIO) impl_->audio_idx = static_cast<int>(i);
    }
    if (impl_->video_idx < 0) {
        if (logger_) logger_->error("RtspInput: no video stream from {}",
                                    rtspUrlForLogs(cfg_.url));
        impl_->fmt.reset();
        return false;
    }

    {
        const AVStream* st  = impl_->fmt->streams[impl_->video_idx];
        const auto*     par = st->codecpar;
        // Production policy (fix/fix15-rtsp.md step 1): H.264 only. The
        // downstream pipeline (encoder, MPEG-TS muxer, HLS/RTMP/SRT
        // outputs) is x264-tuned; HEVC/AV1/MJPEG would force a transcode
        // we deliberately don't support here. Reject fast so the operator
        // sees an unhealthy input in /live-status instead of silent
        // mid-stream failures further down.
        if (par->codec_id != AV_CODEC_ID_H264) {
            if (logger_) logger_->error(
                "RtspInput: unsupported video codec id={} ({}) — H.264 required",
                static_cast<int>(par->codec_id),
                avcodec_get_name(par->codec_id));
            impl_->fmt.reset();
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            if (logger_) logger_->error("RtspInput: H.264 decoder not available in this build");
            impl_->fmt.reset();
            return false;
        }
        CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
        if (!ctx
                || avcodec_parameters_to_context(ctx.get(), par) < 0
                || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
            if (logger_) logger_->error("RtspInput: video decoder open failed");
            impl_->fmt.reset();
            return false;
        }
        const int src_w = par->width  > 0 ? par->width  : 16;
        const int src_h = par->height > 0 ? par->height : 16;
        const auto src_fmt = par->format >= 0
                                 ? static_cast<AVPixelFormat>(par->format)
                                 : AV_PIX_FMT_YUV420P;
        SwsCtxPtr sws{sws_getContext(src_w, src_h, src_fmt,
                                     out_width_, out_height_, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr)};
        if (!sws) {
            if (logger_) logger_->error("RtspInput: sws_getContext failed");
            impl_->fmt.reset();
            return false;
        }
        impl_->video_ctx = std::move(ctx);
        impl_->sws       = std::move(sws);
    }

    if (impl_->audio_idx >= 0) {
        const AVStream*          st  = impl_->fmt->streams[impl_->audio_idx];
        const AVCodecParameters* par = st->codecpar;
        // Same policy as video: AAC only. Cameras commonly stream G.711 /
        // G.726 / raw PCM — those would need a separate transcoder we're
        // not willing to wire into the live path (latency budget tight,
        // and the rest of the audio pipeline already speaks AAC). A
        // wrong-codec audio stream on a real camera is almost always a
        // misconfigured profile, so we fail the whole open instead of
        // dropping audio silently — silent audio drop on a "live news"
        // feed is operationally worse than a visibly unhealthy input.
        if (par->codec_id != AV_CODEC_ID_AAC) {
            if (logger_) logger_->error(
                "RtspInput: unsupported audio codec id={} ({}) — AAC required",
                static_cast<int>(par->codec_id),
                avcodec_get_name(par->codec_id));
            impl_->fmt.reset();
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (codec) {
            CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
            if (ctx
                    && avcodec_parameters_to_context(ctx.get(), par) >= 0
                    && avcodec_open2(ctx.get(), codec, nullptr) >= 0) {
                AVChannelLayout in_layout{};
                if (ctx->ch_layout.nb_channels > 0)
                    in_layout = ctx->ch_layout;
                else
                    av_channel_layout_default(&in_layout,
                        par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 1);

                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                SwrContext* swr_raw = nullptr;
                if (swr_alloc_set_opts2(&swr_raw,
                        &out_layout, AV_SAMPLE_FMT_FLT, kOutRate,
                        &in_layout,  ctx->sample_fmt,   ctx->sample_rate,
                        0, nullptr) >= 0
                        && swr_raw && swr_init(swr_raw) >= 0) {
                    impl_->swr.reset(swr_raw);
                    impl_->audio_ctx = std::move(ctx);
                    has_audio_.store(true, std::memory_order_release);
                } else if (swr_raw) {
                    swr_free(&swr_raw);
                }
            }
        }
    }

    connected_.store(true, std::memory_order_release);
    current_backoff_ms_ = kInitialBackoffMs;   // reset ladder on success
    if (logger_) logger_->info("RtspInput: connected to {}",
                               rtspUrlForLogs(cfg_.url));
    return true;
}

void RtspInput::closeContext() {
    std::lock_guard<std::mutex> lk(impl_->open_mtx);
    impl_->sws.reset();
    impl_->swr.reset();
    impl_->video_ctx.reset();
    impl_->audio_ctx.reset();
    impl_->fmt.reset();
    impl_->video_idx = -1;
    impl_->audio_idx = -1;
    connected_.store(false, std::memory_order_release);
}

// ─── Decode loop ─────────────────────────────────────────────────────────────

void RtspInput::decodeLoop(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-rtsp-rx");

    AvPacketPtr pkt{av_packet_alloc()};
    AvFramePtr  frm{av_frame_alloc()};

    while (!st.stop_requested()) {
        if (!impl_->fmt) {
            // Geometric x2 backoff up to cfg_.reconnect_max_backoff_sec.
            // openContext() resets the ladder on success. The sleep is
            // sliced so stop_token interrupts it within ~100 ms.
            stalled_.store(true, std::memory_order_release);
            const auto wait = nextBackoff();
            if (logger_) logger_->info("RtspInput: reconnect in {}ms (host={})",
                                       wait.count(), rtspUrlForLogs(cfg_.url));
            const auto deadline = std::chrono::steady_clock::now() + wait;
            while (!st.stop_requested()
                       && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (st.stop_requested()) break;

            if (openContext()) {
                reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                // Don't pre-stamp last_packet_ns_ here either — wait for
                // the first real av_read_frame so the loss-detector and
                // statusJson "age" gauge stay truthful.
                stalled_.store(false, std::memory_order_release);
            }
            continue;
        }

        const int rc = av_read_frame(impl_->fmt.get(), pkt.get());
        if (rc < 0) {
            if (logger_) logger_->warn("RtspInput: av_read_frame rc={} url={} — disconnecting",
                                       rc, rtspUrlForLogs(cfg_.url));
            closeContext();
            continue;
        }
        last_packet_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        packets_recv_.fetch_add(1, std::memory_order_relaxed);
        bytes_recv_.fetch_add(static_cast<uint64_t>(pkt->size > 0 ? pkt->size : 0),
                              std::memory_order_relaxed);
        stalled_.store(false, std::memory_order_release);

        if (pkt->stream_index == impl_->video_idx) {
            if (avcodec_send_packet(impl_->video_ctx.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(impl_->video_ctx.get(), frm.get()) >= 0) {
                    Frame f;
                    f.width  = out_width_;
                    f.height = out_height_;
                    f.data   = std::shared_ptr<uint8_t[]>(
                        new uint8_t[static_cast<size_t>(out_width_) * out_height_ * 4]());
                    uint8_t* dst[1] = { f.data.get() };
                    int      lin[1] = { out_width_ * 4 };
                    sws_scale(impl_->sws.get(), frm->data, frm->linesize,
                              0, frm->height, dst, lin);
                    if (frm->pts != AV_NOPTS_VALUE)
                        f.pts = frm->pts;
                    {
                        std::lock_guard<std::mutex> lk(last_frame_mtx_);
                        last_frame_ = std::move(f);
                    }
                    av_frame_unref(frm.get());
                }
            }
        } else if (impl_->audio_idx >= 0 && pkt->stream_index == impl_->audio_idx
                       && impl_->audio_ctx && impl_->swr) {
            if (avcodec_send_packet(impl_->audio_ctx.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(impl_->audio_ctx.get(), frm.get()) >= 0) {
                    const int max_out = swr_get_out_samples(impl_->swr.get(), frm->nb_samples);
                    std::vector<float> tmp(static_cast<size_t>(max_out) * kOutCh);
                    uint8_t* out_ptr[1] = { reinterpret_cast<uint8_t*>(tmp.data()) };
                    const int got = swr_convert(impl_->swr.get(),
                        out_ptr, max_out,
                        const_cast<const uint8_t**>(frm->data), frm->nb_samples);
                    if (got > 0) {
                        std::lock_guard<std::mutex> lk(audio_mtx_);
                        const size_t to_push = static_cast<size_t>(got) * kOutCh;
                        audio_buf_.insert(audio_buf_.end(),
                                          tmp.begin(), tmp.begin() + to_push);
                        if (audio_buf_.size() > static_cast<size_t>(kJitterMaxFloats)) {
                            const size_t drop = audio_buf_.size() - kJitterMaxFloats;
                            audio_buf_.erase(audio_buf_.begin(),
                                             audio_buf_.begin() + drop);
                        }
                    }
                    av_frame_unref(frm.get());
                }
            }
        }
        av_packet_unref(pkt.get());
    }
}

// ─── Read accessors ──────────────────────────────────────────────────────────

Frame RtspInput::getFrame() {
    std::lock_guard<std::mutex> lk(last_frame_mtx_);
    return last_frame_;
}

AudioFrame RtspInput::getAudio(int num_samples) {
    AudioFrame out;
    out.num_samples = num_samples;
    out.sample_rate = kOutRate;
    out.channels    = kOutCh;
    out.samples.resize(static_cast<size_t>(num_samples) * kOutCh, 0.0f);
    out.valid       = true;

    std::lock_guard<std::mutex> lk(audio_mtx_);
    const size_t want = out.samples.size();
    const size_t have = std::min(want, audio_buf_.size());
    for (size_t i = 0; i < have; ++i) out.samples[i] = audio_buf_[i];
    audio_buf_.erase(audio_buf_.begin(),
                     audio_buf_.begin() + static_cast<std::ptrdiff_t>(have));
    return out;
}

// ─── Diagnostics ─────────────────────────────────────────────────────────────

nlohmann::json RtspInput::statusJson() const {
    nlohmann::json j;
    j["type"]            = "rtsp";
    // Sanitized — userinfo masked, path kept (path identifies the
    // camera stream, not a secret).
    j["url"]             = rtspUrlForLogs(cfg_.url);
    j["transport"]       = cfg_.transport;
    j["prepared"]        = prepared_.load(std::memory_order_acquire);
    j["connected"]       = connected_.load(std::memory_order_acquire);
    j["stalled"]         = stalled_.load(std::memory_order_acquire);
    j["has_audio"]       = has_audio_.load(std::memory_order_acquire);
    j["packets_recv"]    = packets_recv_.load(std::memory_order_relaxed);
    j["bytes_recv"]      = bytes_recv_.load(std::memory_order_relaxed);
    j["reconnect_count"] = reconnect_count_.load(std::memory_order_relaxed);
    // last_packet_age_ms — observability for the loss-detector. -1 means
    // we haven't received the first packet yet (just opened or stuck in
    // backoff). LiveClip's Lost-state heuristic also uses this via
    // lastPacketNs(); surfacing here lets the operator see exactly how
    // far behind a stalled feed is from the threshold.
    const auto last = last_packet_ns_.load(std::memory_order_acquire);
    if (last == 0) {
        j["last_packet_age_ms"] = -1;
    } else {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        j["last_packet_age_ms"] = (now - last) / 1'000'000;
    }
    return j;
}
