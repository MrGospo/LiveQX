// fix26 c10 — on-disk store for stress run reports.
//
// Reports are written as one JSON file per run to <dir>/<id>.json where
// id = "YYYY-MM-DDTHH-MM-SSZ" derived from the report's started_at_ms.
// On collision (rare; same minute) we append "-N" until unique. We keep
// at most kMaxReports files; oldest are pruned on each write.
//
// The list/read APIs return JSON values directly because the stored
// schema is the StressReport JSON; there is no need to round-trip
// through the C++ struct on read.

#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stress/StressReport.h"

namespace liveqx::stress {

class StressReportStore {
public:
    static constexpr std::size_t kMaxReports = 100;

    explicit StressReportStore(std::filesystem::path dir);

    // Returns the assigned id on success; empty string on failure.
    std::string write(const StressReport& report);

    // List metadata for all reports, newest first.
    struct Entry {
        std::string  id;
        std::int64_t started_at_ms = 0;
        std::int64_t ended_at_ms   = 0;
        bool         pass          = false;
        std::string  verdict;
    };
    std::vector<Entry> list() const;

    // Returns nullopt if id is unknown or the file fails to parse.
    std::optional<nlohmann::json> read(const std::string& id) const;

    // Returns true if the file was deleted, false if not found.
    bool remove(const std::string& id);

    const std::filesystem::path& dir() const { return dir_; }

private:
    static std::string idFromTimestamp(std::int64_t started_at_ms);
    void pruneOldestLocked();

    mutable std::mutex     mu_;
    std::filesystem::path  dir_;
};

}  // namespace liveqx::stress
