// fix22 commit 16/24 — LDAP client поверх libldap. См. LdapClient.h.

#include "auth/LdapClient.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#define LDAP_DEPRECATED 1   // ldap_simple_bind_s остаётся deprecated stable
#include <ldap.h>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

constexpr int kLdapVersion = LDAP_VERSION3;

const char* tlsModeName(LdapTlsMode m) noexcept {
    switch (m) {
        case LdapTlsMode::Plain:    return "plain";
        case LdapTlsMode::StartTls: return "starttls";
        case LdapTlsMode::Ldaps:    return "ldaps";
    }
    return "unknown";
}

// hostHasPort: примитивный детект «host:port» — ищем последний ':' и
// проверяем, что справа все цифры. IPv6 (`[::1]:636`) обрабатываем
// по '['.
bool hostHasPort(std::string_view host) {
    if (host.empty()) return false;
    if (host.front() == '[') {
        // IPv6 в [ ] — порт после ']:'
        const auto rb = host.find(']');
        if (rb == std::string_view::npos) return false;
        return rb + 1 < host.size() && host[rb + 1] == ':';
    }
    const auto colon = host.rfind(':');
    if (colon == std::string_view::npos) return false;
    for (size_t i = colon + 1; i < host.size(); ++i) {
        if (host[i] < '0' || host[i] > '9') return false;
    }
    return colon + 1 < host.size();
}

// ldap_initialize требует C-string URI; держим RAII-обёртку для handle.
struct LdapHandle {
    LDAP* h{nullptr};
    ~LdapHandle() {
        if (h) ldap_unbind_ext_s(h, nullptr, nullptr);
    }
    LdapHandle() = default;
    LdapHandle(const LdapHandle&)            = delete;
    LdapHandle& operator=(const LdapHandle&) = delete;
};

// Применяет общие опции (proto v3, network_timeout, no-referrals).
bool applyCommonOptions(LDAP* h, int network_timeout_sec) {
    int ver = kLdapVersion;
    if (ldap_set_option(h, LDAP_OPT_PROTOCOL_VERSION, &ver) != LDAP_OPT_SUCCESS)
        return false;
    int referrals = 0;
    if (ldap_set_option(h, LDAP_OPT_REFERRALS, &referrals) != LDAP_OPT_SUCCESS)
        return false;
    timeval tv{network_timeout_sec, 0};
    if (ldap_set_option(h, LDAP_OPT_NETWORK_TIMEOUT, &tv) != LDAP_OPT_SUCCESS)
        return false;
    if (ldap_set_option(h, LDAP_OPT_TIMEOUT, &tv) != LDAP_OPT_SUCCESS)
        return false;
    return true;
}

// Service-bind. password пустой → anonymous bind.
int simpleBind(LDAP* h, std::string_view dn, std::string_view password) {
    berval cred{};
    cred.bv_val = const_cast<char*>(password.data());
    cred.bv_len = password.size();
    return ldap_sasl_bind_s(h,
                            std::string(dn).c_str(),
                            LDAP_SASL_SIMPLE, &cred,
                            nullptr, nullptr, nullptr);
}

// Возвращает строковое значение первого элемента атрибута (или "").
std::string firstAttr(LDAP* h, LDAPMessage* entry, const char* attr) {
    berval** vals = ldap_get_values_len(h, entry, attr);
    if (!vals) return "";
    std::string out;
    if (vals[0]) {
        out.assign(vals[0]->bv_val, vals[0]->bv_len);
    }
    ldap_value_free_len(vals);
    return out;
}

std::vector<std::string> allAttrs(LDAP* h, LDAPMessage* entry, const char* attr) {
    berval** vals = ldap_get_values_len(h, entry, attr);
    if (!vals) return {};
    std::vector<std::string> out;
    for (size_t i = 0; vals[i]; ++i) {
        out.emplace_back(vals[i]->bv_val, vals[i]->bv_len);
    }
    ldap_value_free_len(vals);
    return out;
}

}  // namespace

