#include "encoding/EncoderFactory.h"

#include <algorithm>
#include <array>
#include <cctype>

#include <spdlog/spdlog.h>

#include "encoding/NvencVideoEncoder.h"
#include "encoding/QsvVideoEncoder.h"
#include "encoding/VaapiVideoEncoder.h"
#include "encoding/X264VideoEncoder.h"

namespace liveqx::encoding {

namespace {

spdlog::logger& lg(const std::shared_ptr<spdlog::logger>& l) noexcept {
    return l ? *l : *spdlog::default_logger();
}

std::unique_ptr<IVideoEncoder> tryOpen(std::unique_ptr<IVideoEncoder> enc) {
    if (!enc) return nullptr;
    return enc->open() ? std::move(enc) : nullptr;
}

}  // namespace

std::string normalizeEncoderMode(const std::string& mode) {
    std::string out = mode;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::unique_ptr<IVideoEncoder> pickVideoEncoder(
    const std::string&              mode,
    const IVideoEncoder::Config&    cfg,
    std::shared_ptr<spdlog::logger> logger) {

    const std::string m = normalizeEncoderMode(mode);

    // Explicit user request — instantiate exactly that backend, no
    // fallback. Caller decides whether nullptr is fatal or recoverable.
    if (m == "cpu" || m == "x264") {
        return tryOpen(std::make_unique<X264VideoEncoder>(cfg, logger));
    }
    if (m == "nvenc") {
        return tryOpen(std::make_unique<NvencVideoEncoder>(cfg, logger));
    }
    if (m == "qsv") {
        return tryOpen(std::make_unique<QsvVideoEncoder>(cfg, logger));
    }
    if (m == "vaapi") {
        return tryOpen(std::make_unique<VaapiVideoEncoder>(cfg, logger));
    }

    // "auto" / empty / unknown → ladder. Skip GPU backends that aren't
    // even compiled in — open() would just fail with a stub warning,
    // but logging the attempt at info-level is misleading.
    if (m != "auto" && !m.empty()) {
        lg(logger).warn("EncoderFactory: unknown encoder_mode \"{}\", treating as \"auto\"", mode);
    }

    if (NvencVideoEncoder::isBuiltIn()) {
        if (auto e = tryOpen(std::make_unique<NvencVideoEncoder>(cfg, logger))) {
            lg(logger).info("EncoderFactory: auto-selected nvenc");
            return e;
        }
        lg(logger).warn("EncoderFactory: nvenc unavailable at runtime, trying next backend");
    }
    if (QsvVideoEncoder::isBuiltIn()) {
        if (auto e = tryOpen(std::make_unique<QsvVideoEncoder>(cfg, logger))) {
            lg(logger).info("EncoderFactory: auto-selected qsv");
            return e;
        }
        lg(logger).warn("EncoderFactory: qsv unavailable at runtime, trying next backend");
    }
    if (VaapiVideoEncoder::isBuiltIn()) {
        if (auto e = tryOpen(std::make_unique<VaapiVideoEncoder>(cfg, logger))) {
            lg(logger).info("EncoderFactory: auto-selected vaapi");
            return e;
        }
        lg(logger).warn("EncoderFactory: vaapi unavailable at runtime, trying next backend");
    }

    auto x = tryOpen(std::make_unique<X264VideoEncoder>(cfg, logger));
    if (x) lg(logger).info("EncoderFactory: auto-selected x264 (CPU)");
    return x;
}

}  // namespace liveqx::encoding
