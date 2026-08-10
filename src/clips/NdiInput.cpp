#include "clips/NdiInput.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "output/NdiLib.h"
#include "utils/CpuAffinity.h"
#include "utils/Log.h"

namespace liveqx::ndi {

namespace {

// 100 ms recv_capture_v2 timeout — long enough that we don't burn a CPU
// core on an idle source, short enough that release() returns promptly
// (request_stop wakes us within this window at most).
constexpr std::uint32_t kCaptureTimeoutMs = 100;

}  // namespace

// ─── Construction / destruction ──────────────────────────────────────────────

NdiInput::NdiInput(InputCfg cfg, int out_width, int out_height)
    : cfg_(std::move(cfg)),
      out_width_(out_width),
      out_height_(out_height) {}

NdiInput::~NdiInput() { release(); }

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void NdiInput::prepare() {
    if (prepared_.exchange(true)) return;

    if (!logger_) logger_ = spdlog::default_logger();
    logger_->info("NdiInput: prepare() source='{}' (groups='{}', low_bw={})",
                  cfg_.source_name, cfg_.groups, cfg_.low_bandwidth);

    // Initial open is best-effort: an absent source is normal at boot
    // (NDI discovery via mDNS takes a moment). Decode loop keeps polling.
    if (!openReceiver()) {
        logger_->warn("NdiInput: initial open failed — channel will keep "
                      "polling but render falls back to last_frame_");
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

void NdiInput::release() {
    if (!prepared_.exchange(false)) return;

    if (decode_thread_.joinable())   { decode_thread_.request_stop();   decode_thread_.join(); }
    if (watchdog_thread_.joinable()) { watchdog_thread_.request_stop(); watchdog_thread_.join(); }

    closeReceiver();

    {
        std::lock_guard<std::mutex> lk(audio_mtx_);
        audio_buf_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(last_frame_mtx_);
        last_frame_ = Frame{};
    }
    has_audio_.store(false, std::memory_order_release);
    if (logger_) logger_->info("NdiInput: released ('{}')", cfg_.source_name);
}

// ─── libndi open/close ───────────────────────────────────────────────────────

bool NdiInput::openReceiver() {
    if (receiver_) return true;

    lib_ = NdiLib::getOrLoad(cfg_.lib_path_override, logger_);
    if (!lib_ || !lib_->ok() || !lib_->recv_create_v3) {
        if (logger_) logger_->warn("NdiInput: libndi unavailable: {}",
                                   lib_ ? lib_->lastError() : "no library handle");
        lib_.reset();
        return false;
    }

    abi::recv_create_v3_t r{};
    r.source_to_connect_to.p_ndi_name    = cfg_.source_name.c_str();
    r.source_to_connect_to.p_url_address = cfg_.url_address.empty()
                                           ? nullptr : cfg_.url_address.c_str();
    // BGRX_BGRA — receiver decodes to RGBA-with-alpha or BGRX (same byte
    // layout as our render Frame after the swizzle). Cheaper than UYVY
    // since libndi already does YUV→RGB in-engine, and lets us skip
    // libswscale color conversion (only spatial scale remains).
    r.color_format        = abi::recv_color_format_BGRX_BGRA;
    r.bandwidth           = cfg_.low_bandwidth ? abi::recv_bandwidth_lowest
                                               : abi::recv_bandwidth_highest;
    r.allow_video_fields  = false;
    r.p_ndi_recv_name     = cfg_.recv_name.c_str();

    receiver_ = lib_->recv_create_v3(&r);
    if (!receiver_) {
        if (logger_) logger_->error("NdiInput: NDIlib_recv_create_v3('{}') returned null",
                                    cfg_.source_name);
        lib_.reset();
        return false;
    }

    if (logger_) logger_->info("NdiInput: receiver created for '{}' (bw={})",
                               cfg_.source_name, r.bandwidth);
    return true;
}

void NdiInput::closeReceiver() {
    if (receiver_ && lib_ && lib_->recv_destroy) {
        lib_->recv_destroy(receiver_);
    }
    receiver_ = nullptr;
    // Drop our shared_ptr — the NdiLib singleton lives until process exit
    // anyway; this just relinquishes our hold so Output drivers can drop
    // theirs symmetrically.
    lib_.reset();
}

// ─── Decode loop ─────────────────────────────────────────────────────────────

void NdiInput::decodeLoop(std::stop_token st) {
    numa::bindCurrentThreadToNode(numa_node_);
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-ndi-rx");

    abi::video_frame_v2_t v{};
    abi::audio_frame_v2_t a{};

    while (!st.stop_requested()) {
        if (!receiver_ || !lib_ || !lib_->recv_capture_v2) {
            // Receiver missing — try to open. Backoff 500ms between attempts.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (st.stop_requested()) break;
            if (!openReceiver()) continue;
        }

        const int ft = lib_->recv_capture_v2(receiver_, &v, &a, nullptr,
                                              kCaptureTimeoutMs);
        switch (ft) {
            case abi::frame_type_video:
                last_packet_ns_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_relaxed);
                packets_recv_.fetch_add(1, std::memory_order_relaxed);
                stalled_.store(false, std::memory_order_release);
                if (v.p_data) ingestVideo(v);
                if (lib_->recv_free_video) lib_->recv_free_video(receiver_, &v);
                break;
            case abi::frame_type_audio:
                last_packet_ns_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_relaxed);
                packets_recv_.fetch_add(1, std::memory_order_relaxed);
                stalled_.store(false, std::memory_order_release);
                if (a.p_data) ingestAudio(a);
                if (lib_->recv_free_audio) lib_->recv_free_audio(receiver_, &a);
                break;
            case abi::frame_type_status_change:
                // Source resolution / format changed. NDI does the renegotiate
                // internally; we just clear has_audio_ so the next audio frame
                // re-asserts capability rather than a stale latch.
                has_audio_.store(false, std::memory_order_release);
                break;
            case abi::frame_type_error:
                if (logger_) logger_->warn("NdiInput: recv_capture_v2 returned error");
                break;
            case abi::frame_type_none:
            default:
                // No frame within timeout — just loop. Watchdog flags
                // stall if silence persists.
                break;
        }
    }
}

// ─── Watchdog ────────────────────────────────────────────────────────────────

void NdiInput::watchdogLoop(std::stop_token st) {
    using namespace std::chrono;
    Log::setThreadName("ch" + (channel_id_.empty() ? std::string("?") : channel_id_) + "-ndi-wd");

    const auto silence_threshold = seconds(cfg_.reconnect_on_silence_sec);
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(milliseconds(500));
        if (st.stop_requested()) break;

        const auto now_ns = duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
        const auto last_ns = last_packet_ns_.load(std::memory_order_relaxed);
        const auto gap_ns  = now_ns - last_ns;

        if (gap_ns > duration_cast<nanoseconds>(silence_threshold).count()) {
            if (!stalled_.exchange(true, std::memory_order_acq_rel)) {
                if (logger_) logger_->warn("NdiInput: silence > {}s on '{}' (NDI "
                                           "handles reconnect internally)",
                                           cfg_.reconnect_on_silence_sec,
                                           cfg_.source_name);
            }
        }
    }
}

