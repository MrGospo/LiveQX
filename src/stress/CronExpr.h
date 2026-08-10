// fix26 c10 — minimal "m h dom mon dow" cron parser.
//
// Five fields, space-separated, each one of:
//   *           any value
//   N           literal number
//   N-M         inclusive range (N <= M)
//   N,M,...     list of literals/ranges
//   */K         every K-th value starting from the field minimum
//
// Field ranges:
//   minutes 0-59  hours 0-23  dom 1-31  mon 1-12  dow 0-6 (0 = Sunday)
//
// We deliberately do NOT support names ("MON", "JAN") or the L/W/?
// extensions — the runner only needs daily/hourly/weekly cadences.
//
// matches(time_t) checks all five fields against UTC components of the
// timestamp. The scheduler ticks once per minute and calls matches() with
// `t` aligned to the minute boundary, so a cron firing on a given minute
// runs at most once per minute regardless of scheduler poll frequency.

#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace liveqx::stress {

class CronExpr {
public:
    // Returns nullopt on parse failure with `err` populated.
    static std::optional<CronExpr> parse(const std::string& expr,
                                          std::string* err = nullptr);

    bool matches(std::time_t utc_seconds) const;

    const std::string& source() const { return source_; }

private:
    struct Field {
        std::vector<int> values;   // sorted, unique, populated set of allowed ints
        int min = 0, max = 0;      // bounds for the field (used by parse only)
    };

    static bool parseField(const std::string& s, int min, int max, Field& out);

    Field minute_, hour_, dom_, month_, dow_;
    std::string source_;
};

}  // namespace liveqx::stress
