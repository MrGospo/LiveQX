#include "utils/Log.h"

#include <cstring>
#include <filesystem>
#include <mutex>
#include <pthread.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "utils/PathUtils.h"

namespace {

// Console sink shared across main + every per-channel logger so colored
// stdout output stays a single stream.
std::shared_ptr<spdlog::sinks::stdout_color_sink_mt>& sharedConsoleSink() {
    static std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> s;
    return s;
}

std::filesystem::path& cachedLogDir() {
    static std::filesystem::path p;
    return p;
}

std::mutex& createMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

void Log::init(const std::string& log_dir, const std::string& level) {
    std::filesystem::create_directories(log_dir);
    cachedLogDir() = log_dir;

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sharedConsoleSink() = console;

    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/liveqx.log",
        /*max_size=*/10 * 1024 * 1024,
        /*max_files=*/5);

    auto logger = std::make_shared<spdlog::logger>(
        "main", spdlog::sinks_init_list{console, file});

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::from_str(level));
    spdlog::flush_on(spdlog::level::info);
    // FFmpeg redirect lives in LogFFmpeg.cpp; main.cpp installs it after
    // init(). Test binaries that don't link libav skip this.
}

namespace {

// Shared logger build path used by both createChannelLogger (legacy global
// dir, file name "ch{id}-{name}.log") and createChannelLoggerInDir
// (per-channel dir, file name "channel.log"). Caller controls the full
// file path so the function only assembles sinks + pattern.
std::shared_ptr<spdlog::logger>
buildChannelLogger(int id, const std::string& name,
                   const std::string& file_path) {
    std::filesystem::create_directories(
        std::filesystem::path(file_path).parent_path());

    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        file_path,
        /*max_size=*/10 * 1024 * 1024,
        /*max_files=*/5);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(file);
    if (auto& c = sharedConsoleSink()) sinks.push_back(c);

    const std::string lname = "ch" + std::to_string(id);
    // Drop any prior registration so callers can rebuild the channel
    // (rare; primarily protects against id reuse in long-running
    // processes where a channel was deleted and another created with
    // the same id).
    spdlog::drop(lname);

    auto logger = std::make_shared<spdlog::logger>(lname, sinks.begin(), sinks.end());
    const std::string sanitized = PathUtils::sanitizeForPath(name);
    const std::string display = sanitized.empty() ? std::to_string(id)
                                                  : (std::to_string(id) + ":" + name);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [ch" + display + "] [%^%l%$] [%t] %v");
    if (auto def = spdlog::default_logger()) logger->set_level(def->level());
    logger->flush_on(spdlog::level::info);

    spdlog::register_logger(logger);
    return logger;
}

}  // namespace

std::shared_ptr<spdlog::logger>
Log::createChannelLogger(int id, const std::string& name) {
    std::lock_guard lk(createMutex());

    const std::string sanitized = PathUtils::sanitizeForPath(name);
    const std::string fname     = sanitized.empty()
        ? ("ch" + std::to_string(id))
        : ("ch" + std::to_string(id) + "-" + sanitized);

    const auto& dir = cachedLogDir();
    const std::string base = dir.empty() ? std::string("logs")
                                         : dir.string();
    return buildChannelLogger(id, name, base + "/" + fname + ".log");
}

std::shared_ptr<spdlog::logger>
Log::createChannelLoggerInDir(int id, const std::string& name,
                               const std::string& log_dir) {
    std::lock_guard lk(createMutex());
    const std::string base = log_dir.empty() ? std::string("logs") : log_dir;
    return buildChannelLogger(id, name, base + "/channel.log");
}

void Log::setLevelGlobally(const std::string& level) {
    const auto lvl = spdlog::level::from_str(level);
    spdlog::set_level(lvl); // affects default + future creates
    spdlog::apply_all([lvl](std::shared_ptr<spdlog::logger> l) {
        if (l) l->set_level(lvl);
    });
}

void Log::setThreadName(const std::string& name) {
#if defined(__linux__)
    // Linux limits thread names to 15 bytes (TASK_COMM_LEN-1).
    char buf[16];
    std::strncpy(buf, name.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
}
