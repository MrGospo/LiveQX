#include "clips/RtmpInput.h"
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

using namespace liveqx::rtmp;

// ─── RAII wrappers ───────────────────────────────────────────────────────────

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

// FFmpeg interrupt callback. Runs on libavformat's blocking I/O (open,
// read, write); returning non-zero aborts the blocked call with
// AVERROR_EXIT. We pin it to the per-instance stop_io_ flag so release()
// can shortcut a server-mode listener that's still waiting for its first
// publisher.
int rtmpInterruptCallback(void* opaque) noexcept {
    auto* flag = reinterpret_cast<std::atomic<bool>*>(opaque);
    return (flag && flag->load(std::memory_order_acquire)) ? 1 : 0;
}

} // namespace

// The stream key rides in the URL path on every CDN ingest we target,
// so every log line and statusJson field goes through the shared helper
// in utils/UrlSanitize.h. Pulled into the file scope so RtmpOutput.cpp
// uses the same canonical name.
using liveqx::util::rtmpUrlForLogs;

// ─── Impl ────────────────────────────────────────────────────────────────────

struct RtmpInput::Impl {
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

RtmpInput::RtmpInput(InputCfg cfg, int out_width, int out_height)
    : cfg_(std::move(cfg)),
      out_width_(out_width),
      out_height_(out_height),
      impl_(std::make_unique<Impl>()),
      current_backoff_ms_(cfg_.reconnect_initial_ms) {}

RtmpInput::~RtmpInput() { release(); }

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void RtmpInput::prepare() {
    if (prepared_.exchange(true)) return;

    if (logger_) logger_->info("RtmpInput: prepare() mode={} {} buffer={}ms backoff {}..{}ms",
                               cfg_.mode, rtmpUrlForLogs(cfg_.url), cfg_.buffer_ms,
                               cfg_.reconnect_initial_ms, cfg_.reconnect_max_ms);

    // Client-mode: initial open is best-effort; the decode thread keeps
    // retrying with exponential backoff if the broker is unreachable.
    //
    // Server-mode MUST NOT run open here: avformat_open_input(listen=1)
    // blocks until the first publisher arrives, and prepare() is invoked
    // synchronously from the render thread's onTick() → LiveClip::prepare.
    // Hanging here would freeze the timeline. Defer the (blocking) open
    // entirely to the decode thread.
    if (cfg_.mode == "client") {
        if (!openContext() && logger_)
            logger_->warn("RtmpInput: initial connect failed; will retry with backoff");
    }

    last_packet_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);

    decode_thread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });
}

void RtmpInput::release() {
    if (!prepared_.exchange(false)) return;

    // Trip the FFmpeg interrupt before joining: a server-mode decoder may
    // still be blocked inside avformat_open_input waiting for the first
    // publisher, and request_stop alone doesn't reach into libav.
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
    has_audio_.store(false,  std::memory_order_release);
    connected_.store(false,  std::memory_order_release);
    stop_io_.store(false,    std::memory_order_release);    // re-armable
    if (logger_) logger_->info("RtmpInput: released");
}

// ─── Backoff ─────────────────────────────────────────────────────────────────

std::chrono::milliseconds RtmpInput::nextBackoff() {
    // Server-mode is fundamentally different: we listen for incoming
    // publishers, so a "reconnect" is just bind+accept again. There's no
    // upstream broker we'd be hammering, only our own kernel — a constant
    // small wait is correct (avoids spinning on a transient bind error).
    if (cfg_.mode == "server")
        return std::chrono::milliseconds(cfg_.reconnect_initial_ms);

    const int now_ms = current_backoff_ms_;
    // Geometric x2 with a soft cap; the cap is configurable so an operator can
    // pin "never wait more than 5s before retrying" on a critical channel.
    long long doubled = static_cast<long long>(now_ms) * 2;
    if (doubled > cfg_.reconnect_max_ms) doubled = cfg_.reconnect_max_ms;
    current_backoff_ms_ = static_cast<int>(doubled);
    return std::chrono::milliseconds(now_ms);
}

// ─── FFmpeg open / close ─────────────────────────────────────────────────────

