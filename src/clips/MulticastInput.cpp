#include "clips/MulticastInput.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

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

using namespace liveqx::multicast;

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

// Maximum jitter-buffered audio we hold before dropping the oldest. 1 second
// of stereo float at 48 kHz = ~48 000 samples. Live render should not lag
// behind the source by more than this — if it does, we have a worse problem
// than dropped audio.
constexpr int kJitterMaxFloats = 48000 * 2;

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct MulticastInput::Impl {
    FmtCtxPtr   fmt;
    CodecCtxPtr video_ctx;
    CodecCtxPtr audio_ctx;
    SwsCtxPtr   sws;
    SwrCtxPtr   swr;
    int         video_idx = -1;
    int         audio_idx = -1;

    std::mutex  open_mtx;   // guards open/close transitions across threads
};

// ─── Construction / destruction ──────────────────────────────────────────────

MulticastInput::MulticastInput(InputCfg cfg, int out_width, int out_height)
    : cfg_(std::move(cfg)),
      out_width_(out_width),
      out_height_(out_height),
      impl_(std::make_unique<Impl>()) {}

MulticastInput::~MulticastInput() { release(); }

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void MulticastInput::prepare() {
    if (prepared_.exchange(true)) return;

    if (logger_) {
        logger_->info("MulticastInput: prepare() udp://{}:{} iface='{}' jitter={}ms",
                      cfg_.address, cfg_.port,
                      cfg_.interface_addr.empty() ? "default" : cfg_.interface_addr,
                      cfg_.jitter_buffer_ms);
    }

    // Don't fail prepare() if the multicast group is currently silent — the
    // watchdog will keep retrying. We *do* fail if the FFmpeg URL itself is
    // malformed (caught in openContext via avformat_open_input).
    if (!openContext()) {
        if (logger_) logger_->warn("MulticastInput: initial open failed, "
                                   "watchdog will retry");
    }
    last_packet_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);

    decode_thread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });
    if (cfg_.reconnect_on_silence_sec > 0) {
        watchdog_thread_ = std::jthread([this](std::stop_token st) { watchdogLoop(st); });
    }
}

void MulticastInput::release() {
    if (!prepared_.exchange(false)) return;

    if (decode_thread_.joinable())   { decode_thread_.request_stop();   decode_thread_.join(); }
    if (watchdog_thread_.joinable()) { watchdog_thread_.request_stop(); watchdog_thread_.join(); }

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
    if (logger_) logger_->info("MulticastInput: released");
}

// ─── FFmpeg open / close ─────────────────────────────────────────────────────

bool MulticastInput::openContext() {
    std::lock_guard<std::mutex> lk(impl_->open_mtx);

    if (impl_->fmt) return true;  // already open

    const std::string url = buildFfmpegUrl(cfg_);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    // 'overrun_nonfatal' — keeps demux alive across kernel UDP overruns
    // instead of returning AVERROR_EOF on a single drop.
    av_dict_set(&opts, "overrun_nonfatal", "1", 0);

    AVFormatContext* raw = nullptr;
    int rc = avformat_open_input(&raw, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        if (logger_) logger_->warn("MulticastInput: avformat_open_input({}) failed: rc={}",
                                   url, rc);
        return false;
    }
    impl_->fmt.reset(raw);

    // Stream info read can take time on a slow multicast feed (probesize
    // = ~5MB by default). Bound it to 2 seconds so prepare() returns
    // promptly; the decoder will still work on whatever streams it found.
    impl_->fmt->probesize    = 1 << 20;
    impl_->fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(impl_->fmt.get(), nullptr) < 0) {
        if (logger_) logger_->warn("MulticastInput: find_stream_info failed for {}", url);
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
        if (logger_) logger_->error("MulticastInput: no video stream in {}", url);
        impl_->fmt.reset();
        return false;
    }

    // Video decoder + scaler.
    {
        const AVStream* st  = impl_->fmt->streams[impl_->video_idx];
        const auto*     par = st->codecpar;
        const AVCodec*  codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            if (logger_) logger_->error("MulticastInput: unsupported video codec");
            impl_->fmt.reset();
            return false;
        }
        CodecCtxPtr ctx{avcodec_alloc_context3(codec)};
        if (!ctx
                || avcodec_parameters_to_context(ctx.get(), par) < 0
                || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
            if (logger_) logger_->error("MulticastInput: video decoder open failed");
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
            if (logger_) logger_->error("MulticastInput: sws_getContext failed");
            impl_->fmt.reset();
            return false;
        }
        impl_->video_ctx = std::move(ctx);
        impl_->sws       = std::move(sws);
    }

    // Audio decoder + resampler (best-effort — feeds without audio still play).
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
    return true;
}

