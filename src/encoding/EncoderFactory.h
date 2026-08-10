#pragma once

// fix29 c11: pickVideoEncoder() — central dispatch from a textual mode
// onto a concrete IVideoEncoder backend. Used by Encoder::open() so
// callers don't have to #include every backend.
//
// Modes (case-insensitive):
//   "cpu"   / "x264"  → X264VideoEncoder (always available)
//   "nvenc"           → NvencVideoEncoder
//   "qsv"             → QsvVideoEncoder
//   "vaapi"           → VaapiVideoEncoder
//   "auto" / ""       → first available GPU backend, falling back to x264
//
// The factory both creates AND opens the backend. On open() failure it
// returns nullptr so callers can propagate the error verbatim. In "auto"
// mode failures are absorbed and the next backend is tried; the final
// fallback (x264) always succeeds in a normally-built binary.
//
// "auto" never picks a backend whose isBuiltIn() is false — there's no
// point in trying open() on a stub that's hard-coded to fail.

#include <memory>
#include <string>

#include <spdlog/logger.h>

#include "encoding/IVideoEncoder.h"

namespace liveqx::encoding {

std::unique_ptr<IVideoEncoder> pickVideoEncoder(
    const std::string&              mode,
    const IVideoEncoder::Config&    cfg,
    std::shared_ptr<spdlog::logger> logger);

// Lower-case copy of mode used by REST handlers and the dispatch table.
// Exposed so /api/system/gpu can present the same canonical labels.
std::string normalizeEncoderMode(const std::string& mode);

}  // namespace liveqx::encoding
