// fix22 commit 6/24 — RBAC middleware impl.
//
// Алгоритм authorize():
//   1. Найти правило: literal map → fallback на pattern-list.
//      Не нашли — Decision::NotConfigured (default-deny). В commit 24/24
//      ControlApi регистрирует правила для всех своих эндпоинтов;
//      missing-rule в проде = баг.
//   2. Если rule.open — Allow без проверки токена.
//   3. Bearer token обязателен. Пусто/нет — Unauthorized.
//   4. JWT verify (signature/exp). Сломан — Unauthorized.
//   5. Sessions.revoked_at должен быть NULL. Иначе — Unauthorized.
//   6. must_change_password=true блокирует ВСЁ кроме self-password
//      change (POST /api/auth/me/password). PasswordChangeRequired.
//   7. Role rank проверяется: Viewer < Operator < Admin.
//   8. Если needs_channel_acl — нужен grant с правильным perm'ом.
//      Admin bypass: не нуждается в явном grant.
//
// channel_id<0 означает "не применимо к этому endpoint'у" — ACL-чек
// тогда пропускается даже если rule.needs_channel_acl=true (caller
// должен был передать корректный id; если не передал, считаем что
// endpoint не привязан к каналу). Это разумно для GET-листингов где
// нет конкретного id в URL.

#include "auth/RbacMiddleware.h"

#include <algorithm>

#include "auth/AuthDb.h"
#include "auth/JwtIssuer.h"
#include "utils/Log.h"

namespace liveqx::auth {
namespace {

// Path identical to "/api/auth/me/password" — единственный mutating
// endpoint, открытый для пользователя с must_change_password=true.
constexpr const char* kSelfPasswordKey = "POST /api/auth/me/password";

int roleRank(Role r) noexcept {
    switch (r) {
        case Role::Viewer:   return 1;
        case Role::Operator: return 2;
        case Role::Admin:    return 3;
    }
    return 0;
}

std::vector<std::string> splitSegments(std::string_view path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            out.emplace_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool isPattern(std::string_view path) {
    return path.find('{') != std::string_view::npos;
}

bool segmentMatches(const std::string& pattern, std::string_view actual) {
    if (!pattern.empty() && pattern.front() == '{' && pattern.back() == '}') {
        return !actual.empty();  // {id} матчит любой непустой сегмент
    }
    return pattern == actual;
}

}  // namespace

RbacMiddleware::RbacMiddleware(AuthDb& db, JwtIssuer& jwt) : db_(db), jwt_(jwt) {}

void RbacMiddleware::registerEndpoint(std::string method_and_path,
                                      EndpointRule rule) {
    const auto sp = method_and_path.find(' ');
    if (sp == std::string::npos) {
        LOG_ERROR("RbacMiddleware: malformed endpoint key '{}' — expected '<METHOD> <PATH>'",
                  method_and_path);
        return;
    }
    const std::string method = method_and_path.substr(0, sp);
    const std::string path   = method_and_path.substr(sp + 1);

    if (isPattern(path)) {
        PatternRule pr;
        pr.method   = method;
        pr.segments = splitSegments(path);
        pr.rule     = rule;
        pattern_rules_.push_back(std::move(pr));
    } else {
        literal_rules_[std::move(method_and_path)] = rule;
    }
}

void RbacMiddleware::clearRules() {
    literal_rules_.clear();
    pattern_rules_.clear();
}

std::size_t RbacMiddleware::ruleCount() const {
    return literal_rules_.size() + pattern_rules_.size();
}

const RbacMiddleware::EndpointRule*
RbacMiddleware::findRule(std::string_view method, std::string_view path) const {
    // Literal lookup first — O(1).
    std::string key;
    key.reserve(method.size() + 1 + path.size());
    key.append(method).append(" ").append(path);
    if (auto it = literal_rules_.find(key); it != literal_rules_.end()) {
        return &it->second;
    }

    // Pattern fallback — O(N×segments). N typically < 50, fine.
    auto path_segs = splitSegments(path);
    for (const auto& pr : pattern_rules_) {
        if (pr.method != method)               continue;
        if (pr.segments.size() != path_segs.size()) continue;
        bool ok = true;
        for (std::size_t i = 0; i < pr.segments.size(); ++i) {
            if (!segmentMatches(pr.segments[i], path_segs[i])) { ok = false; break; }
        }
        if (ok) return &pr.rule;
    }
    return nullptr;
}

RbacMiddleware::Decision
RbacMiddleware::authorize(std::string_view method,
                          std::string_view path,
                          std::string_view bearer_token,
                          std::int64_t      channel_id,
                          RequestContext*   out_ctx) {
    const auto* rule = findRule(method, path);
    if (!rule) {
        // default-deny + лог чтобы при разработке вылавливать
        // незарегистрированные эндпоинты.
        LOG_WARN("RbacMiddleware: no rule for '{} {}'",
                 std::string(method), std::string(path));
        return Decision::NotConfigured;
    }

    if (rule->open) {
        // healthz/login/refresh — токен можно не парсить.
        return Decision::Allow;
    }

    if (bearer_token.empty()) return Decision::Unauthorized;

    auto claims = jwt_.verify(bearer_token);
    if (!claims) return Decision::Unauthorized;

    auto sess = db_.findSessionByJwtId(claims->jti);
    if (!sess || sess->revoked_at.has_value()) return Decision::Unauthorized;

    if (claims->must_change_password) {
        // Читаем ровно один key — POST /api/auth/me/password — иначе блок.
        std::string key;
        key.reserve(method.size() + 1 + path.size());
        key.append(method).append(" ").append(path);
        if (key != kSelfPasswordKey) return Decision::PasswordChangeRequired;
    }

    if (roleRank(claims->role) < roleRank(rule->min_role)) {
        return Decision::Forbidden;
    }

    if (rule->needs_channel_acl && channel_id >= 0) {
        // Admin минует per-channel ACL — он по определению полно-привилегированный.
        if (claims->role != Role::Admin) {
            bool ok = false;
            for (const auto& g : claims->channel_grants) {
                if (g.channel_id != channel_id) continue;
                if (rule->channel_perm == ChannelPermission::Operate) {
                    ok = (g.permission == ChannelPermission::Operate);
                } else {
                    ok = true;  // any grant покрывает View
                }
                if (ok) break;
            }
            if (!ok) return Decision::Forbidden;
        }
    }

    if (out_ctx) {
        out_ctx->user_id              = claims->user_id;
        out_ctx->username             = claims->username;
        out_ctx->role                 = claims->role;
        out_ctx->must_change_password = claims->must_change_password;
        out_ctx->channel_grants       = claims->channel_grants;
    }
    return Decision::Allow;
}

}  // namespace liveqx::auth