void MulticastInput::closeContext() {
    std::lock_guard<std::mutex> lk(impl_->open_mtx);
    impl_->sws.reset();
    impl_->swr.reset();
    impl_->video_ctx.reset();
    impl_->audio_ctx.reset();
    impl_->fmt.reset();
    impl_->video_idx = -1;
    impl_->audio_idx = -1;
}

// ─── Decode loop ─────────────────────────────────────────────────────────────

void MulticastInput::decodeLoop(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-mc-rx");

    AvPacketPtr pkt{av_packet_alloc()};
    AvFramePtr  frm{av_frame_alloc()};

    while (!st.stop_requested()) {
        // Without an open context (initial open failed, or watchdog tore it
        // down) just idle — watchdog handles re-open.
        if (!impl_->fmt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const int rc = av_read_frame(impl_->fmt.get(), pkt.get());
        if (rc < 0) {
            // EOF/timeout/EAGAIN — under multicast normally only happens when
            // the source disappears. Sleep briefly; watchdog decides on
            // reconnect based on cumulative silence.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
                        // Bound jitter buffer — drop oldest on overflow.
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

// ─── Watchdog ────────────────────────────────────────────────────────────────

void MulticastInput::watchdogLoop(std::stop_token st) {
    using namespace std::chrono;
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-mc-wd");

    const auto silence_threshold = seconds(cfg_.reconnect_on_silence_sec);
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(milliseconds(500));
        if (st.stop_requested()) break;

        const auto now_ns = duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
        const auto last_ns = last_packet_ns_.load(std::memory_order_relaxed);
        const auto gap_ns  = now_ns - last_ns;

        if (gap_ns > duration_cast<nanoseconds>(silence_threshold).count()) {
            stalled_.store(true, std::memory_order_release);
            if (logger_) logger_->warn("MulticastInput: silence > {}s — re-joining {}:{}",
                                       cfg_.reconnect_on_silence_sec,
                                       cfg_.address, cfg_.port);
            closeContext();
            const bool ok = openContext();
            if (ok) {
                last_packet_ns_.store(now_ns, std::memory_order_relaxed);
                reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                if (logger_) logger_->info("MulticastInput: re-join succeeded");
            } else if (logger_) {
                logger_->warn("MulticastInput: re-join failed, will retry");
            }
        }
    }
}

// ─── Read accessors ──────────────────────────────────────────────────────────

Frame MulticastInput::getFrame() {
    std::lock_guard<std::mutex> lk(last_frame_mtx_);
    return last_frame_;   // shared_ptr copy — no allocation
}

AudioFrame MulticastInput::getAudio(int num_samples) {
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
    // Trailing samples remain zero (silence) on underflow.
    return out;
}

// ─── Diagnostics ─────────────────────────────────────────────────────────────

nlohmann::json MulticastInput::statusJson() const {
    nlohmann::json j;
    j["type"]              = "multicast";
    j["address"]           = cfg_.address;
    j["port"]              = cfg_.port;
    j["interface"]         = cfg_.interface_addr;
    j["container"]         = cfg_.container;
    j["jitter_buffer_ms"]  = cfg_.jitter_buffer_ms;
    j["packets_recv"]      = packets_recv_.load(std::memory_order_relaxed);
    j["reconnect_count"]   = reconnect_count_.load(std::memory_order_relaxed);
    j["stalled"]           = stalled_.load(std::memory_order_acquire);
    j["has_audio"]         = has_audio_.load(std::memory_order_acquire);
    j["prepared"]          = prepared_.load(std::memory_order_acquire);
    return j;
}
