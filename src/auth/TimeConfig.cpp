#include "auth/TimeConfig.h"

#include <stdexcept>

#include <date/tz.h>

namespace liveqx::auth {

std::string timeSourceToString(TimeSource s) noexcept {
    switch (s) {
        case TimeSource::SystemLocal: return "system_local";
        case TimeSource::Ntp:         return "ntp";
        case TimeSource::Manual:      return "manual";
    }
    return "system_local";
}

std::optional<TimeSource> timeSourceFromString(std::string_view s) noexcept {
    if (s == "system_local") return TimeSource::SystemLocal;
    if (s == "ntp")          return TimeSource::Ntp;
    if (s == "manual")       return TimeSource::Manual;
    return std::nullopt;
}

bool isValidIanaTimezone(std::string_view name) noexcept {
    if (name.empty()) return false;
    try {
        // date::locate_zone бросает std::runtime_error на unknown зону.
        // date/tz.h — переносимый tzdb (libstdc++ 12 std::chrono tzdb не имеет).
        (void)date::locate_zone(std::string(name));
        return true;
    } catch (const std::runtime_error&) {
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace liveqx::auth
