#pragma once
#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace Log {
    // Initialize main logger (logs/liveqx.log + console). Sets the
    // global default logger, FFmpeg redirect, and the level used by every
    // logger created afterwards.
    void init(const std::string& log_dir, const std::string& level = "info");

    // Build a per-channel rotating logger sinking to
    // logs/ch{id}-{sanitized_name}.log (10MB × 5) plus the shared console
    // sink. Pattern includes "[ch{id}:{name}]" so callers can use plain
    // logger->info("...") without hand-prefixing each line.
    //
    // Logger name is "ch{id}" — registered with spdlog so spdlog::get(...)
    // works elsewhere. Subsequent calls with the same id replace the
    // existing registration. Returns the new logger.
    std::shared_ptr<spdlog::logger>
    createChannelLogger(int id, const std::string& name);

    // Same as createChannelLogger but writes to {log_dir}/channel.log instead
    // of the global cached log dir. Used when each channel owns its own
    // directory (channels/ch{id}-{name}/logs/channel.log) — see fix7.
    // log_dir is created if missing. Logger registration name remains
    // "ch{id}" so spdlog::get works the same.
    std::shared_ptr<spdlog::logger>
    createChannelLoggerInDir(int id, const std::string& name,
                              const std::string& log_dir);

    // Apply the given level (spdlog::level::from_str) to the default
    // logger AND every registered channel logger. Persists for loggers
    // created afterwards as well.
    void setLevelGlobally(const std::string& level);

    // Redirect FFmpeg (libav*) log output into spdlog. Implemented in
    // LogFFmpeg.cpp so test binaries that don't link libavutil can omit it.
    void installFFmpegRedirect();

    // Best-effort wrapper around pthread_setname_np. Linux limits names
    // to 15 bytes — longer names are silently truncated. No-op on
    // platforms without pthread support.
    void setThreadName(const std::string& name);
}

// syslog.h (pulled in by libsrt) defines LOG_* as integers — override them
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#define LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
