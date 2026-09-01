#include "audit/AuditPayload.h"

#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>

namespace liveqx::audit {
namespace {

constexpr std::string_view kSensitive[] = {
    "password", "passwd", "secret", "token", "apikey", "api_key",
    "private_key", "privatekey", "master_key", "masterkey",
    "sign_key", "signkey", "hmac_key", "hmackey",
    "ssh_key", "sshkey", "jwt", "bearer", "authorization",
    "session_id", "sessionid",
};

std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool isSensitiveKey(std::string_view key) {
    const std::string lk = toLower(key);
    for (auto s : kSensitive)
        if (lk.find(s) != std::string::npos) return true;
    return false;
}

}  // namespace

void redactSensitiveKeys(nlohmann::json& v) {
    if (v.is_object()) {
        for (auto& [k, val] : v.items()) {
            if (isSensitiveKey(k)) {
                val = "[REDACTED]";
            } else {
                redactSensitiveKeys(val);
            }
        }
    } else if (v.is_array()) {
        for (auto& e : v) redactSensitiveKeys(e);
    }
}

std::string buildAuditDetailsFromBody(const std::string& body,
                                      const std::string& content_type) {
    constexpr std::size_t kMaxBodyParse    = 64 * 1024;
    constexpr std::size_t kMaxDetailsBytes =  4 * 1024;
    if (body.empty()) return "{}";

    nlohmann::json out = nlohmann::json::object();
    out["body_size"] = static_cast<std::int64_t>(body.size());

    const bool is_json =
        content_type.find("application/json") != std::string::npos;
    if (!is_json) {
        out["body_note"] = "non_json";
        return out.dump();
    }
    if (body.size() > kMaxBodyParse) {
        out["body_note"] = "too_large";
        return out.dump();
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (...) {
        out["body_note"] = "parse_error";
        return out.dump();
    }
    redactSensitiveKeys(parsed);
    out["body"] = std::move(parsed);
    std::string dumped = out.dump();
    if (dumped.size() > kMaxDetailsBytes) {
        nlohmann::json trimmed = nlohmann::json::object();
        trimmed["body_size"] = static_cast<std::int64_t>(body.size());
        trimmed["body_note"] = "truncated";
        return trimmed.dump();
    }
    return dumped;
}

namespace {

// True if either input is not an object — in that case jsonDiff should
// wrap the pair as a leaf rather than recursing.
bool bothAreObjects(const nlohmann::json& a, const nlohmann::json& b) {
    return a.is_object() && b.is_object();
}

// Wrap a differing value pair as an audit-style leaf. Redacts values
// under sensitive keys so a password change surfaces as an event but
// the actual old/new values never touch the audit row.
nlohmann::json makeLeaf(const std::string& key,
                        const nlohmann::json& before,
                        const nlohmann::json& after) {
    nlohmann::json leaf = nlohmann::json::object();
    if (isSensitiveKey(key)) {
        leaf["before"] = "[REDACTED]";
        leaf["after"]  = "[REDACTED]";
    } else {
        leaf["before"] = before;
        leaf["after"]  = after;
    }
    return leaf;
}

nlohmann::json diffImpl(const nlohmann::json& before,
                        const nlohmann::json& after,
                        const std::unordered_set<std::string>& skip_keys) {
    nlohmann::json out = nlohmann::json::object();
    if (!bothAreObjects(before, after)) return out;

    // Union of keys — a missing side is represented as JSON null so the
    // caller sees a real add/remove instead of a silent skip.
    for (auto it = before.begin(); it != before.end(); ++it) {
        const std::string& k = it.key();
        if (skip_keys.count(k)) continue;
        const auto& b = it.value();
        if (after.contains(k)) {
            const auto& a = after.at(k);
            if (bothAreObjects(b, a)) {
                auto sub = diffImpl(b, a, skip_keys);
                if (!sub.empty()) out[k] = std::move(sub);
            } else if (b != a) {
                out[k] = makeLeaf(k, b, a);
            }
        } else {
            out[k] = makeLeaf(k, b, nlohmann::json());
        }
    }
    for (auto it = after.begin(); it != after.end(); ++it) {
        const std::string& k = it.key();
        if (skip_keys.count(k)) continue;
        if (!before.contains(k)) {
            out[k] = makeLeaf(k, nlohmann::json(), it.value());
        }
    }
    return out;
}

}  // namespace

nlohmann::json jsonDiff(const nlohmann::json& before,
                        const nlohmann::json& after,
                        const std::unordered_set<std::string>& skip_keys) {
    return diffImpl(before, after, skip_keys);
}

}  // namespace liveqx::audit
