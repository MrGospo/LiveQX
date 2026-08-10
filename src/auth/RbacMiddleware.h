#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "auth/AuthTypes.h"

namespace liveqx::auth {

class JwtIssuer;
class AuthDb;

// Pre-handler hook in ControlApi. For every incoming request:
//   1. Bypass for open endpoints (login, refresh, healthz, etc).
//   2. Otherwise: parse `Authorization: Bearer`, verify JWT, populate
//      RequestContext.
//   3. Reject with 401 if token missing/invalid/expired/revoked.
//   4. Reject with 403 if role/ACL doesn't satisfy the endpoint
//      requirement, or if must_change_password=true and the request
//      mutates anything other than /api/auth/me/password.
//
// Endpoint key format: "<METHOD> <PATH>" where PATH may contain segments
// like "{id}" — they match any single non-slash segment. Examples:
//   "GET /healthz"
//   "POST /api/channels/{id}/play"
//   "DELETE /api/channels/{id}/playlist/{idx}"
//
// Channel-ID extraction is the caller's job: ControlApi already does
// req.matches[1] for routing; it passes the resolved id (or -1 if N/A)
// to authorize() for ACL checks. The middleware itself doesn't parse
// path params.
//
// Implementation lands in commit 6/24 of fix22.
class RbacMiddleware {
public:
    enum class Decision {
        Allow,
        Unauthorized,        // 401 — no/bad/expired/revoked token
        Forbidden,           // 403 — role insufficient or no channel ACL
        PasswordChangeRequired, // 403 — must_change_password=true blocks mutation
        NotConfigured,       // 500 — no rule registered for this endpoint
                             // (default-deny: missing rule = bug, not "open")
    };

    struct EndpointRule {
        Role         min_role{Role::Admin};
        bool         needs_channel_acl{false};
        ChannelPermission channel_perm{ChannelPermission::View};
        bool         open{false};   // login, healthz, etc.
    };

    RbacMiddleware(AuthDb& db, JwtIssuer& jwt);

    // Register the static endpoint→rule table. Called once at server
    // startup, lookups are O(1) thereafter for literal paths and O(N)
    // over patterns for parameterized paths.
    //
    // Endpoint format: "<METHOD> <PATH>" (single space). Re-registering
    // an existing key overwrites the previous rule.
    void registerEndpoint(std::string method_and_path, EndpointRule rule);

    // Test helper: drop all rules.
    void clearRules();

    // Test helper: how many rules currently registered (literal + pattern).
    std::size_t ruleCount() const;

    // Authorize a request. `bearer_token` may be empty.
    // `channel_id` is the resolved channel id from the URL (<0 when N/A).
    // Sets `*out_ctx` on Allow (only when out_ctx != nullptr).
    Decision authorize(std::string_view method,
                       std::string_view path,
                       std::string_view bearer_token,
                       std::int64_t      channel_id,
                       RequestContext*   out_ctx);

private:
    AuthDb&    db_;
    JwtIssuer& jwt_;

    // Literal endpoints (no {…}) — exact map lookup.
    std::unordered_map<std::string, EndpointRule> literal_rules_;

    // Parameterized endpoints — split into segments at registration; a
    // "{name}" segment matches any non-empty path segment.
    struct PatternRule {
        std::string method;
        std::vector<std::string> segments;  // includes leading "" for "/foo/bar"
        EndpointRule rule;
    };
    std::vector<PatternRule> pattern_rules_;

    // Returns nullptr if no rule matches.
    const EndpointRule* findRule(std::string_view method,
                                 std::string_view path) const;
};

}  // namespace liveqx::auth
