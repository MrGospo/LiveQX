#include "stress/StressReportStore.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace liveqx::stress {

namespace {

bool endsWithDotJson(const fs::path& p) {
    return p.extension() == ".json";
}

}  // namespace

StressReportStore::StressReportStore(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        spdlog::warn("stress[reports]: cannot create {} ({})",
                     dir_.string(), ec.message());
    }
}

std::string StressReportStore::idFromTimestamp(std::int64_t started_at_ms) {
    std::time_t t = static_cast<std::time_t>(started_at_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H-%M-%SZ");
    return os.str();
}

void StressReportStore::pruneOldestLocked() {
    std::error_code ec;
    std::vector<fs::directory_entry> files;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (e.is_regular_file() && endsWithDotJson(e.path())) {
            files.push_back(e);
        }
    }
    if (files.size() <= kMaxReports) return;
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) {
                  return a.path().filename() < b.path().filename();
              });
    const std::size_t to_drop = files.size() - kMaxReports;
    for (std::size_t i = 0; i < to_drop; ++i) {
        std::error_code rmec;
        fs::remove(files[i].path(), rmec);
    }
}

std::string StressReportStore::write(const StressReport& report) {
    std::scoped_lock lk(mu_);
    std::error_code ec;
    fs::create_directories(dir_, ec);

    std::string base = idFromTimestamp(report.started_at_ms);
    std::string id   = base;
    int suffix = 1;
    while (fs::exists(dir_ / (id + ".json"))) {
        id = base + "-" + std::to_string(suffix++);
        if (suffix > 1000) {  // give up; very unlikely
            return {};
        }
    }
    const auto path = dir_ / (id + ".json");
    {
        std::ofstream out(path);
        if (!out) {
            spdlog::warn("stress[reports]: cannot open {} for write",
                         path.string());
            return {};
        }
        out << report.toJson().dump(2);
    }
    pruneOldestLocked();
    return id;
}

std::vector<StressReportStore::Entry> StressReportStore::list() const {
    std::scoped_lock lk(mu_);
    std::vector<Entry> out;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!e.is_regular_file() || !endsWithDotJson(e.path())) continue;
        std::ifstream in(e.path());
        if (!in) continue;
        nlohmann::json j;
        try { in >> j; } catch (...) { continue; }
        Entry ent;
        ent.id = e.path().stem().string();
        if (j.contains("started_at_ms") && j["started_at_ms"].is_number_integer())
            ent.started_at_ms = j["started_at_ms"].get<std::int64_t>();
        if (j.contains("ended_at_ms") && j["ended_at_ms"].is_number_integer())
            ent.ended_at_ms = j["ended_at_ms"].get<std::int64_t>();
        if (j.contains("pass") && j["pass"].is_boolean())
            ent.pass = j["pass"].get<bool>();
        if (j.contains("verdict") && j["verdict"].is_string())
            ent.verdict = j["verdict"].get<std::string>();
        out.push_back(std::move(ent));
    }
    // Newest first by id (id is derived from started timestamp).
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.id > b.id; });
    return out;
}

std::optional<nlohmann::json> StressReportStore::read(const std::string& id) const {
    std::scoped_lock lk(mu_);
    const auto path = dir_ / (id + ".json");
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;
    std::ifstream in(path);
    if (!in) return std::nullopt;
    nlohmann::json j;
    try { in >> j; } catch (...) { return std::nullopt; }
    return j;
}

bool StressReportStore::remove(const std::string& id) {
    std::scoped_lock lk(mu_);
    const auto path = dir_ / (id + ".json");
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return false;
    fs::remove(path, ec);
    return !ec;
}

}  // namespace liveqx::stress
