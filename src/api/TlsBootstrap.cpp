// See api/TlsBootstrap.h. The body was moved out of main.cpp unchanged.

#include "api/TlsBootstrap.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>

#include "utils/Log.h"
#include "utils/Paths.h"
#include "utils/Tls.h"
#include "utils/TlsConfig.h"

bool serverCertDue(const std::filesystem::path& cert_path, std::int64_t now_unix) {
    namespace sct = liveqx::tls;
    std::error_code ec;
    if (!std::filesystem::exists(cert_path, ec)) return true;
    const auto ci = sct::readCertInfo(cert_path);
    if (ci.subject.empty()) return true;
    if (now_unix == 0)
        now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
    return ci.not_after_unix - now_unix < static_cast<std::int64_t>(kServerCertRenewBeforeDays) * 86400;
}

ServerCertRenewal renewServerCertIfDue(const std::filesystem::path& tls_dir,
                                       const std::vector<std::string>& san_extra,
                                       std::string& error, std::int64_t now_unix) {
    namespace sct = liveqx::tls;
    error.clear();
    if (!serverCertDue(tls_dir / "server.crt", now_unix)) return ServerCertRenewal::NotDue;
    const auto sans = sct::autoDetectSans(san_extra);
    if (sans.empty()) {
        error = "the list of names and addresses to put in the certificate is empty";
        return ServerCertRenewal::Failed;
    }
    const auto srv = sct::issueServerCert(tls_dir, "liveqx", sans);
    if (!srv.ok()) {
        error = srv.error;
        return ServerCertRenewal::Failed;
    }
    return ServerCertRenewal::Renewed;
}

TlsBindings resolveTlsBindings(const nlohmann::json& cfg,
                               const liveqx::paths::Paths& paths,
                               const std::string& api_bind,
                               bool& tls_failed) {
    namespace sct = liveqx::tls;
    TlsBindings out;
    out.bind = api_bind;
    auto pr = sct::parseConfig(cfg);
    for (const auto& w : pr.warnings) LOG_WARN("tls config: {}", w);
    if (!pr.ok()) {
        for (const auto& e : pr.errors) LOG_ERROR("tls config: {}", e);
        tls_failed = true;
        return out;
    }
    const auto& tcfg     = pr.config;
    const auto  tls_dir  = paths.tlsDir();
    out.mode      = sct::modeToString(tcfg.mode);
    out.san_extra = tcfg.san_extra;

    switch (tcfg.mode) {
    case sct::Mode::Auto: {
        std::error_code ec;
        std::filesystem::create_directories(tls_dir, ec);
        if (ec) {
            LOG_ERROR("tls: cannot create {}: {}",
                      tls_dir.string(), ec.message());
            tls_failed = true; return out;
        }
        auto ca = sct::ensureCa(tls_dir);
        if (!ca.ok()) {
            LOG_ERROR("tls: ensureCa failed: {}", ca.error);
            tls_failed = true; return out;
        }
        if (ca.generated) {
            LOG_INFO("tls: generated internal CA at {}",
                     ca.cert_path.string());
        } else {
            LOG_INFO("tls: reusing internal CA at {}",
                     ca.cert_path.string());
        }

        // Re-mint server.crt at boot when it is missing, unreadable or has
        // under kServerCertRenewBeforeDays left (the same rule the timer uses
        // while the server runs). Cheap (~5ms).
        const auto srv_cert_path = tls_dir / "server.crt";
        const bool need_reissue = serverCertDue(srv_cert_path);
        std::filesystem::path srv_cert, srv_key;
        if (need_reissue) {
            auto sans = sct::autoDetectSans(tcfg.san_extra);
            if (sans.empty()) {
                LOG_ERROR("tls: auto-detected SAN list is empty; "
                          "cannot issue server cert");
                tls_failed = true; return out;
            }
            auto srv = sct::issueServerCert(tls_dir,
                                            "liveqx", sans);
            if (!srv.ok()) {
                LOG_ERROR("tls: issueServerCert failed: {}", srv.error);
                tls_failed = true; return out;
            }
            srv_cert = srv.cert_path;
            srv_key  = srv.key_path;
            LOG_INFO("tls: issued server cert sans_dns={} sans_ip4={} "
                     "sans_ip6={}", sans.dns_names.size(),
                     sans.ip_v4.size(), sans.ip_v6.size());
        } else {
            srv_cert = srv_cert_path;
            srv_key  = tls_dir / "server.key";
            LOG_INFO("tls: reusing server cert at {}",
                     srv_cert.string());
        }
        out.cert_path = srv_cert;
        out.key_path  = srv_key;
        out.tls_dir   = tls_dir;
        break;
    }
    case sct::Mode::Provided: {
        auto vk = sct::verifyKeyPair(tcfg.cert_path, tcfg.key_path);
        if (!vk.ok) {
            LOG_ERROR("tls: provided cert+key invalid: {}", vk.error);
            tls_failed = true; return out;
        }
        out.cert_path = tcfg.cert_path;
        out.key_path  = tcfg.key_path;
        out.tls_dir   = tls_dir;  // /api/tls/* still usable
        LOG_INFO("tls: serving operator-provided cert {}",
                 tcfg.cert_path.string());
        break;
    }
    case sct::Mode::BehindProxy: {
        if (tcfg.trust_proxy_cidrs.empty()) {
            LOG_WARN("tls: behind_proxy mode without "
                     "trust_proxy_cidrs — every client will look like the "
                     "proxy (shared rate limits, proxy address in the audit "
                     "trail); X-Forwarded-For is not honoured");
        } else {
            out.trust_proxy_cidrs = tcfg.trust_proxy_cidrs;
            LOG_INFO("tls: behind_proxy mode — X-Forwarded-For honoured from "
                     "{} trusted proxy entry/ies", tcfg.trust_proxy_cidrs.size());
        }
        LOG_INFO("tls: serving plain HTTP (TLS terminates upstream)");
        break;
    }
    case sct::Mode::Disabled: {
        if (api_bind != "127.0.0.1" && api_bind != "::1" &&
            api_bind != "localhost" &&
            !tcfg.allow_insecure_bind) {
            LOG_ERROR("tls.mode=disabled with non-loopback bind={} "
                      "and allow_insecure_bind=false. Either set "
                      "api_bind=127.0.0.1 or "
                      "tls.allow_insecure_bind=true to opt-in.",
                      api_bind);
            tls_failed = true; return out;
        }
        LOG_WARN("tls: disabled — Control API serving plain HTTP. "
                 "Use only on a trusted network or for local dev.");
        break;
    }
    }
    return out;
}
