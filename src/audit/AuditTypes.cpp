#include "audit/AuditTypes.h"

namespace liveqx::audit {

const char* categoryName(Category c) noexcept {
    switch (c) {
        case Category::Auth:    return "auth";
        case Category::Channel: return "channel";
        case Category::Output:  return "output";
        case Category::Gateway: return "gateway";
        case Category::Plugin:  return "plugin";
        case Category::Mount:   return "mount";
        case Category::System:  return "system";
        case Category::Access:  return "access";
    }
    return "system";
}

std::optional<Category> categoryFromString(std::string_view s) noexcept {
    if (s == "auth")    return Category::Auth;
    if (s == "channel") return Category::Channel;
    if (s == "output")  return Category::Output;
    if (s == "gateway") return Category::Gateway;
    if (s == "plugin")  return Category::Plugin;
    if (s == "mount")   return Category::Mount;
    if (s == "system")  return Category::System;
    if (s == "access")  return Category::Access;
    return std::nullopt;
}

int defaultRetentionDays(Category c) noexcept {
    // Regulator baseline: auth trail must survive one year. Business
    // mutations three months (long enough for a quarterly audit).
    // Ambient system/access noise thirty days. Ops overrides these
    // via the retention worker config once we ship it.
    switch (c) {
        case Category::Auth:    return 365;
        case Category::Channel: return 90;
        case Category::Output:  return 90;
        case Category::Gateway: return 90;
        case Category::Plugin:  return 90;
        case Category::Mount:   return 90;
        case Category::System:  return 30;
        case Category::Access:  return 30;
    }
    return 30;
}

}  // namespace liveqx::audit
