// See api/TlsBootstrap.h. The body was moved out of main.cpp unchanged.

#include "api/TlsBootstrap.h"

#include <nlohmann/json.hpp>

#include <filesystem>

#include "utils/Log.h"
#include "utils/Paths.h"
#include "utils/Tls.h"
#include "utils/TlsConfig.h"

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

        // Re-mint server.crt every boot when missing, expired,
        // or older than 30 days. Cheap (~5ms) and keeps SAN
        // drift in sync with NIC/DNS changes.
        const auto srv_cert_path = tls_dir / "server.crt";
        bool need_reissue = !std::filesystem::exists(
            srv_cert_path, ec);
        if (!need_reissue) {
            auto ci = sct::readCertInfo(srv_cert_path);
            if (ci.subject.empty() || ci.days_remaining < 30) {
                need_reissue = true;
            }
        }
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
                     "trust_proxy_cidrs — X-Forwarded-* will not be "
                     "honoured (safe default)");
        } else {
            LOG_WARN("tls: behind_proxy mode — trust_proxy_cidrs "
                     "configured ({} entry/ies) but enforcement is "
                     "deferred to a follow-up commit; treating "
                     "every X-Forwarded-* as untrusted",
                     tcfg.trust_proxy_cidrs.size());
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