// ─── Frame ingestion ─────────────────────────────────────────────────────────

void NdiInput::ingestVideo(const abi::video_frame_v2_t& v) {
    if (v.xres <= 0 || v.yres <= 0 || v.line_stride_in_bytes <= 0) return;
    src_width_.store(v.xres,  std::memory_order_relaxed);
    src_height_.store(v.yres, std::memory_order_relaxed);

    Frame f;
    f.width  = out_width_;
    f.height = out_height_;
    f.data   = std::shared_ptr<uint8_t[]>(
        new uint8_t[static_cast<std::size_t>(out_width_) * out_height_ * 4]());

    // BGRX/BGRA → RGBA: same byte order minus the channel swap. If src
    // dims differ from our render dims, also bilinear-scale via swscale
    // — that's the only reason we pull libswscale here.
    SwsContext* sws = sws_getContext(
        v.xres, v.yres, AV_PIX_FMT_BGRA,
        out_width_, out_height_, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        if (logger_) logger_->warn("NdiInput: sws_getContext failed (xres={}, yres={})",
                                   v.xres, v.yres);
        return;
    }

    const std::uint8_t* src_planes[1]   = { v.p_data };
    const int           src_strides[1]  = { v.line_stride_in_bytes };
    std::uint8_t*       dst_planes[1]   = { f.data.get() };
    const int           dst_strides[1]  = { out_width_ * 4 };
    sws_scale(sws, src_planes, src_strides, 0, v.yres, dst_planes, dst_strides);
    sws_freeContext(sws);

    if (v.timecode != 0) f.pts = v.timecode / 10;  // 100ns → microseconds

    {
        std::lock_guard<std::mutex> lk(last_frame_mtx_);
        last_frame_ = std::move(f);
    }
}