LdapClient::LdapClient(LdapConfig cfg) : cfg_(std::move(cfg)) {}

std::string LdapClient::escapeFilter(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        switch (c) {
            case '\\': out += "\\5c"; break;
            case '*':  out += "\\2a"; break;
            case '(':  out += "\\28"; break;
            case ')':  out += "\\29"; break;
            case 0:    out += "\\00"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    // Не-ASCII / control — экранируем hex-octet'ом.
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string LdapClient::escapeDn(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        const bool is_first = (i == 0);
        const bool is_last  = (i == raw.size() - 1);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '+':  out += "\\+";  break;
            case ',':  out += "\\,";  break;
            case ';':  out += "\\;";  break;
            case '<':  out += "\\<";  break;
            case '>':  out += "\\>";  break;
            case '\\': out += "\\\\"; break;
            case 0:    out += "\\00"; break;
            case ' ':
            case '#':
                // RFC 4514: '#' нужно экранировать, только если
                // оно ведущее; ' ' — leading или trailing.
                if ((c == '#' && is_first) ||
                    (c == ' ' && (is_first || is_last))) {
                    out += '\\';
                }
                out += static_cast<char>(c);
                break;
            default:
                out += static_cast<char>(c);
        }
    }
    return out;
}

std::string LdapClient::buildUri(const LdapConfig& cfg) {
    std::string scheme = (cfg.tls_mode == LdapTlsMode::Ldaps) ? "ldaps://"
                                                              : "ldap://";
    std::string host = cfg.server;
    if (!hostHasPort(host)) {
        host += (cfg.tls_mode == LdapTlsMode::Ldaps) ? ":636" : ":389";
    }
    return scheme + host;
}

std::string LdapClient::renderUserFilter(std::string_view tmpl,
                                         std::string_view username) {
    const auto esc = escapeFilter(username);
    std::string out;
    out.reserve(tmpl.size() + esc.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (i + 1 < tmpl.size() && tmpl[i] == '%' && tmpl[i + 1] == 's') {
            out += esc;
            ++i;
        } else {
            out += tmpl[i];
        }
    }
    return out;
}

std::string LdapClient::validate(const LdapConfig& cfg) {
    if (!cfg.enabled)                  return "ldap not enabled";
    if (cfg.server.empty())            return "server is empty";
    if (cfg.base_dn.empty())           return "base_dn is empty";
    if (cfg.user_filter.empty())       return "user_filter is empty";
    if (cfg.user_filter.find("%s") == std::string::npos)
        return "user_filter must contain '%s' placeholder";
    return "";
}

