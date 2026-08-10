// FFmpeg → spdlog redirector. Split out from Log.cpp so unit-test
// targets that don't link FFmpeg can still pull in Log helpers.
#include <cstring>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/log.h>
}

#include "utils/Log.h"

namespace {
void ffmpegLogCallback(void*, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    int len = static_cast<int>(strlen(buf));
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    if (len == 0) return;
    if (level <= AV_LOG_ERROR) spdlog::error("[FFmpeg] {}", buf);
    else                       spdlog::warn ("[FFmpeg] {}", buf);
}
}  // namespace

namespace Log {
void installFFmpegRedirect() {
    av_log_set_callback(ffmpegLogCallback);
}
}  // namespace Log
