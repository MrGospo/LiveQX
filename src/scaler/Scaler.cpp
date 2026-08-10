#include "scaler/Scaler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// ─── Impl: caches the SwsContext for the current (src_w, src_h, dst_w, dst_h)
//     tuple so we pay the alloc cost only when dimensions change.

struct Scaler::Impl {
    SwsContext* sws     = nullptr;
    int src_w = 0, src_h = 0;
    int dst_w = 0, dst_h = 0;

    ~Impl() { if (sws) sws_freeContext(sws); }

    // Returns a ready context for the requested dimensions, or nullptr on failure.
    SwsContext* get(int sw, int sh, int dw, int dh) {
        if (sws && sw == src_w && sh == src_h && dw == dst_w && dh == dst_h)
            return sws;
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        sws = sws_getContext(sw, sh, AV_PIX_FMT_RGBA,
                             dw, dh, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) { src_w = sw; src_h = sh; dst_w = dw; dst_h = dh; }
        return sws;
    }
};

// ─── helpers ──────────────────────────────────────────────────────────────────

namespace {

// Run sws_scale from a raw RGBA buffer into *dst_ptr*, where dst_ptr is already
// positioned at the correct pixel offset inside the final output buffer.
// out_stride is the stride of the FULL output buffer (not the scaled region).
void doScale(SwsContext* ctx,
             const uint8_t* src, int src_w, int src_h,
             uint8_t* dst_ptr, int out_stride) {
    const uint8_t* const src_data[4] = { src,     nullptr, nullptr, nullptr };
    const int            src_stride[4]= { src_w*4, 0,       0,       0       };
    uint8_t*             dst_data[4]  = { dst_ptr, nullptr, nullptr, nullptr };
    const int            dst_stride[4]= { out_stride, 0,    0,       0       };
    sws_scale(ctx, src_data, src_stride, 0, src_h, dst_data, dst_stride);
}

} // namespace

// ─── Scaler ──────────────────────────────────────────────────────────────────

ScaleMode scaleModeFromString(const std::string& s) {
    if (s == "fill")    return ScaleMode::Fill;
    if (s == "stretch") return ScaleMode::Stretch;
    return ScaleMode::Fit;
}

Scaler::Scaler(int out_width, int out_height, ScaleMode mode)
    : out_width_(out_width), out_height_(out_height), mode_(mode),
      impl_(std::make_unique<Impl>()) {}

Scaler::~Scaler() = default;

Frame Scaler::scale(const Frame& input) {
    if (!input.valid()) return Frame{};

    const int src_w = input.width;
    const int src_h = input.height;
    if (src_w <= 0 || src_h <= 0) return Frame{};

    // Allocate output buffer (zeroed → black for Fit padding).
    const size_t out_bytes = static_cast<size_t>(out_width_) * out_height_ * 4;
    auto out_buf = std::make_shared<uint8_t[]>(out_bytes);
    std::memset(out_buf.get(), 0, out_bytes);

    Frame out;
    out.width  = out_width_;
    out.height = out_height_;
    out.pts    = input.pts;
    out.data   = out_buf;

    switch (mode_) {

    // ── Stretch ───────────────────────────────────────────────────────────────
    case ScaleMode::Stretch: {
        auto* ctx = impl_->get(src_w, src_h, out_width_, out_height_);
        if (!ctx) return out; // sws unavailable — return zeroed buffer
        doScale(ctx, input.pixels(), src_w, src_h,
                out_buf.get(), out_width_ * 4);
        break;
    }

    // ── Fit (letterbox / pillarbox) ───────────────────────────────────────────
    case ScaleMode::Fit: {
        const float sx = static_cast<float>(out_width_)  / src_w;
        const float sy = static_cast<float>(out_height_) / src_h;
        const float s  = std::min(sx, sy);

        const int scaled_w = std::max(1, static_cast<int>(src_w * s));
        const int scaled_h = std::max(1, static_cast<int>(src_h * s));
        const int off_x    = (out_width_  - scaled_w) / 2;
        const int off_y    = (out_height_ - scaled_h) / 2;

        auto* ctx = impl_->get(src_w, src_h, scaled_w, scaled_h);
        if (!ctx) return out;

        // Write scaled image directly into the right position of the output
        // buffer; the surrounding zeroes form the black letterbox bars.
        uint8_t* dst_ptr = out_buf.get() + off_y * out_width_ * 4 + off_x * 4;
        doScale(ctx, input.pixels(), src_w, src_h,
                dst_ptr, out_width_ * 4);
        break;
    }

    // ── Fill (crop to fill, no black bars) ────────────────────────────────────
    case ScaleMode::Fill: {
        const float sx = static_cast<float>(out_width_)  / src_w;
        const float sy = static_cast<float>(out_height_) / src_h;
        const float s  = std::max(sx, sy);

        const int scaled_w = std::max(out_width_,  static_cast<int>(src_w * s));
        const int scaled_h = std::max(out_height_, static_cast<int>(src_h * s));
        const int off_x    = (scaled_w - out_width_)  / 2;
        const int off_y    = (scaled_h - out_height_) / 2;

        auto* ctx = impl_->get(src_w, src_h, scaled_w, scaled_h);
        if (!ctx) return out;

        // Scale into a temporary oversized buffer, then copy the centre crop.
        const size_t tmp_bytes = static_cast<size_t>(scaled_w) * scaled_h * 4;
        auto tmp = std::make_unique<uint8_t[]>(tmp_bytes);
        doScale(ctx, input.pixels(), src_w, src_h,
                tmp.get(), scaled_w * 4);

        for (int row = 0; row < out_height_; ++row) {
            const uint8_t* src_row = tmp.get()
                + static_cast<size_t>(row + off_y) * scaled_w * 4
                + off_x * 4;
            uint8_t* dst_row = out_buf.get()
                + static_cast<size_t>(row) * out_width_ * 4;
            std::memcpy(dst_row, src_row,
                        static_cast<size_t>(out_width_) * 4);
        }
        break;
    }

    } // switch

    return out;
}