LdapClient::AuthResult
LdapClient::authenticate(std::string_view username, std::string_view password) {
    AuthResult r;
    if (auto err = validate(cfg_); !err.empty()) {
        r.ok     = false;
        r.error  = err;
        r.reason = AuthResult::Reason::ConfigError;
        return r;
    }
    if (password.empty()) {
        // Anonymous bind по сети возвращает success на пустой пароль —
        // RFC 4513 §5.1.2 — эту дыру закрываем явно ДО запроса.
        r.error  = "empty password";
        r.reason = AuthResult::Reason::InvalidCredentials;
        return r;
    }

    const std::string uri = buildUri(cfg_);
    LdapHandle h;
    int rc = ldap_initialize(&h.h, uri.c_str());
    if (rc != LDAP_SUCCESS || !h.h) {
        r.error  = std::string("ldap_initialize: ") + ldap_err2string(rc);
        r.reason = AuthResult::Reason::ConnectionFailed;
        return r;
    }
    if (!applyCommonOptions(h.h, cfg_.network_timeout_sec)) {
        r.error  = "ldap_set_option failed";
        r.reason = AuthResult::Reason::ConnectionFailed;
        return r;
    }
    if (cfg_.tls_mode == LdapTlsMode::StartTls) {
        rc = ldap_start_tls_s(h.h, nullptr, nullptr);
        if (rc != LDAP_SUCCESS) {
            r.error  = std::string("StartTLS failed: ") + ldap_err2string(rc);
            r.reason = AuthResult::Reason::ConnectionFailed;
            return r;
        }
    }

    // (1) Service bind — для search-stage.
    rc = simpleBind(h.h, cfg_.bind_dn, cfg_.bind_password);
    if (rc != LDAP_SUCCESS) {
        r.error  = std::string("service bind: ") + ldap_err2string(rc);
        r.reason = AuthResult::Reason::BindServiceFailed;
        return r;
    }

    // (2) Search: base_dn + filter с подставленным username.
    const std::string filter = renderUserFilter(cfg_.user_filter, username);
    const char* attrs[] = {
        cfg_.email_attribute.c_str(),
        cfg_.group_attribute.c_str(),
        nullptr,
    };
    LDAPMessage* msg = nullptr;
    rc = ldap_search_ext_s(h.h,
                           cfg_.base_dn.c_str(), LDAP_SCOPE_SUBTREE,
                           filter.c_str(),
                           const_cast<char**>(attrs),
                           /*attrsonly=*/0,
                           nullptr, nullptr, nullptr, /*sizelimit=*/2,
                           &msg);
    if (rc != LDAP_SUCCESS) {
        r.error  = std::string("user search: ") + ldap_err2string(rc);
        r.reason = AuthResult::Reason::Other;
        if (msg) ldap_msgfree(msg);
        return r;
    }
    LDAPMessage* entry = ldap_first_entry(h.h, msg);
    if (!entry) {
        ldap_msgfree(msg);
        r.error  = "user not found";
        r.reason = AuthResult::Reason::UserNotFound;
        return r;
    }
    char* dn_c = ldap_get_dn(h.h, entry);
    if (!dn_c) {
        ldap_msgfree(msg);
        r.error  = "user has no DN";
        r.reason = AuthResult::Reason::Other;
        return r;
    }
    r.user_dn = dn_c;
    ldap_memfree(dn_c);
    r.email   = firstAttr(h.h, entry, cfg_.email_attribute.c_str());
    r.groups  = allAttrs(h.h, entry, cfg_.group_attribute.c_str());
    ldap_msgfree(msg);

    // (3) Re-bind в качестве user_dn — это password-verify.
    rc = simpleBind(h.h, r.user_dn, password);
    if (rc != LDAP_SUCCESS) {
        r.user_dn.clear();
        r.email.clear();
        r.groups.clear();
        r.error  = std::string("user bind: ") + ldap_err2string(rc);
        r.reason = AuthResult::Reason::InvalidCredentials;
        return r;
    }

    r.ok     = true;
    r.reason = AuthResult::Reason::Ok;
    return r;
}

LdapClient::PingResult LdapClient::ping() {
    PingResult p;
    if (auto err = validate(cfg_); !err.empty()) {
        p.error = err;
        return p;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string uri = buildUri(cfg_);

    LdapHandle h;
    int rc = ldap_initialize(&h.h, uri.c_str());
    if (rc != LDAP_SUCCESS || !h.h) {
        p.error = std::string("ldap_initialize: ") + ldap_err2string(rc);
        return p;
    }
    if (!applyCommonOptions(h.h, cfg_.network_timeout_sec)) {
        p.error = "ldap_set_option failed";
        return p;
    }
    if (cfg_.tls_mode == LdapTlsMode::StartTls) {
        rc = ldap_start_tls_s(h.h, nullptr, nullptr);
        if (rc != LDAP_SUCCESS) {
            p.error = std::string("StartTLS failed: ") + ldap_err2string(rc);
            return p;
        }
    }
    rc = simpleBind(h.h, cfg_.bind_dn, cfg_.bind_password);
    if (rc != LDAP_SUCCESS) {
        p.error = std::string("service bind: ") + ldap_err2string(rc);
        return p;
    }
    p.ok = true;
    p.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_DEBUG("LdapClient: ping ok in {}ms tls={}", p.latency_ms,
              tlsModeName(cfg_.tls_mode));
    return p;
}

}  // namespace liveqx::auth
