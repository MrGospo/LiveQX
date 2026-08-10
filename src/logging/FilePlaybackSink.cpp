#include "FilePlaybackSink.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <vector>

#include <spdlog/spdlog.h>

namespace liveqx::logging {

namespace fs = std::filesystem;

namespace {

nlohmann::json toJson(const PlaybackEvent& ev) {
    return {
        {"channel_id",      ev.channel_id},
        {"clip_path",       ev.clip_path},
        {"clip_type",       ev.clip_type},
        {"started_at_ns",   ev.started_at_ns},
        {"ended_at_ns",     ev.ended_at_ns},
        {"played_sec",      ev.played_sec},
        {"transition_type", ev.transition_type},
        {"status",          ev.status},
        {"error_reason",    ev.error_reason},
    };
}

bool isDayFile(const std::string& name) {
    // file-YYYY-MM-DD.jsonl (15 + 6 = 21 chars total minimum)
    if (name.size() != 21) return false;
    return name.compare(0, 5, "file-") == 0 &&
           name.compare(15, 6, ".jsonl") == 0;
}

}  // namespace

FilePlaybackSink::FilePlaybackSink(int channel_id, fs::path channel_dir)
    : channel_id_(channel_id),
      playback_dir_(std::move(channel_dir) / "playback") {
    std::error_code ec;
    fs::create_directories(playback_dir_, ec);
    if (ec) {
        throw std::runtime_error("FilePlaybackSink: cannot create " +
                                 playback_dir_.string() + ": " + ec.message());
    }
    writer_ = std::thread(&FilePlaybackSink::writerLoop, this);
}

FilePlaybackSink::~FilePlaybackSink() {
    running_.store(false, std::memory_order_release);
    queue_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (current_file_.is_open()) current_file_.close();
}

void FilePlaybackSink::log(const PlaybackEvent& ev) {
    if (!queue_.push(ev)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.notify();
}

std::string FilePlaybackSink::dayString(int64_t ns) {
    const auto sec = static_cast<std::time_t>(ns / 1'000'000'000LL);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void FilePlaybackSink::rotateIfNeeded(const std::string& day) {
    if (day == current_day_ && current_file_.is_open()) return;
    if (current_file_.is_open()) current_file_.close();
    const auto path = playback_dir_ / ("file-" + day + ".jsonl");
    current_file_.open(path, std::ios::app);
    if (!current_file_) {
        spdlog::error("FilePlaybackSink ch{}: failed to open {}", channel_id_,
                      path.string());
        return;
    }
    current_day_ = day;
}

void FilePlaybackSink::writerLoop() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
        auto opt = queue_.pop_wait(100ms);
        if (!opt) continue;
        const auto& ev = *opt;
        rotateIfNeeded(dayString(ev.started_at_ns));
        if (!current_file_.is_open()) continue;
        current_file_ << toJson(ev).dump() << '\n';
        current_file_.flush();
        last_write_ns_.store(ev.started_at_ns, std::memory_order_relaxed);
    }
}

nlohmann::json FilePlaybackSink::query(const QueryParams& params) {
    const int limit  = std::clamp(params.limit, 1, 1000);
    const int offset = std::max(0, params.offset);
    const int64_t from   = params.from_ns.value_or(INT64_MIN);
    const int64_t to     = params.to_ns.value_or(INT64_MAX);
    const int64_t cursor = params.after_ns.value_or(INT64_MIN);

    nlohmann::json result_events = nlohmann::json::array();
    nlohmann::json next_after_ns = nullptr;

    if (!fs::is_directory(playback_dir_)) {
        return {{"events", result_events}, {"next_after_ns", next_after_ns}};
    }

    // Lexicographic sort of YYYY-MM-DD file names == chronological.
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(playback_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (isDayFile(entry.path().filename().string()))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    std::vector<nlohmann::json> matched;
    for (const auto& fp : files) {
        std::ifstream in(fp);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            nlohmann::json j;
            try { j = nlohmann::json::parse(line); }
            catch (...) { continue; }  // skip malformed (e.g. truncated tail)
            const int64_t st = j.value("started_at_ns", int64_t{0});
            if (st < from || st > to) continue;
            if (params.after_ns && st <= cursor) continue;
            matched.push_back(std::move(j));
        }
    }

    std::sort(matched.begin(), matched.end(),
              [](const auto& a, const auto& b) {
                  return a.value("started_at_ns", int64_t{0}) <
                         b.value("started_at_ns", int64_t{0});
              });

    const size_t start = params.after_ns
        ? 0  // cursor mode already filtered above
        : std::min(static_cast<size_t>(offset), matched.size());
    for (size_t i = start;
         i < matched.size() && static_cast<int>(result_events.size()) < limit;
         ++i) {
        result_events.push_back(std::move(matched[i]));
    }

    if (static_cast<int>(result_events.size()) == limit &&
        !result_events.empty()) {
        next_after_ns = result_events.back().value("started_at_ns",
                                                   int64_t{0});
    }

    return {{"events", result_events}, {"next_after_ns", next_after_ns}};
}

namespace {

// "file-YYYY-MM-DD.jsonl" → [start_ns, end_ns] UTC дня. false при ошибке.
bool dayRangeNs(const std::string& filename,
                int64_t* start_ns, int64_t* end_ns) {
    if (!isDayFile(filename)) return false;
    std::tm tm{};
    if (!strptime(filename.c_str(), "file-%Y-%m-%d.jsonl", &tm)) return false;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    const std::time_t s = timegm(&tm);
    if (s == static_cast<std::time_t>(-1)) return false;
    const int64_t start = static_cast<int64_t>(s) * 1'000'000'000LL;
    const int64_t end   = start + 86'400LL * 1'000'000'000LL - 1LL;
    if (start_ns) *start_ns = start;
    if (end_ns)   *end_ns   = end;
    return true;
}

}  // namespace

nlohmann::json FilePlaybackSink::purge(const PurgeParams& params) {
    // FilePlaybackSink — append-only JSONL по дням. Текущий writer может в
    // этот момент держать current_file_ открытым на запись (на сегодняшний
    // день), и переписывать его «под рукой» небезопасно: writer_thread
    // ничего не знает о подмене. Минимально-инвазивная реализация:
    // удалять только файлы, целиком попадающие в [from, to] и НЕ
    // совпадающие с текущим открытым файлом. Частичное прорезание дня
    // (rewrite-with-filter) можно добавить, когда понадобится — пока
    // основной sink в проде это SQLite, см. SqlitePlaybackSink::purge().
    const int64_t from = params.from_ns.value_or(INT64_MIN);
    const int64_t to   = params.to_ns.value_or(INT64_MAX);
    int64_t removed_files = 0;
    if (!fs::is_directory(playback_dir_)) {
        return {{"deleted_rows", 0}, {"removed_files", 0}};
    }
    for (const auto& entry : fs::directory_iterator(playback_dir_)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        int64_t day_start = 0, day_end = 0;
        if (!dayRangeNs(name, &day_start, &day_end)) continue;
        if (day_end < from || day_start > to) continue;
        const bool fully = (day_start >= from && day_end <= to);
        if (!fully) continue;
        std::error_code rm_ec;
        if (fs::remove(entry.path(), rm_ec)) ++removed_files;
    }
    return {{"deleted_rows", 0}, {"removed_files", removed_files}};
}

nlohmann::json FilePlaybackSink::statusJson() const {
    int file_count = 0;
    if (fs::is_directory(playback_dir_)) {
        for (const auto& entry : fs::directory_iterator(playback_dir_)) {
            if (entry.is_regular_file() &&
                isDayFile(entry.path().filename().string()))
                ++file_count;
        }
    }
    const auto last = last_write_ns_.load(std::memory_order_relaxed);
    return {
        {"sink_type",      "file"},
        {"queue_depth",    queue_.empty() ? 0 : 1},  // SPSC has no size()
        {"dropped_count",  dropped_.load(std::memory_order_relaxed)},
        {"last_write_ns",  last == 0 ? nlohmann::json(nullptr)
                                     : nlohmann::json(last)},
        {"files_count",    file_count},
        {"playback_dir",   playback_dir_.string()},
    };
}

}  // namespace liveqx::logging
