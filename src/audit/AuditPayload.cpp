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

}  // namespace liveqx::audit
