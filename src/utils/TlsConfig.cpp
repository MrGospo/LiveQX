// fix38 — TLS configuration parser.

#include "utils/TlsConfig.h"

#include <nlohmann/json.hpp>

namespace liveqx::tls {

const char* modeToString(Mode m) noexcept {
    switch (m) {
        case Mode::Auto:        return "auto";
        case Mode::Provided:    return "provided";
        case Mode::BehindProxy: return "behind_proxy";
        case Mode::Disabled:    return "disabled";
    }
    return "auto";
}

Mode modeFromString(std::string_view s, bool& ok) noexcept {
    ok = true;
    if (s == "auto")         return Mode::Auto;
    if (s == "provided")     return Mode::Provided;
    if (s == "behind_proxy") return Mode::BehindProxy;
    if (s == "disabled")     return Mode::Disabled;
    ok = false;
    return Mode::Auto;
}

namespace {

// Tolerant string-array reader: skips non-string elements with a warning.
std::vector<std::string> readStringArray(const nlohmann::json& arr,
                                         const char*           field,
                                         std::vector<std::string>& warnings) {
    std::vector<std::string> out;
    if (!arr.is_array()) return out;
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& el = arr[i];
        if (!el.is_string()) {
            warnings.emplace_back(std::string("tls.") + field + "[" +
                                  std::to_string(i) + "] is not a string, skipped");
            continue;
        }
        std::string s = el.get<std::string>();
        if (s.empty()) continue;
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

ParseResult parseConfig(const nlohmann::json& cfg) {
    ParseResult r;

    if (!cfg.is_object()) return r;
    auto it = cfg.find("tls");
    if (it == cfg.end()) return r;          // Auto mode default
    const auto& t = *it;
    if (!t.is_object()) {
        r.errors.emplace_back("tls must be an object");
        return r;
    }

    if (auto m_it = t.find("mode"); m_it != t.end()) {
        if (!m_it->is_string()) {
            r.errors.emplace_back("tls.mode must be a string");
        } else {
            bool ok = false;
            r.config.mode = modeFromString(m_it->get<std::string>(), ok);
            if (!ok) {
                r.errors.emplace_back(
                    "tls.mode must be one of: auto, provided, behind_proxy, "
                    "disabled (got '" + m_it->get<std::string>() + "')");
            }
        }
    }

    auto read_path = [&](const char* key, std::filesystem::path& dest) {
        auto p_it = t.find(key);
        if (p_it == t.end()) return;
        if (!p_it->is_string()) {
            r.errors.emplace_back(std::string("tls.") + key + " must be a string");
            return;
        }
        dest = p_it->get<std::string>();
    };
    read_path("cert_path", r.config.cert_path);
    read_path("key_path",  r.config.key_path);
    read_path("ca_path",   r.config.ca_path);

    if (auto a_it = t.find("san_extra"); a_it != t.end()) {
        r.config.san_extra = readStringArray(*a_it, "san_extra", r.warnings);
    }
    if (auto a_it = t.find("trust_proxy_cidrs"); a_it != t.end()) {
        r.config.trust_proxy_cidrs =
            readStringArray(*a_it, "trust_proxy_cidrs", r.warnings);
    }

    if (auto b_it = t.find("allow_insecure_bind"); b_it != t.end()) {
        if (!b_it->is_boolean()) {
            r.errors.emplace_back("tls.allow_insecure_bind must be a boolean");
        } else {
            r.config.allow_insecure_bind = b_it->get<bool>();
        }
    }

    // Mode-specific validation.
    if (r.config.mode == Mode::Provided) {
        if (r.config.cert_path.empty())
            r.errors.emplace_back("tls.cert_path is required when mode='provided'");
        if (r.config.key_path.empty())
            r.errors.emplace_back("tls.key_path is required when mode='provided'");
    }

    return r;
}

}  // namespace liveqx::tls
