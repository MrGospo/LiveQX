#include "stress/CronExpr.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace liveqx::stress {

namespace {

bool toInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    auto* first = s.data();
    auto* last  = s.data() + s.size();
    int v = 0;
    auto [p, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || p != last) return false;
    out = v;
    return true;
}

bool addRange(int lo, int hi, int min, int max,
              std::vector<int>& dst) {
    if (lo < min || hi > max || lo > hi) return false;
    for (int v = lo; v <= hi; ++v) dst.push_back(v);
    return true;
}

}  // namespace

bool CronExpr::parseField(const std::string& s, int min, int max, Field& out) {
    out.min = min;
    out.max = max;
    out.values.clear();

    std::string buf;
    auto flushTerm = [&](const std::string& term) -> bool {
        if (term.empty()) return false;
        // step form  "*/K" or "lo-hi/K"
        auto slash = term.find('/');
        if (slash != std::string::npos) {
            std::string base = term.substr(0, slash);
            std::string step = term.substr(slash + 1);
            int k = 0;
            if (!toInt(step, k) || k <= 0) return false;
            int lo = min, hi = max;
            if (base == "*") {
                // ok
            } else if (auto dash = base.find('-'); dash != std::string::npos) {
                if (!toInt(base.substr(0, dash), lo)) return false;
                if (!toInt(base.substr(dash + 1), hi)) return false;
                if (lo < min || hi > max || lo > hi) return false;
            } else {
                if (!toInt(base, lo)) return false;
                if (lo < min || lo > max) return false;
                hi = max;
            }
            for (int v = lo; v <= hi; v += k) out.values.push_back(v);
            return true;
        }
        if (term == "*") {
            for (int v = min; v <= max; ++v) out.values.push_back(v);
            return true;
        }
        if (auto dash = term.find('-'); dash != std::string::npos) {
            int lo = 0, hi = 0;
            if (!toInt(term.substr(0, dash), lo)) return false;
            if (!toInt(term.substr(dash + 1), hi)) return false;
            return addRange(lo, hi, min, max, out.values);
        }
        int v = 0;
        if (!toInt(term, v)) return false;
        if (v < min || v > max) return false;
        out.values.push_back(v);
        return true;
    };

    // Split on commas.
    std::string term;
    for (char ch : s) {
        if (ch == ',') {
            if (!flushTerm(term)) return false;
            term.clear();
        } else {
            term.push_back(ch);
        }
    }
    if (!flushTerm(term)) return false;

    std::sort(out.values.begin(), out.values.end());
    out.values.erase(std::unique(out.values.begin(), out.values.end()),
                     out.values.end());
    return !out.values.empty();
}

std::optional<CronExpr> CronExpr::parse(const std::string& expr,
                                         std::string* err) {
    std::vector<std::string> fields;
    std::stringstream ss(expr);
    std::string f;
    while (ss >> f) fields.push_back(f);
    if (fields.size() != 5) {
        if (err) *err = "expected 5 fields (m h dom mon dow), got " +
                        std::to_string(fields.size());
        return std::nullopt;
    }

    CronExpr c;
    c.source_ = expr;
    auto fail = [&](const std::string& which) {
        if (err) *err = "invalid " + which + " field: '" + fields[0] + "'";
    };
    if (!parseField(fields[0],  0, 59, c.minute_)) { fail("minute"); return std::nullopt; }
    if (!parseField(fields[1],  0, 23, c.hour_  )) { fail("hour");   return std::nullopt; }
    if (!parseField(fields[2],  1, 31, c.dom_   )) { fail("dom");    return std::nullopt; }
    if (!parseField(fields[3],  1, 12, c.month_ )) { fail("month");  return std::nullopt; }
    if (!parseField(fields[4],  0,  6, c.dow_   )) { fail("dow");    return std::nullopt; }
    return c;
}

bool CronExpr::matches(std::time_t utc_seconds) const {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &utc_seconds);
#else
    gmtime_r(&utc_seconds, &tm);
#endif
    auto contains = [](const Field& f, int v) {
        return std::binary_search(f.values.begin(), f.values.end(), v);
    };
    if (!contains(minute_, tm.tm_min))      return false;
    if (!contains(hour_,   tm.tm_hour))     return false;
    if (!contains(month_,  tm.tm_mon + 1))  return false;
    // Standard cron OR semantics for dom + dow when both are restricted, AND
    // when both are wildcards. We have no easy way to detect wildcard from the
    // expanded vector, so check sizes vs full ranges.
    const bool dom_wild = (dom_.values.size() == 31);
    const bool dow_wild = (dow_.values.size() == 7);
    const bool dom_match = contains(dom_, tm.tm_mday);
    const bool dow_match = contains(dow_, tm.tm_wday);
    if (dom_wild && dow_wild) return true;
    if (dom_wild)             return dow_match;
    if (dow_wild)             return dom_match;
    return dom_match || dow_match;
}

}  // namespace liveqx::stress