bool RtmpInput::openContext() {
    std::lock_guard<std::mutex> lk(impl_->open_mtx);

    if (impl_->fmt) return true;

    const std::string url = buildFfmpegUrl(cfg_);

    AVDictionary* opts = nullptr;
    // TLS for rtmps:// inputs — pass through to FFmpeg's tls layer. Done
    // before mode-specific options so client/server share the same trust
    // config. No-op for plain rtmp:// URLs.
    if (cfg_.url.rfind("rtmps://", 0) == 0) {
        av_dict_set(&opts, "tls_verify", cfg_.tls_verify ? "1" : "0", 0);
        if (!cfg_.tls_ca_file.empty())
            av_dict_set(&opts, "ca_file", cfg_.tls_ca_file.c_str(), 0);
    }
    if (cfg_.mode == "client") {
        // rw_timeout is in microseconds; prevents the open call from hanging
        // forever when the host route exists but the broker silently drops us.
        av_dict_set(&opts, "rw_timeout", "5000000", 0);   // 5 s
    } else {
        // Server-mode listen MUST NOT use rw_timeout — we want to wait
        // indefinitely for a publisher, and the interrupt_callback below
        // is what actually breaks the wait on shutdown.
        //
        // The ?listen=1 query parameter on the URL is forwarded to the
        // remote server during the RTMP connect command rather than
        // configuring the local protocol layer (FFmpeg 6.1.1) — set the
        // option explicitly so avformat_open_input actually binds and
        // listens instead of trying to dial out.
        av_dict_set(&opts, "listen",         "1",  0);
        av_dict_set(&opts, "listen_timeout", "-1", 0);
    }
    if (cfg_.buffer_ms > 0) {
        const auto ms = std::to_string(cfg_.buffer_ms);
        av_dict_set(&opts, "rtmp_buffer", ms.c_str(), 0);
    }
    av_dict_set(&opts, "rtmp_live", "live", 0);       // not VOD

    // Pre-allocate so interrupt_callback is in place BEFORE the (potentially
    // blocking) open call. Without this, server-mode would hang inside
    // avformat_open_input until a publisher shows up — release() couldn't
    // tear it down on a stop request.
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) {
        av_dict_free(&opts);
        if (logger_) logger_->error("RtmpInput: avformat_alloc_context failed");
        return false;
    }
    raw->interrupt_callback.callback = &rtmpInterruptCallback;
    raw->interrupt_callback.opaque   = &stop_io_;

    int rc = avformat_open_input(&raw, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        // raw is freed by avformat_open_input on failure.
        if (logger_) logger_->warn("RtmpInput: {} {} failed (rc={})",
                                   cfg_.mode == "server" ? "listen on" : "connect to",
                                   rtmpUrlForLogs(cfg_.url), rc);
        return false;
    }
    impl_->fmt.reset(raw);

    impl_->fmt->probesize            = 1 << 20;
    impl_->fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(impl_->fmt.get(), nullptr) < 0) {
        if (logger_) logger_->warn("RtmpInput: find_stream_info failed for {}",
                                   rtmpUrlForLogs(cfg_.url));
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
        if (logger_) logger_->error("RtmpInput: no video stream from {}",
                                    rtmpUrlForLogs(cfg_.url));
        impl_->fmt.reset();
        return false;
    }

    {
        const AVStream* st  = impl_->fmt->streams[impl_->video_idx];
        const auto*     par = st->codecpar;
        const AVCodec*  codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            if (logger_) logger_->error("RtmpInput: unsupported video codec");
            impl_->fmt.reset();
            return false;
        }
        CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
        if (!ctx
                || avcodec_parameters_to_context(ctx.get(), par) < 0
                || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
            if (logger_) logger_->error("RtmpInput: video decoder open failed");
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
            if (logger_) logger_->error("RtmpInput: sws_getContext failed");
            impl_->fmt.reset();
            return false;
        }
        impl_->video_ctx = std::move(ctx);
        impl_->sws       = std::move(sws);
    }

    if (impl_->audio_idx >= 0) {
        const AVStream*          st  = impl_->fmt->streams[impl_->audio_idx];
        const AVCodecParameters* par = st->codecpar;
        const AVCodec*           codec = avcodec_find_decoder(par->codec_id);
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
    current_backoff_ms_ = cfg_.reconnect_initial_ms;   // reset ladder
    if (logger_) logger_->info("RtmpInput: {} {}",
                               cfg_.mode == "server" ? "publisher accepted on" : "connected to",
                               rtmpUrlForLogs(cfg_.url));
    return true;
}

void RtmpInput::closeContext() {
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

void RtmpInput::decodeLoop(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-rtmp-rx");

    AvPacketPtr pkt{av_packet_alloc()};
    AvFramePtr  frm{av_frame_alloc()};

    while (!st.stop_requested()) {
        if (!impl_->fmt) {
            // Either initial open failed or a previous read returned an
            // error — backoff, then try again. nextBackoff() advances the
            // ladder; openContext() resets it on success.
            stalled_.store(true, std::memory_order_release);
            const auto wait = nextBackoff();
            if (logger_) logger_->info("RtmpInput: reconnect in {}ms (host={})",
                                       wait.count(), rtmpUrlForLogs(cfg_.url));
            // Sleep in 100ms slices so stop_token can interrupt promptly.
            const auto deadline = std::chrono::steady_clock::now() + wait;
            while (!st.stop_requested()
                       && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (st.stop_requested()) break;

            if (openContext()) {
                reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                last_packet_ns_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_relaxed);
                stalled_.store(false, std::memory_order_release);
            }
            continue;
        }

        const int rc = av_read_frame(impl_->fmt.get(), pkt.get());
        if (rc < 0) {
            // TCP-side error or remote close. Tear down and let the loop
            // top take care of backoff + reopen.
            if (logger_) logger_->warn("RtmpInput: av_read_frame rc={} from {} — disconnecting",
                                       rc, rtmpUrlForLogs(cfg_.url));
            closeContext();
            continue;
        }
        last_packet_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        packets_recv_.fetch_add(1, std::memory_order_relaxed);
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

Frame RtmpInput::getFrame() {
    std::lock_guard<std::mutex> lk(last_frame_mtx_);
    return last_frame_;
}

AudioFrame RtmpInput::getAudio(int num_samples) {
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

nlohmann::json RtmpInput::statusJson() const {
    nlohmann::json j;
    j["type"]            = "rtmp";
    j["mode"]            = cfg_.mode;
    j["host"]            = rtmpUrlForLogs(cfg_.url);   // sanitized — no key in path
    j["buffer_ms"]       = cfg_.buffer_ms;
    j["packets_recv"]    = packets_recv_.load(std::memory_order_relaxed);
    j["reconnect_count"] = reconnect_count_.load(std::memory_order_relaxed);
    j["connected"]       = connected_.load(std::memory_order_acquire);
    j["stalled"]         = stalled_.load(std::memory_order_acquire);
    j["has_audio"]       = has_audio_.load(std::memory_order_acquire);
    j["prepared"]        = prepared_.load(std::memory_order_acquire);
    return j;
}