void NdiInput::ingestAudio(const abi::audio_frame_v2_t& a) {
    if (a.no_samples <= 0 || a.no_channels <= 0 || !a.p_data) return;

    // NDI audio is planar float (channel 0 then channel 1, separated by
    // channel_stride_in_bytes). We interleave into stereo float at 48 kHz.
    // For now reject sample rates other than 48k — c8 ships without
    // libswresample on this path; resampling can come in a follow-up if
    // operator deployments actually carry mismatched rates.
    if (a.sample_rate != kOutRate) {
        // Don't spam — log once per silence/recovery cycle is enough.
        static thread_local bool warned = false;
        if (!warned && logger_) {
            logger_->warn("NdiInput: dropping audio @ {} Hz (need {}); "
                          "resampling not yet implemented",
                          a.sample_rate, kOutRate);
            warned = true;
        }
        return;
    }
    has_audio_.store(true, std::memory_order_release);

    const int  src_ch    = a.no_channels;
    const int  samples   = a.no_samples;
    const auto stride_b  = static_cast<std::size_t>(a.channel_stride_in_bytes);
    if (stride_b < static_cast<std::size_t>(samples) * sizeof(float)) return;

    std::vector<float> interleaved(static_cast<std::size_t>(samples) * kOutCh, 0.0f);
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(a.p_data);
    const float* ch0 = reinterpret_cast<const float*>(base);
    const float* ch1 = (src_ch >= 2)
        ? reinterpret_cast<const float*>(base + stride_b) : ch0;
    for (int i = 0; i < samples; ++i) {
        interleaved[2 * i + 0] = ch0[i];
        interleaved[2 * i + 1] = ch1[i];
    }

    std::lock_guard<std::mutex> lk(audio_mtx_);
    audio_buf_.insert(audio_buf_.end(), interleaved.begin(), interleaved.end());
    if (audio_buf_.size() > static_cast<std::size_t>(kJitterMaxFloats)) {
        const std::size_t drop = audio_buf_.size() - kJitterMaxFloats;
        audio_buf_.erase(audio_buf_.begin(),
                         audio_buf_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

// ─── Read accessors ──────────────────────────────────────────────────────────

Frame NdiInput::getFrame() {
    std::lock_guard<std::mutex> lk(last_frame_mtx_);
    return last_frame_;
}

AudioFrame NdiInput::getAudio(int num_samples) {
    AudioFrame out;
    out.num_samples = num_samples;
    out.sample_rate = kOutRate;
    out.channels    = kOutCh;
    out.samples.resize(static_cast<std::size_t>(num_samples) * kOutCh, 0.0f);
    out.valid       = true;

    std::lock_guard<std::mutex> lk(audio_mtx_);
    const std::size_t want = out.samples.size();
    const std::size_t have = std::min(want, audio_buf_.size());
    for (std::size_t i = 0; i < have; ++i) out.samples[i] = audio_buf_[i];
    audio_buf_.erase(audio_buf_.begin(),
                     audio_buf_.begin() + static_cast<std::ptrdiff_t>(have));
    return out;
}

// ─── Diagnostics ─────────────────────────────────────────────────────────────

nlohmann::json NdiInput::statusJson() const {
    nlohmann::json j;
    j["type"]              = "ndi";
    j["source_name"]       = cfg_.source_name;
    j["url"]               = cfg_.url_address;
    j["groups"]            = cfg_.groups;
    j["low_bandwidth"]     = cfg_.low_bandwidth;
    j["recv_name"]         = cfg_.recv_name;
    j["packets_recv"]      = packets_recv_.load(std::memory_order_relaxed);
    j["reconnect_count"]   = reconnect_count_.load(std::memory_order_relaxed);
    j["stalled"]           = stalled_.load(std::memory_order_acquire);
    j["has_audio"]         = has_audio_.load(std::memory_order_acquire);
    j["prepared"]          = prepared_.load(std::memory_order_acquire);
    j["src_width"]         = src_width_.load(std::memory_order_relaxed);
    j["src_height"]        = src_height_.load(std::memory_order_relaxed);
    j["ndi_loaded"]        = static_cast<bool>(lib_ && lib_->ok());
    j["ndi_version"]       = (lib_ && lib_->ok() && lib_->version_string)
                              ? lib_->version_string() : "";
    return j;
}

}  // namespace liveqx::ndi
