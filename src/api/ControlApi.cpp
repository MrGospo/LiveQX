#include "api/ControlApi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <openssl/rand.h>
}

#include "encoding/NvencVideoEncoder.h"
#include "encoding/QsvVideoEncoder.h"
#include "encoding/VaapiVideoEncoder.h"

#include "api/ChannelManager.h"
#include "metrics/ChannelProfiler.h"
#include "api/MetricsCollector.h"
#include "api/RbacScope.h"
#include "api/SseFilter.h"
#include "auth/AuthService.h"
#include "auth/LdapClient.h"
#include "auth/LdapConfigRepo.h"
#include "auth/RbacMiddleware.h"
#include "auth/SmtpClient.h"
#include "auth/SmtpConfigRepo.h"
#include "auth/SntpClient.h"
#include "auth/TimeConfig.h"
#include "auth/TimeConfigRepo.h"
#include "auth/TimeSource.h"
#include "core/ChannelInstance.h"
#include "events/EventBus.h"
#include "gateway/GatewayManager.h"
#include "gateway/probe/PsiProbe.h"
#include "preview/PreviewManager.h"
#include "stress/StressService.h"
#include "plugins/PluginManager.h"
#include "mounts/MountManager.h"
#include "mounts/MountSpec.h"
#include "metrics/ChannelMetrics.h"
#include "metrics/HostMetrics.h"
#include "utils/Log.h"
#include "utils/NetIfaces.h"
#include "utils/Tls.h"

using nlohmann::json;
using R  = ChannelManager::Result;
using GR = liveqx::gateway::GatewayManager::Result;

struct ControlApi::Impl {
    // fix38 — server is base-class pointer so we can hold either
    // httplib::Server (HTTP) or httplib::SSLServer (HTTPS) without a
    // template explosion across every route registration site. Routes
    // are registered through the inherited Server interface, which
    // SSLServer reuses unchanged — only listen()/stop()/socket
    // handling differs internally.
    std::unique_ptr<httplib::Server>               server;
    std::string                                    bind_addr;
    bool                                           tls_enabled{false};
    std::thread                                    thread;
    std::atomic<bool>                              stopped{false};
    ChannelManager&                                manager;
    MetricsCollector*                              metrics;
    LivezOptions                                   livez;
    liveqx::gateway::GatewayManager*       gateways;
    liveqx::auth::AuthService*             auth;
    liveqx::auth::LdapConfigRepo*          ldap_repo;
    liveqx::auth::SmtpConfigRepo*          smtp_repo;
    liveqx::auth::RbacMiddleware*          rbac;
    liveqx::events::EventBus*              events;
    liveqx::preview::PreviewManager*       preview;
    liveqx::stress::StressService*         stress;
    liveqx::plugins::PluginManager*        plugins;
    liveqx::auth::MasterKey*               master_key;
    liveqx::mounts::MountManager*          mounts;
    liveqx::auth::TimeConfigRepo*          time_repo;
    liveqx::auth::TimeSourceManager*       time_src;
    liveqx::auth::ISntpClient*             sntp;
    TlsBindings                                    tls_bindings;
    std::function<void()>                          on_tls_reload;
    std::mutex                                     metrics_token_mu;

    Impl(ChannelManager& m, MetricsCollector* mc, LivezOptions lz,
         liveqx::gateway::GatewayManager* gw,
         liveqx::auth::AuthService* a,
         liveqx::auth::LdapConfigRepo* lr,
         liveqx::auth::SmtpConfigRepo* sr,
         liveqx::auth::RbacMiddleware* rb,
         liveqx::events::EventBus* ev,
         liveqx::preview::PreviewManager* pv,
         liveqx::stress::StressService* st,
         liveqx::plugins::PluginManager* pl,
         liveqx::auth::MasterKey* mk,
         liveqx::mounts::MountManager* mn,
         liveqx::auth::TimeConfigRepo* tr,
         liveqx::auth::TimeSourceManager* ts,
         liveqx::auth::ISntpClient* sn,
         const TlsBindings& tls)
        : bind_addr(tls.bind.empty() ? std::string{"0.0.0.0"} : tls.bind),
          manager(m), metrics(mc), livez(lz), gateways(gw), auth(a),
          ldap_repo(lr), smtp_repo(sr), rbac(rb), events(ev), preview(pv),
          stress(st), plugins(pl), master_key(mk), mounts(mn),
          time_repo(tr), time_src(ts), sntp(sn),
          tls_bindings(tls) {
        if (!tls.cert_path.empty() && !tls.key_path.empty()) {
            auto* ssl = new httplib::SSLServer(
                tls.cert_path.string().c_str(),
                tls.key_path.string().c_str());
            if (!ssl->is_valid()) {
                LOG_ERROR("SSLServer init failed (cert={} key={}); "
                          "falling back to HTTP",
                          tls.cert_path.string(), tls.key_path.string());
                delete ssl;
                server = std::make_unique<httplib::Server>();
            } else {
                server.reset(ssl);
                tls_enabled = true;
            }
        } else {
            server = std::make_unique<httplib::Server>();
        }
    }
};

namespace {

void writeJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

int statusFor(R r) {
    switch (r) {
        case R::Ok:             return 200;
        case R::NotFound:       return 404;
        case R::AlreadyExists:  return 409;
        case R::AlreadyRunning: return 409;
        case R::AlreadyStopped: return 409;
        case R::BadPatch:       return 400;
        case R::BuildFailed:    return 400;
        case R::StartFailed:    return 500;
        case R::ManagedByContentSync: return 409;
        case R::BadJson:        return 400;
        case R::ItemBuildFailed:return 400;
        case R::IndexOutOfRange:return 404;
        case R::PathNotFound:   return 404;
        case R::OutputIdConflict:  return 409;
        case R::OutputBuildFailed: return 400;
        case R::OutputStartFailed: return 500;
        case R::OutputNotFound:    return 404;
    }
    return 500;
}

void writeError(httplib::Response& res, R r) {
    writeJson(res, statusFor(r), {{"error", channelManagerResultName(r)}});
}

int gatewayStatusFor(GR r) {
    switch (r) {
        case GR::Ok:               return 200;
        case GR::NotFound:         return 404;
        case GR::AlreadyExists:    return 409;
        case GR::AlreadyRunning:   return 409;
        case GR::AlreadyStopped:   return 409;
        case GR::StartFailed:      return 500;
        case GR::BadJson:          return 400;
        case GR::BadPatch:         return 400;
        case GR::OutputIdConflict: return 409;
        case GR::OutputNotFound:   return 404;
    }
    return 500;
}

void writeGatewayError(httplib::Response& res, GR r) {
    using namespace liveqx::gateway;
    writeJson(res, gatewayStatusFor(r),
              {{"error", gatewayManagerResultName(r)}});
}

bool parseJsonBody(const httplib::Request& req, httplib::Response& res, json& out) {
    if (req.body.empty()) { out = json::object(); return true; }
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception& e) {
        writeJson(res, 400, {{"error", "invalid_json"}, {"detail", e.what()}});
        return false;
    }
}

bool parseId(const httplib::Request& req, httplib::Response& res, int& out) {
    try {
        out = std::stoi(req.matches[1]);
        return true;
    } catch (const std::exception&) {
        writeJson(res, 400, {{"error", "invalid_id"}});
        return false;
    }
}

// ── RBAC wiring (commit 24/24) ──────────────────────────────────────────
//
// Эта секция — единственное место в ядре, где REST-маршруты сопоставляются
// с правилами авторизации. Pre-routing handler перехватывает каждый
// запрос ДО роутинга, поэтому правила и реальный диспетчер остаются
// в синхроне просто за счёт парности «s.<METHOD>(path,…) ↔
// rbac.registerEndpoint("<METHOD> path",…)». Если в коде появится
// маршрут без правила — RbacMiddleware::Decision::NotConfigured даст 500
// с кодом rbac.misconfigured (default-deny + WARN-лог).

using RbacRule = liveqx::auth::RbacMiddleware::EndpointRule;
using RbacRole = liveqx::auth::Role;
using RbacPerm = liveqx::auth::ChannelPermission;

inline RbacRule rbacOpen() {
    RbacRule r;
    r.open = true;
    return r;
}

inline RbacRule rbacRole(RbacRole role) {
    RbacRule r;
    r.min_role = role;
    return r;
}

inline RbacRule rbacChan(RbacRole role, RbacPerm perm) {
    RbacRule r;
    r.min_role          = role;
    r.needs_channel_acl = true;
    r.channel_perm      = perm;
    return r;
}

void registerRbacRules(liveqx::auth::RbacMiddleware& rbac) {
    // ── Open / probes / login ─────────────────────────────────────────
    rbac.registerEndpoint("GET /healthz",          rbacOpen());
    rbac.registerEndpoint("GET /readyz",           rbacOpen());
    rbac.registerEndpoint("GET /livez",            rbacOpen());
    rbac.registerEndpoint("GET /api/health",       rbacOpen());
    rbac.registerEndpoint("GET /api/metrics",              rbacOpen());
    rbac.registerEndpoint("GET /api/metrics/token",        rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/metrics/token",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("DELETE /api/metrics/token",     rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/version",      rbacOpen());
    rbac.registerEndpoint("GET /api/status",       rbacOpen());
    rbac.registerEndpoint("POST /api/auth/login",   rbacOpen());
    rbac.registerEndpoint("POST /api/auth/refresh", rbacOpen());

    // ── Authenticated, any role ───────────────────────────────────────
    rbac.registerEndpoint("POST /api/auth/logout",      rbacRole(RbacRole::Viewer));
    rbac.registerEndpoint("GET /api/auth/me",           rbacRole(RbacRole::Viewer));
    rbac.registerEndpoint("POST /api/auth/me/password", rbacRole(RbacRole::Viewer));
    rbac.registerEndpoint("GET /api/auth/me/sessions",  rbacRole(RbacRole::Viewer));
    rbac.registerEndpoint("DELETE /api/auth/me/sessions/{jwt_id}",
                          rbacRole(RbacRole::Viewer));

    // ── Master-key metadata (fix32 B3) ────────────────────────────────
    rbac.registerEndpoint("GET /api/auth/master-key/info",
                          rbacRole(RbacRole::Admin));

    // ── Channel listing / creation ────────────────────────────────────
    rbac.registerEndpoint("GET /api/channels",  rbacRole(RbacRole::Viewer));
    rbac.registerEndpoint("POST /api/channels", rbacRole(RbacRole::Admin));

    // ── Channel-bound, View ACL ───────────────────────────────────────
    rbac.registerEndpoint("GET /api/channels/{id}",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/playlist",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/outputs",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/outputs/{oid}/status",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/watcher/status",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/live-status",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/schedule",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/schedule/active",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/schedule/upcoming",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/playback-log",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/playback-log/status",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    // fix23: WebRTC preview is read-only viewing of an existing channel —
    // a Viewer with channel.view ACL can negotiate offers and inspect
    // their own session stats. No mutation of state, no Operate needed.
    rbac.registerEndpoint("POST /api/channels/{id}/preview/offer",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("DELETE /api/channels/{id}/preview/sessions/{sid}",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("GET /api/channels/{id}/preview/stats",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));

    // ── Channel-bound, Operate ACL ────────────────────────────────────
    rbac.registerEndpoint("PUT /api/channels/{id}/config",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/play",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/stop",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/next",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/playlist",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/playlist/append",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("DELETE /api/channels/{id}/playlist",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("DELETE /api/channels/{id}/playback-log",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("DELETE /api/channels/{id}/playlist/{idx}",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/playlist/notify-deleted",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/outputs",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("DELETE /api/channels/{id}/outputs/{oid}",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("PATCH /api/channels/{id}/outputs/{oid}",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("PUT /api/channels/{id}/schedule",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/watcher/rescan",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    // fix26: profiler control. Read-only viewers can fetch the snapshot;
    // start/stop are operate-level since they change a channel's state.
    rbac.registerEndpoint("GET /api/channels/{id}/perf",
                          rbacChan(RbacRole::Viewer, RbacPerm::View));
    rbac.registerEndpoint("POST /api/channels/{id}/perf/start",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));
    rbac.registerEndpoint("POST /api/channels/{id}/perf/stop",
                          rbacChan(RbacRole::Operator, RbacPerm::Operate));

    // ── Channel-bound, Admin ──────────────────────────────────────────
    rbac.registerEndpoint("DELETE /api/channels/{id}",
                          rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/channels/{id}/permissions",
                          rbacRole(RbacRole::Admin));
    // Raw channel.log exposes internal diagnostic output (ffmpeg
    // warnings, decoder fallbacks, thread stack traces on assert).
    // Restrict to Admin — an Operator with a single-channel ACL
    // shouldn't see host-level diagnostics.
    rbac.registerEndpoint("GET /api/channels/{id}/logs",
                          rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/channels/{id}/logs/stream",
                          rbacRole(RbacRole::Admin));

    // ── Gateways ──────────────────────────────────────────────────────
    rbac.registerEndpoint("GET /api/gateways",            rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("POST /api/gateways",           rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/gateways/{id}",       rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("DELETE /api/gateways/{id}",    rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PATCH /api/gateways/{id}",     rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/gateways/{id}/play", rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("POST /api/gateways/{id}/stop", rbacRole(RbacRole::Operator));

    // ── Stream probe (PSI inspection for the mux composer UI) ─────────
    rbac.registerEndpoint("POST /api/streams/probe",      rbacRole(RbacRole::Operator));

    // ── System / network info ─────────────────────────────────────────
    rbac.registerEndpoint("GET /api/system/interfaces", rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/system/gpu",        rbacRole(RbacRole::Admin));
    // Host-resource dashboard (CPU/RAM/NIC/FS/disk-io). Snapshot GET is
    // for scripts / manual curl; the SSE stream drives the admin UI page
    // and is lazy-started per subscriber (no work when nobody is watching).
    rbac.registerEndpoint("GET /api/system/host_metrics",        rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/system/host_metrics/stream", rbacRole(RbacRole::Admin));
    // fix36: filesystem browse endpoint backs the UI folder picker for
    // share_path / hls_dir inputs. Admin-only — directory listing is
    // information disclosure on a multi-tenant box.
    rbac.registerEndpoint("GET /api/system/browse",     rbacRole(RbacRole::Admin));

    // fix41 — CIFS/NFS mounts. Mount config writes systemd units and
    // hands credentials to a privileged helper; Admin-only across the
    // board, no Operator carve-out.
    rbac.registerEndpoint("GET /api/system/mounts",         rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/system/mounts",        rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/system/mounts/test",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/system/mounts/{id}",    rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/system/mounts/{id}",    rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("DELETE /api/system/mounts/{id}", rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/system/mounts/{id}/sync",
                          rbacRole(RbacRole::Admin));

    // fix33 — server time / NTP config. GET — Operator (mostly read для
    // overview), PUT/POST/test — Admin (правка системного clock-источника).
    rbac.registerEndpoint("GET /api/system/time",       rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("PUT /api/system/time",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/system/time/test", rbacRole(RbacRole::Admin));

    // fix23: process-wide preview snapshot — admin diagnostic only.
    rbac.registerEndpoint("GET /api/preview", rbacRole(RbacRole::Admin));

    // fix19: plugin lifecycle. Install/uninstall ставят .so в наш адресный
    // прострочной — это RCE-as-a-feature, поэтому все 5 ручек строго Admin.
    // Default-deny RBAC бесплатно даёт правильный 403 для Operator/Viewer.
    rbac.registerEndpoint("GET /api/plugins",                       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/plugins/{name}",                rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/plugins/{name}/install",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("DELETE /api/plugins/{name}",             rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/plugins/{name}/eula/accept",   rbacRole(RbacRole::Admin));

    // fix26 c11: stress runner. View+List for Operator (so on-call can
    // read status/reports without admin), mutating ops + start/stop are
    // Admin only.
    rbac.registerEndpoint("GET /api/stress/config",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/stress/config",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/stress/start",       rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/stress/stop",        rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/stress/status",       rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("GET /api/stress/reports",         rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("GET /api/stress/reports/{id}",    rbacRole(RbacRole::Operator));
    rbac.registerEndpoint("DELETE /api/stress/reports/{id}", rbacRole(RbacRole::Admin));

    // ── Auth admin (users / audit / ldap / smtp) ──────────────────────
    rbac.registerEndpoint("GET /api/auth/users",         rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/users",        rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/auth/users/{uid}",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/auth/users/{uid}",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("DELETE /api/auth/users/{uid}",rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/users/{uid}/enable",         rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/users/{uid}/reset-password", rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/users/{uid}/unlock",         rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/users/{uid}/purge",          rbacRole(RbacRole::Admin));

    rbac.registerEndpoint("GET /api/auth/audit",         rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/audit/purge",  rbacRole(RbacRole::Admin));

    rbac.registerEndpoint("GET /api/auth/ldap/config",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/auth/ldap/config",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/ldap/test",    rbacRole(RbacRole::Admin));

    rbac.registerEndpoint("GET /api/auth/smtp/config",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/auth/smtp/config",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/auth/smtp/test",    rbacRole(RbacRole::Admin));

    rbac.registerEndpoint("GET /api/auth/users/{uid}/channels",
                          rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("PUT /api/auth/users/{uid}/channels/{cid}",
                          rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("DELETE /api/auth/users/{uid}/channels/{cid}",
                          rbacRole(RbacRole::Admin));

    // ── fix23: UI events stream (SSE) ────────────────────────────────
    // Любой авторизованный role видит stream; payload фильтруется
    // на стороне subscriber-thread по типу события + per-channel ACL.
    rbac.registerEndpoint("GET /api/events/stream", rbacRole(RbacRole::Viewer));

    // ── fix38: TLS administration ─────────────────────────────────────
    // info / ca-bundle: Admin (CA bundle is small, but operator-only).
    // regenerate-server / import: Admin (rotates the listener cert).
    rbac.registerEndpoint("GET /api/tls/info",                 rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("GET /api/tls/ca-bundle",            rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/tls/regenerate-server",   rbacRole(RbacRole::Admin));
    rbac.registerEndpoint("POST /api/tls/import",              rbacRole(RbacRole::Admin));
}

// Извлекает channel-id из URL ровно для тех маршрутов, где нужен ACL-чек.
// Возвращает -1, если канал не применим (RbacMiddleware пропустит ACL).
std::int64_t extractChannelIdFromPath(std::string_view path) {
    static const std::string p1 = "/api/channels/";
    if (path.size() > p1.size() && path.compare(0, p1.size(), p1) == 0) {
        std::size_t end = path.find('/', p1.size());
        if (end == std::string_view::npos) end = path.size();
        std::string seg(path.substr(p1.size(), end - p1.size()));
        try { return std::stoll(seg); } catch (...) { return -1; }
    }
    static const std::string p2 = "/api/auth/users/";
    if (path.size() > p2.size() && path.compare(0, p2.size(), p2) == 0) {
        std::size_t after_uid = path.find('/', p2.size());
        if (after_uid == std::string_view::npos) return -1;
        static const std::string p3 = "/channels/";
        if (path.compare(after_uid, p3.size(), p3) != 0) return -1;
        std::size_t cid_start = after_uid + p3.size();
        std::size_t end = path.find('/', cid_start);
        if (end == std::string_view::npos) end = path.size();
        std::string seg(path.substr(cid_start, end - cid_start));
        try { return std::stoll(seg); } catch (...) { return -1; }
    }
    return -1;
}

std::string generateMetricsToken() {
    std::array<unsigned char, 32> buf{};
    RAND_bytes(buf.data(), static_cast<int>(buf.size()));
    std::ostringstream oss;
    for (auto b : buf)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

std::string extractBearerToken(const httplib::Request& req) {
    auto v = req.get_header_value("Authorization");
    static constexpr std::string_view pfx = "Bearer ";
    if (v.size() > pfx.size() &&
        std::string_view(v).substr(0, pfx.size()) == pfx) {
        return v.substr(pfx.size());
    }
    return {};
}

void installRbacPreHandler(httplib::Server& s,
                           liveqx::auth::RbacMiddleware& rbac) {
    using Decision = liveqx::auth::RbacMiddleware::Decision;
    s.set_pre_routing_handler(
        [&rbac](const httplib::Request& req, httplib::Response& res) {
            // fix35 A3.5–A3.9 — static UI assets are public. Skip RBAC for
            // anything outside the API/probe namespaces; cpp-httplib's mount
            // point handler (and the SPA fallback in mountUi()) handles those.
            // Without this bypass, /, /assets/*, /channels/42 would all hit
            // Decision::NotConfigured and 500 with rbac.misconfigured.
            static constexpr std::array<std::string_view, 5> api_prefixes = {
                "/api/", "/healthz", "/readyz", "/livez", "/metrics",
            };
            const std::string_view path{req.path};
            bool is_api = false;
            for (auto p : api_prefixes) {
                if (path.starts_with(p)) { is_api = true; break; }
            }
            if (!is_api) {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            const auto channel_id = extractChannelIdFromPath(req.path);
            const auto bearer     = extractBearerToken(req);
            const auto d = rbac.authorize(req.method, req.path, bearer,
                                          channel_id, nullptr);
            switch (d) {
                case Decision::Allow:
                    return httplib::Server::HandlerResponse::Unhandled;
                case Decision::NotConfigured:
                    writeJson(res, 500, {{"error", "rbac.misconfigured"}});
                    return httplib::Server::HandlerResponse::Handled;
                case Decision::Unauthorized:
                    writeJson(res, 401, {{"error", "unauthorized"}});
                    return httplib::Server::HandlerResponse::Handled;
                case Decision::Forbidden:
                    writeJson(res, 403, {{"error", "forbidden"}});
                    return httplib::Server::HandlerResponse::Handled;
                case Decision::PasswordChangeRequired:
                    writeJson(res, 403,
                              {{"error", "password_change_required"}});
                    return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
}

}  // namespace

ControlApi::ControlApi(int port, ChannelManager& manager,
                       MetricsCollector* metrics,
                       LivezOptions livez,
                       liveqx::gateway::GatewayManager* gateways,
                       liveqx::auth::AuthService* auth,
                       liveqx::auth::LdapConfigRepo* ldap_repo,
                       liveqx::auth::SmtpConfigRepo* smtp_repo,
                       liveqx::auth::RbacMiddleware* rbac,
                       liveqx::events::EventBus* events,
                       liveqx::preview::PreviewManager* preview,
                       liveqx::stress::StressService* stress,
                       liveqx::plugins::PluginManager* plugins,
                       liveqx::auth::MasterKey* master_key,
                       liveqx::mounts::MountManager* mounts,
                       TlsBindings tls,
                       liveqx::auth::TimeConfigRepo* time_repo,
                       liveqx::auth::TimeSourceManager* time_src,
                       liveqx::auth::ISntpClient* sntp)
    : impl_(std::make_unique<Impl>(manager, metrics, livez, gateways, auth,
                                   ldap_repo, smtp_repo, rbac, events,
                                   preview, stress, plugins, master_key,
                                   mounts, time_repo, time_src, sntp, tls)),
      port_(port) {
    auto* impl = impl_.get();
    auto& s    = *impl_->server;
    auto& mgr  = impl_->manager;
    auto* mc   = impl_->metrics;
    auto* gws  = impl_->gateways;
    auto* au   = impl_->auth;
    auto* lr   = impl_->ldap_repo;
    auto* sr   = impl_->smtp_repo;
    auto* ev   = impl_->events;
    auto* pv   = impl_->preview;
    auto* st   = impl_->stress;
    auto* pl   = impl_->plugins;
    auto* mk   = impl_->master_key;
    auto* mn   = impl_->mounts;
    auto* tr   = impl_->time_repo;
    auto* ts   = impl_->time_src;
    auto* sn   = impl_->sntp;
    const double stuck_threshold_sec = impl_->livez.stuck_threshold_seconds;

    // fix16: Kubernetes-style probes. Open (no auth) — fix22 RBAC must keep
    // them open or oncall pages will misfire on token rotation.

    // /healthz — process-alive only. If the main thread can route here, it's
    // alive. Stuck render threads are caught by /livez; not-yet-loaded state
    // is caught by /readyz.
    s.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        writeJson(res, 200, {{"status", "ok"}});
    });

    // /readyz — ready to receive traffic. 503 with reasons[] until every
    // gate (channels-on-disk loaded, fix17 state restored, fix22 auth db
    // up) has flipped green.
    s.Get("/readyz", [&mgr](const httplib::Request&, httplib::Response& res) {
        json reasons = json::array();
        if (!mgr.isLoaded())                  reasons.push_back("channels_loading");
        if (!mgr.isStatePersistenceLoaded())  reasons.push_back("state_persistence_loading");
        if (!mgr.isAuthLoaded())              reasons.push_back("auth_loading");
        if (reasons.empty()) {
            writeJson(res, 200, {{"status", "ready"}});
        } else {
            writeJson(res, 503, {
                {"status",  "not_ready"},
                {"reasons", std::move(reasons)},
            });
        }
    });

    // /livez — liveness with self-check. For each running channel, refuses
    // (500) if its last render tick is older than `stuck_threshold_seconds`.
    // No running channels → 200 (the engine is up; there's just nothing to
    // watch). Reads ChannelMetrics::last_tick_ns directly — Watchdog already
    // computes the same delta but kicks at 1Hz, which is too coarse for an
    // on-demand probe.
    s.Get("/livez", [&mgr, stuck_threshold_sec](const httplib::Request&,
                                                httplib::Response& res) {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        const std::int64_t threshold_ns =
            static_cast<std::int64_t>(stuck_threshold_sec * 1e9);

        json stuck = json::array();
        mgr.forEachChannel([&](const ChannelInstance& ch) {
            if (!ch.isRunning()) return;
            const auto m = ch.metrics();
            if (!m) return;
            const std::int64_t last = m->last_tick_ns.load(std::memory_order_relaxed);
            if (last <= 0) return;  // pre-first-tick — Watchdog has its own grace
            if (now_ns - last > threshold_ns)
                stuck.push_back(ch.id());
        });

        if (stuck.empty()) {
            writeJson(res, 200, {{"status", "alive"}});
        } else {
            writeJson(res, 500, {
                {"status",         "unhealthy"},
                {"stuck_channels", std::move(stuck)},
            });
        }
    });

    s.Get("/api/health", [&mgr](const httplib::Request&, httplib::Response& res) {
        const auto body = mgr.healthJson();
        const auto& chs = body["channels"];
        bool any_failed = body.value("overall", "") == std::string("failed");
        writeJson(res, any_failed ? 503 : 200, body);
        (void)chs;
    });

    // Both /api/metrics and /api/status are registered as `rbacOpen()`
    // (Prometheus scrape and unauthenticated probes still work). When a
    // valid Bearer token IS supplied, we apply role-based filtering so a
    // viewer can't curl the endpoint and read every channel's name/fps.
    //
    //   anonymous (no Bearer)     → full view
    //   bad/expired token         → full view (handler doesn't re-deny;
    //                               operationally, the token would have
    //                               failed at /api/auth/login)
    //   valid Admin / Operator    → full view
    //   valid Viewer              → only channels in channel_grants
    //
    // We only filter when authorize() returns Allow with a populated ctx;
    // any other decision falls through to the unfiltered path so the
    // public/probe semantics of these endpoints are preserved.
    auto* rb_scope = impl_->rbac;
    auto scopeForRequest =
        [rb_scope](const httplib::Request& req,
                   liveqx::api::ChannelScope& out) -> bool {
        if (!rb_scope) return false;
        const auto bearer = extractBearerToken(req);
        if (bearer.empty()) return false;
        liveqx::auth::RequestContext ctx;
        const auto d = rb_scope->authorize(req.method, req.path, bearer,
                                           /*channel_id=*/-1, &ctx);
        if (d != liveqx::auth::RbacMiddleware::Decision::Allow) {
            return false;
        }
        // Open routes (e.g. /api/status) return Allow without populating ctx
        // (user_id stays 0). Treat them as "no scope" so all channels are visible.
        if (ctx.user_id <= 0) return false;
        out = liveqx::api::makeChannelScope(ctx);
        return true;
    };

    s.Get("/api/metrics",
          [mc, scopeForRequest, livez](const httplib::Request& req, httplib::Response& res) {
        if (!livez.metrics_bearer_token.empty()) {
            const auto token = extractBearerToken(req);
            if (token != livez.metrics_bearer_token) {
                res.status = 401;
                res.set_header("WWW-Authenticate", "Bearer realm=\"metrics\"");
                res.set_content("unauthorized\n", "text/plain");
                return;
            }
        }
        if (!mc) {
            res.status = 503;
            res.set_content("metrics_collector_not_configured\n", "text/plain");
            return;
        }
        liveqx::api::ChannelScope scope;
        const bool have = scopeForRequest(req, scope);
        res.status = 200;
        res.set_content(mc->renderPrometheus(have ? &scope : nullptr),
                        "text/plain; version=0.0.4; charset=utf-8");
    });

    // ── Metrics bearer token management (Admin only) ───────────────────────
    s.Get("/api/metrics/token",
          [impl](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(impl->metrics_token_mu);
        const auto& t = impl->livez.metrics_bearer_token;
        if (t.empty()) {
            writeJson(res, 200, {{"configured", false}});
        } else {
            writeJson(res, 200, {
                {"configured", true},
                {"preview",    t.substr(0, 8) + "…"},
            });
        }
    });

    s.Post("/api/metrics/token",
           [impl](const httplib::Request& req, httplib::Response& res) {
        std::string new_token;
        if (req.body.empty()) {
            new_token = generateMetricsToken();
        } else {
            json body;
            if (!parseJsonBody(req, res, body)) return;
            if (body.value("generate", false)) {
                new_token = generateMetricsToken();
            } else if (body.contains("token") && body["token"].is_string()) {
                new_token = body["token"].get<std::string>();
                if (new_token.size() < 16) {
                    writeJson(res, 400, {{"error", "token too short (min 16 chars)"}});
                    return;
                }
            } else {
                writeJson(res, 400, {{"error", "provide {generate:true} or {token:\"...\"}"}});
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(impl->metrics_token_mu);
            if (impl->livez.on_metrics_token_changed
                && !impl->livez.on_metrics_token_changed(new_token)) {
                writeJson(res, 500, {{"error", "could not persist metrics token"}});
                return;
            }
            impl->livez.metrics_bearer_token = new_token;
        }
        writeJson(res, 200, {{"token", new_token}, {"configured", true}});
    });

    s.Delete("/api/metrics/token",
             [impl](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(impl->metrics_token_mu);
        if (impl->livez.on_metrics_token_changed
            && !impl->livez.on_metrics_token_changed(std::string{})) {
            writeJson(res, 500, {{"error", "could not persist metrics token"}});
            return;
        }
        impl->livez.metrics_bearer_token.clear();
        writeJson(res, 200, {{"configured", false}});
    });

    s.Get("/api/version", [mc](const httplib::Request&, httplib::Response& res) {
        if (!mc) { writeJson(res, 503, {{"error", "metrics_collector_not_configured"}}); return; }
        writeJson(res, 200, mc->renderVersion());
    });

    s.Get("/api/status",
          [mc, scopeForRequest](const httplib::Request& req, httplib::Response& res) {
        if (!mc) { writeJson(res, 503, {{"error", "metrics_collector_not_configured"}}); return; }
        liveqx::api::ChannelScope scope;
        const bool have = scopeForRequest(req, scope);
        writeJson(res, 200, mc->renderStatus(have ? &scope : nullptr));
    });

    // ── fix23: SSE event stream ───────────────────────────────────────────
    // GET /api/events/stream  Accept: text/event-stream
    //
    // Опциональные query / headers:
    //   ?types=clip_change,output_state_change   фильтр по типам
    //   Last-Event-ID: <int>                     replay из ring buffer
    //
    // Без ?types=... включён default subscription: часть шумных типов
    // (сейчас — clip_change) в дефолтную ленту не попадает, чтобы не
    // затопить страницу «События». Клиенты, которым эти типы нужны
    // (LogTab канала), подписываются явно через ?types=clip_change.
    //
    // RBAC pre-handler уже проверил Bearer + role.viewer. Здесь мы ещё раз
    // авторизуем (дёшево — те же rules в hash-map), чтобы вытащить
    // RequestContext (role + channel_grants) и применять per-event фильтр
    // (sseEventVisibleTo). Pre-handler не передаёт ctx наружу — это
    // ограничение httplib, не RBAC'а.
    //
    // Lifecycle: provider lambda вызывается воркером httplib в цикле; на
    // каждом тике drain'ит до 15с с CV — если за это время что-то
    // прилетело, пушим SSE-блок; иначе шлём ': keepalive\n\n' чтобы
    // прокси не оборвал idle connection. Ошибка sink.write (клиент
    // дисконнектнулся) или overflow подписки → провайдер возвращает
    // false и httplib закрывает chunked stream.
    auto* rb = impl_->rbac;
    s.Get("/api/events/stream",
          [ev, rb](const httplib::Request& req, httplib::Response& res) {
        if (!ev) {
            writeJson(res, 503, {{"error", "events_bus_not_configured"}});
            return;
        }

        // ── re-authorize to obtain RequestContext (role + channel_grants).
        // Pre-handler already returned 401/403 for invalid tokens, so this
        // path should always succeed; on the off-chance it doesn't (race
        // with token revoke between pre-handler and here) we deny.
        liveqx::auth::RequestContext ctx;
        if (rb) {
            const auto bearer = extractBearerToken(req);
            const auto d = rb->authorize(req.method, req.path, bearer,
                                         /*channel_id=*/-1, &ctx);
            if (d != liveqx::auth::RbacMiddleware::Decision::Allow) {
                writeJson(res, 401, {{"error", "unauthorized"}});
                return;
            }
        } else {
            // RBAC disabled (test build) → admin-equivalent.
            ctx.role = liveqx::auth::Role::Admin;
        }

        // ── parse ?types= ────────────────────────────────────────────────
        std::vector<liveqx::events::EventType> filter;
        if (req.has_param("types")) {
            const auto& csv = req.get_param_value("types");
            std::size_t i = 0;
            while (i < csv.size()) {
                std::size_t j = csv.find(',', i);
                if (j == std::string::npos) j = csv.size();
                std::string token = csv.substr(i, j - i);
                if (auto t = liveqx::events::parseEventType(token)) {
                    filter.push_back(*t);
                }
                i = j + 1;
            }
        }
        auto matches = [filter](liveqx::events::EventType t) {
            if (filter.empty()) {
                // Default subscription: hide loud per-clip traffic. Clients
                // that want it must ask for it via ?types=clip_change (the
                // channel Log tab does exactly that).
                return liveqx::api::sseEventInDefaultSubscription(t);
            }
            for (auto f : filter) if (f == t) return true;
            return false;
        };

        // ── parse Last-Event-ID ─────────────────────────────────────────
        std::optional<std::uint64_t> since_id;
        if (req.has_header("Last-Event-ID")) {
            try {
                since_id = std::stoull(req.get_header_value("Last-Event-ID"));
            } catch (...) {
                /* ignore malformed — start from now */
            }
        }

        auto sub = ev->subscribe(since_id);

        // SSE response headers via cpp-httplib chunked provider.
        res.set_header("Cache-Control",       "no-cache");
        res.set_header("X-Accel-Buffering",   "no"); // disable nginx buffering
        res.set_chunked_content_provider(
            "text/event-stream",
            [sub, matches, ctx](size_t, httplib::DataSink& sink) -> bool {
                using namespace std::chrono_literals;
                auto evs = sub->drain(15s);

                // Slow-consumer guard: queue overflowed → tell client to
                // reconnect with last seen id and abort the stream.
                if (sub->overflowed()) {
                    const std::string err =
                        "event: error\n"
                        "data: {\"error\":\"event_bus_overflow\"}\n\n";
                    sink.write(err.c_str(), err.size());
                    sink.done();
                    return false;
                }

                if (evs.empty()) {
                    // Keepalive comment (SSE — lines starting with ':' are
                    // ignored by the EventSource API but keep proxy paths
                    // and TCP connection live).
                    static constexpr char kKeepalive[] = ": keepalive\n\n";
                    return sink.write(kKeepalive, sizeof(kKeepalive) - 1);
                }

                std::string payload;
                payload.reserve(256 * evs.size());
                for (const auto& e : evs) {
                    if (!matches(e.type))                                  continue;
                    if (!liveqx::api::sseEventVisibleTo(ctx, e))   continue;
                    payload += "id: ";
                    payload += std::to_string(e.id);
                    payload += '\n';
                    payload += "event: ";
                    payload += liveqx::events::eventTypeName(e.type);
                    payload += '\n';
                    payload += "data: ";
                    // Inject `type` and `ts` into the data JSON so a single
                    // payload parse on the client yields a self-describing
                    // event. The SSE `event:` line is consumed by EventSource
                    // but not by our fetch+ReadableStream parser's onMessage
                    // path, which only feeds `data` into JSON.parse.
                    nlohmann::json wire = e.payload.is_object()
                        ? e.payload
                        : nlohmann::json::object();
                    wire["type"] = liveqx::events::eventTypeName(e.type);
                    wire["ts"]   = e.ts_unix_ms;
                    payload += wire.dump();
                    payload += "\n\n";
                }
                if (payload.empty()) {
                    // Filtered out — still send keepalive so we don't
                    // miss the keepalive cadence.
                    static constexpr char kKeepalive[] = ": keepalive\n\n";
                    return sink.write(kKeepalive, sizeof(kKeepalive) - 1);
                }
                return sink.write(payload.data(), payload.size());
            });
    });

    // Channel-mutation audit helpers. Placed inline (rather than reusing
    // the actorContext lambda at ~L2279) because the channel endpoints
    // predate the auth block lexically and we don't want the extra file
    // hop for readers tracing an /api/channels/... call. Same shape as
    // pluginActorOf / mountActorOf: {user_id, username}, empty on absent
    // bearer so pre-RBAC tests keep working. emitChannelAudit is a
    // fire-and-forget wrapper — DB write + EventBus fan-out happen in
    // AuthService::emitAudit, this is only the actor plumbing.
    auto channelActorOf = [au](const httplib::Request& req)
        -> std::pair<std::optional<std::int64_t>, std::string> {
        if (!au || !req.has_header("Authorization")) return {std::nullopt, ""};
        const auto v = req.get_header_value("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (v.rfind(kPrefix, 0) != 0) return {std::nullopt, ""};
        auto claims = au->verifyActiveAccess(v.substr(7));
        if (!claims) return {std::nullopt, ""};
        return {claims->user_id, claims->username};
    };
    auto emitChannelAudit = [au](std::string_view event,
                                 const std::optional<std::int64_t>& uid,
                                 std::string_view username,
                                 std::string_view ip,
                                 const json& details) {
        if (!au) return;
        au->emitAudit(event, uid, username, ip, details.dump());
    };

    s.Get("/api/channels", [&mgr](const httplib::Request&, httplib::Response& res) {
        writeJson(res, 200, mgr.listJson());
    });

    s.Post("/api/channels",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseJsonBody(req, res, body)) return;
        int id = 0;
        const auto r = mgr.create(body, &id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("channel.created", uid, uname, req.remote_addr,
                         {{"channel_id", id},
                          {"name", body.value("name", std::string{})}});
        writeJson(res, 201, {{"id", id}});
    });

    s.Get(R"(/api/channels/(\d+))",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        auto status = mgr.statusJson(id);
        if (status.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, status);
    });

    s.Delete(R"(/api/channels/(\d+))",
             [&mgr, channelActorOf, emitChannelAudit]
             (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        // Snapshot name BEFORE remove — post-delete statusJson is null.
        std::string name;
        if (auto snap = mgr.statusJson(id); !snap.is_null()) {
            name = snap.value("name", std::string{});
        }
        const auto r = mgr.remove(id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("channel.deleted", uid, uname, req.remote_addr,
                         {{"channel_id", id}, {"name", name}});
        res.status = 204;
    });

    s.Put(R"(/api/channels/(\d+)/config)",
          [&mgr, channelActorOf, emitChannelAudit]
          (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json patch;
        if (!parseJsonBody(req, res, patch)) return;
        if (patch.contains("name")) {
            writeJson(res, 400, json{{"error",
                "name is immutable; recreate the channel to rename"}});
            return;
        }
        const auto r = mgr.updateConfig(id, patch);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        // Log the keys the operator touched — not the full patch, so a
        // secret-bearing field (SRT passphrase, if that ever migrated to
        // updateConfig) doesn't land in the audit trail.
        std::vector<std::string> keys;
        if (patch.is_object()) {
            keys.reserve(patch.size());
            for (auto it = patch.begin(); it != patch.end(); ++it)
                keys.push_back(it.key());
        }
        emitChannelAudit("channel.updated", uid, uname, req.remote_addr,
                         {{"channel_id", id}, {"fields", keys}});
        writeJson(res, 200, mgr.statusJson(id));
    });

    s.Post(R"(/api/channels/(\d+)/play)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.play(id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("channel.play", uid, uname, req.remote_addr,
                         {{"channel_id", id}});
        writeJson(res, 200, mgr.statusJson(id));
    });

    s.Post(R"(/api/channels/(\d+)/stop)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.stop(id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("channel.stop", uid, uname, req.remote_addr,
                         {{"channel_id", id}});
        writeJson(res, 200, mgr.statusJson(id));
    });

    s.Post(R"(/api/channels/(\d+)/next)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.next(id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("channel.next", uid, uname, req.remote_addr,
                         {{"channel_id", id}});
        writeJson(res, 200, {{"ok", true}});
    });

    // ── Playlist endpoints ──────────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/playlist)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        auto pl = mgr.playlistJson(id);
        if (pl.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, pl);
    });

    s.Post(R"(/api/channels/(\d+)/playlist)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_array()) { writeError(res, R::BadJson); return; }
        const auto items = body.size();
        const auto r = mgr.replacePlaylist(id, body);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("playlist.replaced", uid, uname, req.remote_addr,
                         {{"channel_id", id}, {"items", items}});
        writeJson(res, 200, mgr.playlistJson(id));
    });

    s.Post(R"(/api/channels/(\d+)/playlist/append)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_array()) { writeError(res, R::BadJson); return; }
        int first_idx = -1;
        const auto items = body.size();
        const auto r = mgr.appendPlaylist(id, body, &first_idx);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("playlist.appended", uid, uname, req.remote_addr,
                         {{"channel_id", id},
                          {"items", items},
                          {"first_idx", first_idx}});
        writeJson(res, 200, {{"first_idx", first_idx},
                             {"playlist",  mgr.playlistJson(id)}});
    });

    s.Delete(R"(/api/channels/(\d+)/playlist/(\d+))",
             [&mgr, channelActorOf, emitChannelAudit]
             (const httplib::Request& req, httplib::Response& res) {
        int id = 0;
        try { id = std::stoi(req.matches[1]); }
        catch (...) { writeJson(res, 400, {{"error", "invalid_id"}}); return; }
        int idx = 0;
        try { idx = std::stoi(req.matches[2]); }
        catch (...) { writeJson(res, 400, {{"error", "invalid_index"}}); return; }
        bool was_active = false;
        const auto r = mgr.removeAt(id, idx, &was_active);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("playlist.item_removed", uid, uname, req.remote_addr,
                         {{"channel_id", id},
                          {"index", idx},
                          {"was_active", was_active}});
        writeJson(res, 200, {{"was_active", was_active},
                             {"playlist",   mgr.playlistJson(id)}});
    });

    s.Delete(R"(/api/channels/(\d+)/playlist)",
             [&mgr, channelActorOf, emitChannelAudit]
             (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.clearPlaylist(id);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("playlist.cleared", uid, uname, req.remote_addr,
                         {{"channel_id", id}});
        res.status = 204;
    });

    s.Post(R"(/api/channels/(\d+)/playlist/notify-deleted)",
           [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object() || !body.contains("path") || !body["path"].is_string()) {
            writeError(res, R::BadJson); return;
        }
        const auto r = mgr.notifyDeleted(id, body["path"].get<std::string>());
        if (r != R::Ok) { writeError(res, r); return; }
        writeJson(res, 200, mgr.playlistJson(id));
    });

    // ── Outputs (fix12 c4) ──────────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/outputs)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.outputsJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    s.Post(R"(/api/channels/(\d+)/outputs)",
           [&mgr, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        const auto r = mgr.addOutput(id, body);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("output.added", uid, uname, req.remote_addr,
                         {{"channel_id", id},
                          {"kind", body.value("kind", std::string{})},
                          {"output_id", body.value("id", std::string{})}});
        writeJson(res, 201, mgr.outputsJson(id));
    });

    s.Delete(R"(/api/channels/(\d+)/outputs/([^/]+))",
             [&mgr, channelActorOf, emitChannelAudit]
             (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const std::string oid = req.matches[2];
        const auto r = mgr.removeOutput(id, oid);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("output.removed", uid, uname, req.remote_addr,
                         {{"channel_id", id}, {"output_id", oid}});
        writeJson(res, 200, mgr.outputsJson(id));
    });

    s.Get(R"(/api/channels/(\d+)/outputs/([^/]+)/status)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const std::string oid = req.matches[2];
        const auto j = mgr.outputStatusJson(id, oid);
        if (j.is_null()) { writeError(res, R::OutputNotFound); return; }
        writeJson(res, 200, j);
    });

    s.Patch(R"(/api/channels/(\d+)/outputs/([^/]+))",
            [&mgr, channelActorOf, emitChannelAudit]
            (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const std::string oid = req.matches[2];
        json body;
        if (!parseJsonBody(req, res, body)) return;
        const auto r = mgr.patchOutput(id, oid, body);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        std::vector<std::string> keys;
        if (body.is_object()) {
            keys.reserve(body.size());
            for (auto it = body.begin(); it != body.end(); ++it)
                keys.push_back(it.key());
        }
        emitChannelAudit("output.updated", uid, uname, req.remote_addr,
                         {{"channel_id", id},
                          {"output_id", oid},
                          {"fields", keys}});
        writeJson(res, 200, mgr.outputsJson(id));
    });

    s.Get(R"(/api/channels/(\d+)/watcher/status)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.watcherStatus(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // ── Live inputs (fix13 c8) ──────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/live-status)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.liveStatusJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, {{"channel_id", id}, {"live_inputs", j}});
    });

    s.Post(R"(/api/channels/(\d+)/watcher/rescan)",
           [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.requestRescan(id);
        if (r != R::Ok) { writeError(res, r); return; }
        writeJson(res, 202, {{"scheduled", true}});
    });

    // ── Profiler (fix26) ────────────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/perf)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.profilerSnapshotJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    s.Post(R"(/api/channels/(\d+)/perf/start)",
           [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        // mode comes from query (?mode=) or JSON body {"mode":"..."}.
        std::string mode_str = req.get_param_value("mode");
        bool reset = req.get_param_value("reset") == "true";
        if (!req.body.empty()) {
            json body;
            if (!parseJsonBody(req, res, body)) return;
            if (body.is_object()) {
                if (body.contains("mode") && body["mode"].is_string())
                    mode_str = body["mode"].get<std::string>();
                if (body.contains("reset") && body["reset"].is_boolean())
                    reset = body["reset"].get<bool>();
            }
        }
        if (mode_str.empty()) {
            writeJson(res, 400, {{"error", "missing 'mode' (sampling|instrumentation)"}});
            return;
        }
        const auto m = liveqx::profiler::parseMode(mode_str);
        if (m == liveqx::profiler::Mode::Off) {
            writeJson(res, 400,
                {{"error", "invalid mode — use 'sampling' or 'instrumentation'"}});
            return;
        }
        const auto r = mgr.profilerStart(id, m, reset);
        if (r != R::Ok) { writeError(res, r); return; }
        writeJson(res, 200, mgr.profilerSnapshotJson(id));
    });

    s.Post(R"(/api/channels/(\d+)/perf/stop)",
           [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = mgr.profilerStop(id);
        if (r != R::Ok) { writeError(res, r); return; }
        writeJson(res, 200, mgr.profilerSnapshotJson(id));
    });

    // ── Stress (fix26 c11) ──────────────────────────────────────────────────
    // Endpoints return 503 if no StressService was wired in (e.g. running
    // a stripped binary or in tests that pass nullptr).
    auto stressUnavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "stress_disabled"}});
    };

    s.Get("/api/stress/config",
          [st, stressUnavailable](const httplib::Request&, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        writeJson(res, 200, st->configJson());
    });

    s.Put("/api/stress/config",
          [st, stressUnavailable, channelActorOf, emitChannelAudit]
          (const httplib::Request& req, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        std::string err;
        if (!st->setConfigJson(body, &err)) {
            writeJson(res, 400, {{"error", "invalid_config"}, {"detail", err}});
            return;
        }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("stress.config_updated", uid, uname, req.remote_addr,
                         json::object());
        writeJson(res, 200, st->configJson());
    });

    s.Post("/api/stress/start",
           [st, stressUnavailable, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        std::string err;
        if (!st->startNow(&err)) {
            writeJson(res, 409, {{"error", "cannot_start"}, {"detail", err}});
            return;
        }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("stress.started", uid, uname, req.remote_addr,
                         json::object());
        writeJson(res, 202, st->statusJson());
    });

    s.Post("/api/stress/stop",
           [st, stressUnavailable, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        st->stopNow();
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("stress.stopped", uid, uname, req.remote_addr,
                         json::object());
        writeJson(res, 200, st->statusJson());
    });

    s.Get("/api/stress/status",
          [st, stressUnavailable](const httplib::Request&, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        writeJson(res, 200, st->statusJson());
    });

    s.Get("/api/stress/reports",
          [st, stressUnavailable](const httplib::Request&, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        const auto j = st->listReportsJson();
        if (j.is_null()) {
            writeJson(res, 200, json::array());
        } else {
            writeJson(res, 200, j);
        }
    });

    s.Get(R"(/api/stress/reports/([A-Za-z0-9_\-]+))",
          [st, stressUnavailable](const httplib::Request& req, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        const auto id = std::string(req.matches[1]);
        auto j = st->readReportJson(id);
        if (!j) { writeJson(res, 404, {{"error", "not_found"}}); return; }
        writeJson(res, 200, *j);
    });

    s.Delete(R"(/api/stress/reports/([A-Za-z0-9_\-]+))",
          [st, stressUnavailable, channelActorOf, emitChannelAudit]
          (const httplib::Request& req, httplib::Response& res) {
        if (!st) { stressUnavailable(res); return; }
        const auto id = std::string(req.matches[1]);
        if (!st->removeReport(id)) { writeJson(res, 404, {{"error", "not_found"}}); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("stress.report_deleted", uid, uname, req.remote_addr,
                         {{"report_id", id}});
        res.status = 204;
    });

    // ── Schedule (fix9 step 6) ───────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/schedule)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.scheduleJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    s.Put(R"(/api/channels/(\d+)/schedule)",
          [&mgr, channelActorOf, emitChannelAudit]
          (const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_array()) { writeError(res, R::BadJson); return; }
        const auto entries = body.size();
        const auto r = mgr.replaceSchedule(id, body);
        if (r != R::Ok) { writeError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("schedule.replaced", uid, uname, req.remote_addr,
                         {{"channel_id", id}, {"entries", entries}});
        writeJson(res, 200, mgr.scheduleJson(id));
    });

    s.Get(R"(/api/channels/(\d+)/schedule/active)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.scheduleActiveJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // GET /api/channels/{id}/schedule/upcoming?within_sec=N
    //   default within_sec = 3600 (one hour). Clamped server-side to
    //   [0, 30 days]. Always returns a JSON array (possibly empty).
    s.Get(R"(/api/channels/(\d+)/schedule/upcoming)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        int64_t within = 3600;
        if (req.has_param("within_sec")) {
            try { within = std::stoll(req.get_param_value("within_sec")); }
            catch (...) {
                writeJson(res, 400, json{{"error", "invalid_within_sec"}});
                return;
            }
        }
        const auto j = mgr.scheduleUpcomingJson(id, within);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // ── Playback log (fix8) ─────────────────────────────────────────────────
    s.Get(R"(/api/channels/(\d+)/playback-log/status)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto j = mgr.playbackLogStatusJson(id);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // GET /api/channels/{id}/playback-log
    //   ?from_ns=...&to_ns=...&after_ns=...&limit=...&offset=...
    // limit clamped to [1, 1000] inside the sink; default 100.
    s.Get(R"(/api/channels/(\d+)/playback-log)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;

        auto parse_int64 = [&](const std::string& key) -> std::optional<int64_t> {
            if (!req.has_param(key.c_str())) return std::nullopt;
            try { return std::stoll(req.get_param_value(key.c_str())); }
            catch (...) { return std::nullopt; }
        };
        const auto from_ns  = parse_int64("from_ns");
        const auto to_ns    = parse_int64("to_ns");
        const auto after_ns = parse_int64("after_ns");

        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        int offset = 0;
        if (req.has_param("offset")) {
            try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
        }

        const auto j = mgr.queryPlaybackLog(id, from_ns, to_ns, after_ns, limit, offset);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // DELETE /api/channels/{id}/playback-log
    //   ?from_ns=...&to_ns=...
    // Удаляет события воспроизведения канала в диапазоне started_at_ns ∈
    // [from_ns, to_ns] включительно. Без параметров — вычистить весь лог.
    // 200 → {"deleted_rows":N, "removed_files":M}.
    s.Delete(R"(/api/channels/(\d+)/playback-log)",
          [&mgr](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;

        auto parse_int64 = [&](const std::string& key) -> std::optional<int64_t> {
            if (!req.has_param(key.c_str())) return std::nullopt;
            try { return std::stoll(req.get_param_value(key.c_str())); }
            catch (...) { return std::nullopt; }
        };
        const auto from_ns = parse_int64("from_ns");
        const auto to_ns   = parse_int64("to_ns");

        const auto j = mgr.purgePlaybackLog(id, from_ns, to_ns);
        if (j.is_null()) { writeError(res, R::NotFound); return; }
        writeJson(res, 200, j);
    });

    // ── channel.log raw tail (admin-only) ───────────────────────────────────
    // The channel log file lives at {channel_dir}/logs/channel.log and is
    // written via spdlog rotating sink from ChannelInstance. The two
    // endpoints below let the UI show operators the raw file contents
    // (last N lines + live tail), which contains diagnostic output that
    // isn't exposed via the structured event stream — muxer errors,
    // ffmpeg warnings, gpu decode fallbacks, etc. Path is derived
    // server-side from the live ChannelInstance via
    // ChannelManager::channelDir(id) so a client never influences the
    // filesystem lookup (no path-traversal surface).
    auto channelLogPathOf = [&mgr](int id) -> std::filesystem::path {
        auto dir = mgr.channelDir(id);
        if (dir.empty()) return {};
        return dir / "logs" / "channel.log";
    };

    // GET /api/channels/{id}/logs?tail=N
    //   Returns the last N lines of channel.log. N is clamped to
    //   [1, 5000], default 200. 200 → {lines: [...], truncated: bool,
    //   size_bytes: N}. 404 if channel is not live or log file missing.
    s.Get(R"(/api/channels/(\d+)/logs)",
          [channelLogPathOf](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto path = channelLogPathOf(id);
        if (path.empty()) { writeError(res, R::NotFound); return; }

        int tail = 200;
        if (req.has_param("tail")) {
            try { tail = std::stoi(req.get_param_value("tail")); } catch (...) {}
        }
        if (tail < 1)    tail = 1;
        if (tail > 5000) tail = 5000;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            writeJson(res, 200, {{"lines", json::array()},
                                 {"truncated", false},
                                 {"size_bytes", 0}});
            return;
        }
        const auto sz = std::filesystem::file_size(path, ec);
        if (ec) { writeError(res, R::NotFound); return; }

        std::ifstream in(path, std::ios::binary);
        if (!in) { writeError(res, R::NotFound); return; }

        // Read last ~256 KiB — enough for 5000 typical log lines. This
        // avoids slurping arbitrarily large rotated files into memory.
        constexpr std::uintmax_t kMaxRead = 256 * 1024;
        std::uintmax_t start = sz > kMaxRead ? sz - kMaxRead : 0;
        in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        std::string buf((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (start > 0) {
            // Drop the first partial line so we don't return a truncated
            // record. Safe on empty buf (find returns npos, +1 → 0 → substr(0)).
            auto nl = buf.find('\n');
            if (nl != std::string::npos) buf.erase(0, nl + 1);
        }

        std::vector<std::string> all_lines;
        all_lines.reserve(256);
        std::size_t pos = 0;
        while (pos < buf.size()) {
            auto nl = buf.find('\n', pos);
            if (nl == std::string::npos) {
                if (pos < buf.size()) all_lines.emplace_back(buf.substr(pos));
                break;
            }
            all_lines.emplace_back(buf.substr(pos, nl - pos));
            pos = nl + 1;
        }

        const bool truncated = (start > 0) ||
                               (static_cast<int>(all_lines.size()) > tail);
        json lines = json::array();
        const std::size_t begin = all_lines.size() > static_cast<std::size_t>(tail)
                                  ? all_lines.size() - tail : 0;
        for (std::size_t i = begin; i < all_lines.size(); ++i) {
            lines.push_back(std::move(all_lines[i]));
        }
        writeJson(res, 200, {{"lines",      std::move(lines)},
                             {"truncated",  truncated},
                             {"size_bytes", static_cast<std::int64_t>(sz)}});
    });

    // GET /api/channels/{id}/logs/stream
    //   Server-Sent Events tail-F of channel.log. Each new line is
    //   emitted as a `data:` frame with a JSON body {"line":"..."}. The
    //   handler polls file size every 500ms and streams any new bytes
    //   past its last read offset. On file shrink (spdlog rotation
    //   truncated us out) we reset to offset 0 and emit a `rotated`
    //   comment so the client can clear its buffer.
    s.Get(R"(/api/channels/(\d+)/logs/stream)",
          [channelLogPathOf](const httplib::Request& req, httplib::Response& res) {
        int id = 0; if (!parseId(req, res, id)) return;
        const auto path = channelLogPathOf(id);
        if (path.empty()) { writeError(res, R::NotFound); return; }

        struct StreamState {
            std::filesystem::path path;
            std::uintmax_t        offset{0};
            std::string           carry;  // partial trailing line
        };
        auto state = std::make_shared<StreamState>();
        state->path = path;

        // Seed offset at end-of-file so the stream only shows lines
        // appended after the client subscribed. Historical lines come
        // from GET /logs?tail=N.
        std::error_code ec;
        if (std::filesystem::exists(state->path, ec) && !ec) {
            const auto sz = std::filesystem::file_size(state->path, ec);
            if (!ec) state->offset = sz;
        }

        res.set_header("Cache-Control",     "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [state](std::size_t, httplib::DataSink& sink) -> bool {
                using namespace std::chrono_literals;
                std::error_code ec2;
                if (!std::filesystem::exists(state->path, ec2) || ec2) {
                    static constexpr char kKeepalive[] = ": keepalive\n\n";
                    std::this_thread::sleep_for(500ms);
                    return sink.write(kKeepalive, sizeof(kKeepalive) - 1);
                }
                const auto sz = std::filesystem::file_size(state->path, ec2);
                if (ec2) {
                    std::this_thread::sleep_for(500ms);
                    return true;
                }

                // Rotation / truncation — file shrank under us. Reset
                // offset to 0 and tell the client to drop its buffer.
                if (sz < state->offset) {
                    state->offset = 0;
                    state->carry.clear();
                    static constexpr char kRot[] = ": rotated\n\n";
                    if (!sink.write(kRot, sizeof(kRot) - 1)) return false;
                }

                if (sz == state->offset) {
                    static constexpr char kKeepalive[] = ": keepalive\n\n";
                    std::this_thread::sleep_for(500ms);
                    return sink.write(kKeepalive, sizeof(kKeepalive) - 1);
                }

                std::ifstream in(state->path, std::ios::binary);
                if (!in) {
                    std::this_thread::sleep_for(500ms);
                    return true;
                }
                in.seekg(static_cast<std::streamoff>(state->offset), std::ios::beg);
                std::string chunk((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                state->offset = sz;

                std::string body = state->carry + chunk;
                state->carry.clear();

                std::string payload;
                payload.reserve(body.size() + 64);
                std::size_t pos = 0;
                while (pos < body.size()) {
                    auto nl = body.find('\n', pos);
                    if (nl == std::string::npos) {
                        state->carry.assign(body, pos, std::string::npos);
                        break;
                    }
                    std::string_view line(body.data() + pos, nl - pos);
                    // Strip trailing '\r' (windows-style logs, unlikely
                    // here but defensive).
                    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                    nlohmann::json j = {{"line", std::string(line)}};
                    payload += "data: ";
                    payload += j.dump();
                    payload += "\n\n";
                    pos = nl + 1;
                }

                if (payload.empty()) {
                    static constexpr char kKeepalive[] = ": keepalive\n\n";
                    std::this_thread::sleep_for(500ms);
                    return sink.write(kKeepalive, sizeof(kKeepalive) - 1);
                }
                if (!sink.write(payload.data(), payload.size())) return false;
                std::this_thread::sleep_for(500ms);
                return true;
            });
    });

    // ── WebRTC preview (fix23) ──────────────────────────────────────────────
    // POST /api/channels/{id}/preview/offer — body: {sdp, type:"offer"}
    //   200 → {session_id, sdp, type:"answer"}
    //   400 BadOffer / 404 channel not found / 429 per-channel cap /
    //   501 feature compiled out / 503 backend not wired (no PreviewManager)
    auto previewStatusFor = [](liveqx::preview::PreviewManager::Result r) {
        using PR = liveqx::preview::PreviewManager::Result;
        switch (r) {
            case PR::Ok:                return 200;
            case PR::NotFound:          return 404;
            case PR::TooManyClients:    return 429;
            case PR::CapacityExhausted: return 503;
            case PR::BadOffer:          return 400;
            case PR::NotCompiled:       return 501;
            case PR::InternalError:     return 500;
        }
        return 500;
    };
    auto previewErrorName = [](liveqx::preview::PreviewManager::Result r) {
        using PR = liveqx::preview::PreviewManager::Result;
        switch (r) {
            case PR::Ok:                return "ok";
            case PR::NotFound:          return "not_found";
            case PR::TooManyClients:    return "too_many_clients";
            case PR::CapacityExhausted: return "capacity_exhausted";
            case PR::BadOffer:          return "bad_offer";
            case PR::NotCompiled:       return "preview_not_compiled";
            case PR::InternalError:     return "internal_error";
        }
        return "unknown";
    };

    s.Post(R"(/api/channels/(\d+)/preview/offer)",
           [&mgr, pv, previewStatusFor, previewErrorName]
           (const httplib::Request& req, httplib::Response& res) {
        if (!pv) {
            writeJson(res, 503, {{"error","preview_not_configured"}});
            return;
        }
        int id = 0; if (!parseId(req, res, id)) return;
        if (mgr.statusJson(id).is_null()) {
            writeError(res, R::NotFound);
            return;
        }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        json answer;
        const auto r = pv->createOffer(id, body, &answer);
        if (r != liveqx::preview::PreviewManager::Result::Ok) {
            writeJson(res, previewStatusFor(r),
                      {{"error", previewErrorName(r)}});
            return;
        }
        writeJson(res, 200, answer);
    });

    s.Delete(R"(/api/channels/(\d+)/preview/sessions/([A-Za-z0-9_\-]+))",
             [&mgr, pv, previewStatusFor, previewErrorName]
             (const httplib::Request& req, httplib::Response& res) {
        if (!pv) {
            writeJson(res, 503, {{"error","preview_not_configured"}});
            return;
        }
        int id = 0; if (!parseId(req, res, id)) return;
        if (mgr.statusJson(id).is_null()) {
            writeError(res, R::NotFound);
            return;
        }
        const auto sid = req.matches[2].str();
        const auto r = pv->closeSession(id, sid);
        if (r != liveqx::preview::PreviewManager::Result::Ok) {
            writeJson(res, previewStatusFor(r),
                      {{"error", previewErrorName(r)}});
            return;
        }
        writeJson(res, 200, {{"status","closed"},{"session_id", sid}});
    });

    s.Get(R"(/api/channels/(\d+)/preview/stats)",
          [&mgr, pv](const httplib::Request& req, httplib::Response& res) {
        if (!pv) {
            writeJson(res, 503, {{"error","preview_not_configured"}});
            return;
        }
        int id = 0; if (!parseId(req, res, id)) return;
        if (mgr.statusJson(id).is_null()) {
            writeError(res, R::NotFound);
            return;
        }
        // null when the channel has no preview pipeline yet — return an
        // empty shape so the UI doesn't have to special-case 404.
        auto j = pv->statsJson(id);
        if (j.is_null()) {
            j = {{"channel_id", id},
                 {"encoder",    nullptr},
                 {"sessions",   json::array()}};
        }
        writeJson(res, 200, j);
    });

    s.Get("/api/preview",
          [pv](const httplib::Request&, httplib::Response& res) {
        if (!pv) {
            writeJson(res, 503, {{"error","preview_not_configured"}});
            return;
        }
        writeJson(res, 200, pv->globalSnapshotJson());
    });

    // ─── Gateway endpoints (fix18 c5/10) ────────────────────────────────────
    // When `gateways` is null, every gateway endpoint short-circuits to 503.
    // We still register the routes so clients get a useful diagnostic instead
    // of an opaque 404.

    s.Get("/api/gateways",
          [gws](const httplib::Request&, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        writeJson(res, 200, gws->listJson());
    });

    s.Post("/api/gateways",
           [gws, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        int id = 0;
        const auto r = gws->create(body, &id);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("gateway.created", uid, uname, req.remote_addr,
                         {{"gateway_id", id},
                          {"name", body.value("name", std::string{})}});
        writeJson(res, 201, {{"id", id}});
    });

    s.Get(R"(/api/gateways/(\d+))",
          [gws](const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto status = gws->statusJson(id);
        if (status.is_null()) { writeGatewayError(res, GR::NotFound); return; }
        writeJson(res, 200, status);
    });

    s.Delete(R"(/api/gateways/(\d+))",
             [gws, channelActorOf, emitChannelAudit]
             (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        std::string name;
        if (auto snap = gws->statusJson(id); !snap.is_null()) {
            name = snap.value("name", std::string{});
        }
        const auto r = gws->remove(id);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("gateway.deleted", uid, uname, req.remote_addr,
                         {{"gateway_id", id}, {"name", name}});
        writeJson(res, 200, {{"status","deleted"},{"id",id}});
    });

    s.Patch(R"(/api/gateways/(\d+))",
            [gws, channelActorOf, emitChannelAudit]
            (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        const auto r = gws->patch(id, body);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        std::vector<std::string> keys;
        if (body.is_object()) {
            keys.reserve(body.size());
            for (auto it = body.begin(); it != body.end(); ++it)
                keys.push_back(it.key());
        }
        emitChannelAudit("gateway.updated", uid, uname, req.remote_addr,
                         {{"gateway_id", id}, {"fields", keys}});
        writeJson(res, 200, gws->statusJson(id));
    });

    s.Post(R"(/api/gateways/(\d+)/play)",
           [gws, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = gws->play(id);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("gateway.play", uid, uname, req.remote_addr,
                         {{"gateway_id", id}});
        writeJson(res, 200, {{"status","playing"},{"id",id}});
    });

    // fix40 A7 — FEC config + live counters for one gateway. The PATCH form
    // is a thin wrapper over the generic patch({"fec": {...}}) so the UI can
    // edit FEC without touching the rest of the cfg. GET surfaces both the
    // current FecCfg and the per-encoder live counters that the gateway
    // exposes through its statusJson()["fec"] block.
    s.Get(R"(/api/gateways/(\d+)/fec)",
          [gws](const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto status = gws->statusJson(id);
        if (status.is_null()) { writeGatewayError(res, GR::NotFound); return; }
        json fec_block = status.value("fec", json::object());
        writeJson(res, 200, fec_block);
    });

    s.Patch(R"(/api/gateways/(\d+)/fec)",
            [gws, channelActorOf, emitChannelAudit]
            (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;
        // Wrap the fec body in {"fec": ...} so we can reuse the generic
        // hot-swap path. The body itself is the FecCfg JSON shape, which
        // matches gateways->patch({"fec": body}).
        json wrapped = {{"fec", body}};
        const auto r = gws->patch(id, wrapped);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto status = gws->statusJson(id);
        json fec_block = status.is_object() ? status.value("fec", json::object())
                                            : json::object();
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("gateway.fec_updated", uid, uname, req.remote_addr,
                         {{"gateway_id", id}});
        writeJson(res, 200, fec_block);
    });

    s.Post(R"(/api/gateways/(\d+)/stop)",
           [gws, channelActorOf, emitChannelAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!gws) { writeJson(res, 503, {{"error","gateways_not_configured"}}); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        const auto r = gws->stop(id);
        if (r != GR::Ok) { writeGatewayError(res, r); return; }
        auto [uid, uname] = channelActorOf(req);
        emitChannelAudit("gateway.stop", uid, uname, req.remote_addr,
                         {{"gateway_id", id}});
        writeJson(res, 200, {{"status","stopped"},{"id",id}});
    });

    // ─── Stream probe ───────────────────────────────────────────────────────
    // One-shot UDP sniff + PSI parse. Body:
    //   { "address":"239.1.2.3", "port":1234,
    //     "interface_name":"eth0"|null, "interface_addr":"10.0.0.5"|null,
    //     "duration_ms":3000, "recv_buffer_kb":1024 }
    // The handler blocks the httplib worker for duration_ms — kept short by
    // default (probe.duration_ms max is clamped to 10 s) so a runaway request
    // can't starve other endpoints.
    s.Post("/api/streams/probe",
           [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseJsonBody(req, res, body)) return;
        namespace probe = liveqx::gateway::probe;
        probe::ProbeOptions opts;
        try {
            opts.socket.address = body.value("address", std::string{});
            opts.socket.port    = body.value("port", 0);
        } catch (const std::exception& e) {
            writeJson(res, 400, {{"error", "invalid_body"}, {"detail", e.what()}});
            return;
        }
        if (opts.socket.address.empty() || opts.socket.port <= 0 ||
            opts.socket.port > 65535) {
            writeJson(res, 400, {{"error", "invalid_address_or_port"}});
            return;
        }
        opts.socket.interface_name = body.value("interface_name", std::string{});
        opts.socket.interface_addr = body.value("interface_addr", std::string{});
        opts.socket.recv_buffer_kb = body.value("recv_buffer_kb", 1024);
        opts.duration_ms = std::clamp(body.value("duration_ms", 3000), 200, 10000);

        auto r = probe::probe(opts);
        if (!r.success) {
            writeJson(res, 502, {{"error", "probe_failed"}, {"detail", r.error}});
            return;
        }
        json out = {
            {"transport_stream_id", r.transport_stream_id},
            {"original_network_id", r.original_network_id},
            {"total_bitrate_bps",   r.total_bitrate_bps},
            {"bytes_received",      r.bytes_received},
            {"packets_received",    r.packets_received},
            {"duration_ms",         r.duration_ms},
            {"programs",            json::array()},
        };
        for (const auto& p : r.programs) {
            json streams = json::array();
            for (const auto& s : p.streams) {
                streams.push_back({
                    {"pid",         s.pid},
                    {"stream_type", s.stream_type},
                    {"codec",       s.codec},
                    {"language",    s.language},
                    {"bitrate_bps", s.bitrate_bps},
                });
            }
            out["programs"].push_back({
                {"program_number", p.program_number},
                {"pmt_pid",        p.pmt_pid},
                {"pcr_pid",        p.pcr_pid},
                {"service_name",   p.service_name},
                {"provider_name",  p.provider_name},
                {"streams",        std::move(streams)},
            });
        }
        writeJson(res, 200, out);
    });

    // ─── System endpoints (fix18 c6/10) ─────────────────────────────────────
    // NIC enumeration so REST clients can populate dropdowns when wiring a
    // gateway's input/output interface. Always available — independent of
    // gateways being configured.
    s.Get("/api/system/interfaces",
          [](const httplib::Request&, httplib::Response& res) {
        writeJson(res, 200, liveqx::utils::enumerateInterfacesJson());
    });

    // fix29 c12: per-backend GPU encoder availability. Two layers:
    //   built_in        — compile-time; reflects ENABLE_NVENC/QSV/VAAPI.
    //   codec_registered — runtime; FFmpeg has the codec compiled and
    //                       avcodec_find_encoder_by_name() succeeds.
    // built_in=true + codec_registered=false means a binary built with
    // ENABLE_X but linked against an FFmpeg that lacks the encoder —
    // a real production misconfiguration that this endpoint surfaces.
    s.Get("/api/system/gpu",
          [](const httplib::Request&, httplib::Response& res) {
        auto codecRegistered = [](const char* name) -> bool {
            return avcodec_find_encoder_by_name(name) != nullptr;
        };
        json gpu = json::object();
        gpu["nvenc"] = {
            {"built_in",         liveqx::encoding::NvencVideoEncoder::isBuiltIn()},
            {"codec_registered", codecRegistered("h264_nvenc")},
        };
        gpu["qsv"] = {
            {"built_in",         liveqx::encoding::QsvVideoEncoder::isBuiltIn()},
            {"codec_registered", codecRegistered("h264_qsv")},
        };
        gpu["vaapi"] = {
            {"built_in",         liveqx::encoding::VaapiVideoEncoder::isBuiltIn()},
            {"codec_registered", codecRegistered("h264_vaapi")},
        };
        gpu["x264"] = {
            {"built_in",         true},
            {"codec_registered", codecRegistered("libx264")},
        };
        writeJson(res, 200, gpu);
    });

    // Host-resource dashboard: one-shot snapshot + SSE stream. Both are
    // stateless as far as the server is concerned; the SSE handler holds
    // the previous snapshot in a per-connection shared_ptr to compute
    // CPU%/NIC-bps rates between successive samples. No background thread
    // exists — sampling only runs while a client is connected.
    s.Get("/api/system/host_metrics",
          [](const httplib::Request&, httplib::Response& res) {
        writeJson(res, 200,
            liveqx::metrics::HostMetricsReader::toJson(
                liveqx::metrics::HostMetricsReader::sample()));
    });

    s.Get("/api/system/host_metrics/stream",
          [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control",     "no-cache");
        res.set_header("X-Accel-Buffering", "no");  // disable nginx buffering

        struct StreamState {
            bool                                  have_prev = false;
            liveqx::metrics::HostSnapshot         prev{};
        };
        auto state = std::make_shared<StreamState>();

        res.set_chunked_content_provider(
            "text/event-stream",
            [state](std::size_t, httplib::DataSink& sink) -> bool {
                using namespace std::chrono_literals;
                auto curr = liveqx::metrics::HostMetricsReader::sample();
                nlohmann::json payload = state->have_prev
                    ? liveqx::metrics::HostMetricsReader::toJsonWithRates(state->prev, curr)
                    : liveqx::metrics::HostMetricsReader::toJson(curr);
                state->prev      = std::move(curr);
                state->have_prev = true;

                std::string frame = "data: ";
                frame += payload.dump();
                frame += "\n\n";
                if (!sink.write(frame.data(), frame.size())) return false;
                // Sample cadence. Sleeping in the response thread is fine —
                // this endpoint is admin-only, expected concurrency is 1-2.
                std::this_thread::sleep_for(1s);
                return true;
            });
    });

    // fix36: directory browse for the UI folder picker. Admin-only. The
    // path is canonicalised through std::filesystem::weakly_canonical so
    // ../../etc inputs are resolved before listing — symlinks aren't a
    // sandbox-escape vector here because admin already has filesystem
    // read access via shell, but canonicalisation makes the parent link
    // stable and prevents the picker from confusing the user with
    // unresolved relative paths.
    s.Get("/api/system/browse",
          [](const httplib::Request& req, httplib::Response& res) {
        namespace fs = std::filesystem;
        std::string raw = req.has_param("path") ? req.get_param_value("path") : std::string{"/"};
        if (raw.empty()) raw = "/";
        // include_files=true → also list regular files. Default is dirs-only
        // so the legacy folder picker keeps its current behaviour.
        const bool include_files = req.has_param("include_files")
            && (req.get_param_value("include_files") == "1"
                || req.get_param_value("include_files") == "true");
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) p = fs::path(raw);
        if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
            writeJson(res, 404, {{"error", "not_a_directory"}, {"path", p.string()}});
            return;
        }
        json entries = json::array();
        for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code ec2;
            const bool is_dir = it->is_directory(ec2) && !ec2;
            if (!is_dir && !include_files) continue;
            std::error_code ec3;
            const bool is_reg = it->is_regular_file(ec3) && !ec3;
            if (!is_dir && !is_reg) continue;  // skip sockets/devices/etc
            const auto name = it->path().filename().string();
            if (!name.empty() && name.front() == '.') continue;  // hide dot-files
            json e = {
                {"name",      name},
                {"full_path", it->path().string()},
                {"is_dir",    is_dir},
            };
            if (is_reg) {
                std::error_code ec4;
                const auto sz = fs::file_size(it->path(), ec4);
                if (!ec4) e["size_bytes"] = static_cast<std::int64_t>(sz);
            }
            entries.push_back(std::move(e));
        }
        // Dirs first, then files; alphabetical within each group.
        std::sort(entries.begin(), entries.end(),
                  [](const json& a, const json& b) {
                      const bool ad = a.value("is_dir", true);
                      const bool bd = b.value("is_dir", true);
                      if (ad != bd) return ad;
                      return a["name"] < b["name"];
                  });
        json body = {
            {"path",    p.string()},
            {"parent",  p.has_parent_path() && p != p.root_path() ? json(p.parent_path().string()) : json(nullptr)},
            {"entries", entries},
        };
        writeJson(res, 200, body);
    });

    // ─── Plugin lifecycle (fix19 c3/15) ─────────────────────────────────────
    //   GET    /api/plugins                       — list installed plugins
    //   GET    /api/plugins/{name}                — single record
    //   POST   /api/plugins/{name}/install        — multipart .so upload
    //   DELETE /api/plugins/{name}                — uninstall (pending_unload)
    //   POST   /api/plugins/{name}/eula/accept    — mark EULA accepted
    //
    // pl==nullptr ⇒ plugin layer не сконфигурирован (минимальный build,
    // тесты без manager'а) — все 5 эндпоинтов отдают 503, как preview/auth/
    // gateway при отсутствии своих сервисов. Все 5 — admin-only по RBAC.
    auto plugins_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error","plugins_not_configured"}});
    };

    auto pluginListingJson = [](const liveqx::plugins::PluginManager::Listing& l) {
        return json{
            {"name",            l.name},
            {"version",         l.version},
            {"sha256",          l.sha256},
            {"installed_at",    l.installed_at},
            {"forced",          l.forced},
            {"eula_accepted",   l.eula_accepted},
            {"pending_unload",  l.pending_unload},
            {"output_drivers",  l.output_drivers},
            {"input_drivers",   l.input_drivers},
        };
    };

    auto installStatusInfo = [](liveqx::plugins::InstallStatus s)
        -> std::pair<int, const char*> {
        using S = liveqx::plugins::InstallStatus;
        switch (s) {
            case S::Ok:               return {201, "ok"};
            case S::AlreadyInstalled: return {409, "already_installed"};
            case S::InvalidUpload:    return {400, "invalid_upload"};
            case S::NotInAllowList:   return {412, "not_in_allow_list"};
            case S::BadHash:          return {412, "bad_hash"};
            case S::BadAbi:           return {422, "bad_abi"};
            case S::InitFailed:       return {422, "init_failed"};
            case S::IoError:          return {500, "io_error"};
        }
        return {500, "unknown"};
    };

    s.Get("/api/plugins",
          [pl, plugins_unavailable, pluginListingJson]
          (const httplib::Request&, httplib::Response& res) {
        if (!pl) { plugins_unavailable(res); return; }
        json arr = json::array();
        for (const auto& l : pl->list()) arr.push_back(pluginListingJson(l));
        writeJson(res, 200, {{"items", arr}});
    });

    s.Get(R"(/api/plugins/([a-zA-Z0-9_-]+))",
          [pl, plugins_unavailable, pluginListingJson]
          (const httplib::Request& req, httplib::Response& res) {
        if (!pl) { plugins_unavailable(res); return; }
        const std::string name = req.matches[1];
        auto opt = pl->get(name);
        if (!opt) { writeJson(res, 404, {{"error","not_installed"},{"name",name}}); return; }
        writeJson(res, 200, pluginListingJson(*opt));
    });

    // Install принимает либо multipart (поле "plugin" или "file" — UI) либо
    // raw application/octet-stream в req.body (curl/тесты). Multipart выбран
    // как первичный путь, потому что HTML-форма/UI это нативно умеет.
    // Лёгкий bearer→{user_id,username} helper, локальный для plugin-секции
    // (не зависит от actorContext, который определён ниже в auth-секции).
    // RBAC уже отрезал запросы без валидного токена — здесь best-effort
    // пропись installer'а в манифест и audit log. {-1, ""} если auth
    // не сконфигурирован или bearer некорректен.
    auto pluginActorOf = [au](const httplib::Request& req)
        -> std::pair<std::int64_t, std::string> {
        if (!au || !req.has_header("Authorization")) return {-1, ""};
        const auto v = req.get_header_value("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (v.rfind(kPrefix, 0) != 0) return {-1, ""};
        auto claims = au->verifyActiveAccess(v.substr(7));
        if (!claims) return {-1, ""};
        return {claims->user_id, claims->username};
    };

    // c5: write into auth_audit. PluginManager не имеет доступа к
    // username/ip — actor известен только REST-слою, поэтому audit
    // emission делается здесь, после mutation. event= "plugin.install" /
    // "plugin.uninstall" / "plugin.eula_accept".
    auto emitPluginAudit = [au](std::string_view event,
                                std::int64_t user_id,
                                std::string_view username,
                                std::string_view ip,
                                const json& details) {
        if (!au) return;
        std::optional<std::int64_t> uid;
        if (user_id >= 0) uid = user_id;
        au->emitAudit(event, uid, username, ip, details.dump());
    };

    s.Post(R"(/api/plugins/([a-zA-Z0-9_-]+)/install)",
           [pl, plugins_unavailable, installStatusInfo,
            pluginActorOf, emitPluginAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!pl) { plugins_unavailable(res); return; }
        const std::string name = req.matches[1];

        std::vector<std::uint8_t> blob;
        if (req.is_multipart_form_data()) {
            httplib::MultipartFormData fd;
            if (req.has_file("plugin"))     fd = req.get_file_value("plugin");
            else if (req.has_file("file"))  fd = req.get_file_value("file");
            else { writeJson(res, 400, {{"error","missing_file"}}); return; }
            if (fd.content.empty()) {
                writeJson(res, 400, {{"error","empty_file"}});
                return;
            }
            blob.assign(fd.content.begin(), fd.content.end());
        } else if (!req.body.empty()) {
            blob.assign(req.body.begin(), req.body.end());
        } else {
            writeJson(res, 400, {{"error","empty_body"}});
            return;
        }

        const auto [actor_id, actor_name] = pluginActorOf(req);
        liveqx::plugins::InstallOptions opts;
        opts.force             = req.get_param_value("force") == "true";
        opts.i_understand      = req.get_param_value("i_understand") == "true";
        opts.installer_user_id = actor_id;

        std::string sha;
        const auto st = pl->install(name, blob, opts, &sha);
        const auto [code, label] = installStatusInfo(st);

        json details = {{"name", name}, {"sha256", sha},
                        {"forced", opts.force && opts.i_understand}};
        if (st == liveqx::plugins::InstallStatus::Ok) {
            details["status"] = "ok";
            emitPluginAudit("plugin.install", actor_id, actor_name,
                            req.remote_addr, details);
            writeJson(res, code,
                      {{"status","installed"},{"name",name},{"sha256",sha}});
            return;
        }
        details["status"] = label;
        emitPluginAudit("plugin.install_failed", actor_id, actor_name,
                        req.remote_addr, details);
        writeJson(res, code,
                  {{"error", label},{"name",name},{"sha256",sha}});
    });

    s.Delete(R"(/api/plugins/([a-zA-Z0-9_-]+))",
             [pl, plugins_unavailable, pluginActorOf, emitPluginAudit]
             (const httplib::Request& req, httplib::Response& res) {
        if (!pl) { plugins_unavailable(res); return; }
        const std::string name = req.matches[1];
        const auto [actor_id, actor_name] = pluginActorOf(req);
        using U = liveqx::plugins::UninstallStatus;
        switch (pl->uninstall(name)) {
            case U::Ok:
                emitPluginAudit("plugin.uninstall", actor_id, actor_name,
                                req.remote_addr,
                                {{"name", name}, {"pending_unload", true}});
                writeJson(res, 200, {{"status","pending_unload"},{"name",name}});
                return;
            case U::NotInstalled:
                writeJson(res, 404, {{"error","not_installed"},{"name",name}});
                return;
            case U::IoError:
                emitPluginAudit("plugin.uninstall_failed", actor_id, actor_name,
                                req.remote_addr, {{"name", name}});
                writeJson(res, 500, {{"error","io_error"},{"name",name}});
                return;
        }
    });

    s.Post(R"(/api/plugins/([a-zA-Z0-9_-]+)/eula/accept)",
           [pl, plugins_unavailable, pluginActorOf, emitPluginAudit]
           (const httplib::Request& req, httplib::Response& res) {
        if (!pl) { plugins_unavailable(res); return; }
        const std::string name = req.matches[1];
        const auto [actor_id, actor_name] = pluginActorOf(req);
        if (!pl->acceptEula(name)) {
            writeJson(res, 404, {{"error","not_installed"},{"name",name}});
            return;
        }
        emitPluginAudit("plugin.eula_accept", actor_id, actor_name,
                        req.remote_addr, {{"name", name}});
        writeJson(res, 200, {{"status","ok"},{"name",name}});
    });

    // ─── fix41 — CIFS/NFS mounts ────────────────────────────────────────────
    //
    // CRUD over state/mounts.db proxied through MountManager. The
    // privileged liveqx-mountd renders systemd units; we never
    // exec mount(8) from this process. mn==nullptr ⇒ helper not wired
    // (e.g. unit tests without mounts_mgr) → 503 like other optional
    // services. Audit events: mounts.add / mounts.update / mounts.delete /
    // mounts.test / mounts.sync — admin-actor + ip + spec summary.
    auto mounts_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "mounts_not_configured"}});
    };

    auto mountPublicJson = [](const liveqx::mounts::MountPublic& m) {
        return json{
            {"id",             m.id},
            {"fs_type",        m.fs_type},
            {"source",         m.source},
            {"target",         m.target},
            {"options",        m.options},
            {"ro",             m.ro},
            {"enabled",        m.enabled},
            {"has_password",   m.has_password},
            {"cifs_username",  m.cifs_username},
            {"cifs_domain",    m.cifs_domain},
            {"created_at",     m.created_at},
            {"updated_at",     m.updated_at},
            {"active_state",   m.active_state},
            {"last_status_at", m.last_status_at},
        };
    };

    // Map MountOpResult.error_code → HTTP status. "rpc" stays 502 because
    // the DB write succeeded but the helper rejected/timed out — operator
    // can retry or trigger /sync.
    auto mountStatusFor = [](const std::string& code, bool ok_flag) -> int {
        if (ok_flag)                  return 200;
        if (code == "invalid")        return 400;
        if (code == "duplicate")      return 409;
        if (code == "not_found")      return 404;
        if (code == "crypto")         return 500;
        if (code == "db")             return 500;
        if (code == "rpc")            return 502;
        return 500;
    };

    auto emitMountAudit = [au](std::string_view event,
                               const std::optional<std::int64_t>& uid,
                               std::string_view username,
                               std::string_view ip,
                               const json& details) {
        if (!au) return;
        au->emitAudit(event, uid, username, ip, details.dump());
    };

    // Local actor extractor (mirrors actorContext from the auth block,
    // duplicated so this section doesn't rely on lexical ordering inside
    // the constructor body).
    auto mountActorOf = [au](const httplib::Request& req)
        -> std::pair<std::optional<std::int64_t>, std::string> {
        if (!au || !req.has_header("Authorization"))
            return {std::nullopt, ""};
        const auto v = req.get_header_value("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (v.rfind(kPrefix, 0) != 0) return {std::nullopt, ""};
        auto claims = au->verifyActiveAccess(v.substr(7));
        if (!claims) return {std::nullopt, ""};
        return {claims->user_id, claims->username};
    };

    s.Get("/api/system/mounts",
          [mn, mounts_unavailable, mountPublicJson]
          (const httplib::Request&, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        json arr = json::array();
        for (const auto& m : mn->listAll()) arr.push_back(mountPublicJson(m));
        writeJson(res, 200, json{{"mounts", std::move(arr)}});
    });

    s.Get(R"(/api/system/mounts/(\d+))",
          [mn, mounts_unavailable, mountPublicJson]
          (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        const std::int64_t id = std::stoll(req.matches[1]);
        auto m = mn->getById(id);
        if (!m) { writeJson(res, 404, {{"error","not_found"}}); return; }
        writeJson(res, 200, mountPublicJson(*m));
    });

    s.Post("/api/system/mounts",
           [mn, mounts_unavailable, mountPublicJson, mountStatusFor,
            emitMountAudit, mountActorOf]
           (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;

        std::string perr;
        auto spec = liveqx::mounts::MountSpec::fromJson(body, perr);
        if (!spec) {
            writeJson(res, 400, {{"error","invalid_spec"},{"reason", perr}});
            return;
        }

        const auto [actor_id, actor_name] = mountActorOf(req);
        auto r = mn->addMount(*spec);
        json details = {
            {"id",      r.id},
            {"target",  spec->target},
            {"fs_type", body.value("fs_type", std::string{})},
            {"ok",      r.ok},
            {"error",   r.error},
        };
        emitMountAudit(r.ok ? "mounts.add" : "mounts.add_failed",
                       actor_id, actor_name, req.remote_addr, details);

        const int code = mountStatusFor(r.error_code, r.ok);
        if (!r.ok) {
            writeJson(res, code, {
                {"error",      r.error},
                {"error_code", r.error_code},
                {"id",         r.id},
            });
            return;
        }
        auto m = mn->getById(r.id);
        if (!m) { writeJson(res, 500, {{"error","row_lost"}}); return; }
        json out = mountPublicJson(*m);
        out["helper_status"] = r.helper_status;
        writeJson(res, 201, out);
    });

    s.Put(R"(/api/system/mounts/(\d+))",
          [mn, mounts_unavailable, mountPublicJson, mountStatusFor,
           emitMountAudit, mountActorOf]
          (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        const std::int64_t id = std::stoll(req.matches[1]);
        json body;
        if (!parseJsonBody(req, res, body)) return;

        std::string perr;
        auto spec = liveqx::mounts::MountSpec::fromJson(body, perr);
        if (!spec) {
            writeJson(res, 400, {{"error","invalid_spec"},{"reason", perr}});
            return;
        }
        spec->id = id;

        const auto [actor_id, actor_name] = mountActorOf(req);
        auto r = mn->updateMount(id, *spec);
        json details = {
            {"id",      id},
            {"target",  spec->target},
            {"ok",      r.ok},
            {"error",   r.error},
        };
        emitMountAudit(r.ok ? "mounts.update" : "mounts.update_failed",
                       actor_id, actor_name, req.remote_addr, details);

        const int code = mountStatusFor(r.error_code, r.ok);
        if (!r.ok) {
            writeJson(res, code, {
                {"error",      r.error},
                {"error_code", r.error_code},
            });
            return;
        }
        auto m = mn->getById(id);
        if (!m) { writeJson(res, 500, {{"error","row_lost"}}); return; }
        json out = mountPublicJson(*m);
        out["helper_status"] = r.helper_status;
        writeJson(res, 200, out);
    });

    s.Delete(R"(/api/system/mounts/(\d+))",
             [mn, mounts_unavailable, mountStatusFor,
              emitMountAudit, mountActorOf]
             (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        const std::int64_t id = std::stoll(req.matches[1]);
        const auto [actor_id, actor_name] = mountActorOf(req);
        auto r = mn->removeMount(id);
        emitMountAudit(r.ok ? "mounts.delete" : "mounts.delete_failed",
                       actor_id, actor_name, req.remote_addr,
                       json{{"id", id}, {"ok", r.ok}, {"error", r.error}});
        if (!r.ok) {
            writeJson(res, mountStatusFor(r.error_code, false),
                      {{"error", r.error}, {"error_code", r.error_code}});
            return;
        }
        writeJson(res, 200, {{"status","ok"},{"id", id}});
    });

    s.Post("/api/system/mounts/test",
           [mn, mounts_unavailable, emitMountAudit, mountActorOf]
           (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;

        std::string perr;
        auto spec = liveqx::mounts::MountSpec::fromJson(body, perr);
        if (!spec) {
            writeJson(res, 400, {{"error","invalid_spec"},{"reason", perr}});
            return;
        }
        const auto [actor_id, actor_name] = mountActorOf(req);
        auto r = mn->testMount(*spec);
        emitMountAudit("mounts.test", actor_id, actor_name, req.remote_addr,
                       json{{"target", spec->target},
                            {"ok", r.ok}, {"error", r.error}});
        writeJson(res, r.ok ? 200 : 502, {
            {"ok",            r.ok},
            {"helper_status", r.helper_status},
            {"error",         r.error},
        });
    });

    s.Post(R"(/api/system/mounts/(\d+)/sync)",
           [mn, mounts_unavailable, mountPublicJson, emitMountAudit,
            mountActorOf]
           (const httplib::Request& req, httplib::Response& res) {
        if (!mn) { mounts_unavailable(res); return; }
        const std::int64_t id = std::stoll(req.matches[1]);
        const auto [actor_id, actor_name] = mountActorOf(req);
        // Pull all helper status into the DB then return the freshly-merged
        // row. Cheap (single Status RPC) and lets the operator confirm a
        // hand-fixed unit on the helper side.
        mn->syncStatusFromHelper();
        auto m = mn->getById(id);
        emitMountAudit("mounts.sync", actor_id, actor_name, req.remote_addr,
                       json{{"id", id}, {"found", m.has_value()}});
        if (!m) { writeJson(res, 404, {{"error","not_found"}}); return; }
        writeJson(res, 200, mountPublicJson(*m));
    });

    // ─── Auth endpoints (fix22 c5/24) ───────────────────────────────────────
    // /api/auth/{login,logout,refresh}. RBAC-middleware (commit 6/24)
    // оставит login и refresh открытыми (нужны для самой выдачи токена),
    // logout — требует валидный access (jti берётся из claims).
    //
    // au==nullptr означает auth не сконфигурирован — все три отдают 503,
    // как gateway-эндпоинты при отсутствии GatewayManager.
    auto auth_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "auth_not_configured"}});
    };

    auto tokenPairJson = [](const liveqx::auth::JwtIssuer::TokenPair& p) {
        return json{
            {"access_token",       p.access_token},
            {"refresh_token",      p.refresh_token},
            {"token_type",         "Bearer"},
            {"access_expires_at",  p.access_expires_at},
            {"refresh_expires_at", p.refresh_expires_at},
        };
    };

    // c12/24: best-effort извлечение actor'а из Authorization: Bearer.
    // Используется для audit emission в admin-endpoint'ах. Если bearer
    // нет/невалиден — возвращается {nullopt, ""}: audit-запись будет
    // содержать только ip, без actor'а. После c24/24 RBAC-middleware
    // перестанет пускать такие запросы вообще, но до тех пор тесты ходят
    // без bearer'а, и мы не должны падать.
    auto actorContext = [au](const httplib::Request& req)
        -> std::pair<std::optional<std::int64_t>, std::string> {
        if (!au || !req.has_header("Authorization")) return {std::nullopt, ""};
        const auto v = req.get_header_value("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (v.rfind(kPrefix, 0) != 0) return {std::nullopt, ""};
        std::string bearer = v.substr(7);
        auto claims = au->verifyActiveAccess(bearer);
        if (!claims) return {std::nullopt, ""};
        return {claims->user_id, claims->username};
    };

    s.Post("/api/auth/login",
           [au, auth_unavailable, tokenPairJson]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object() ||
            !body.contains("username") || !body["username"].is_string() ||
            !body.contains("password") || !body["password"].is_string()) {
            writeJson(res, 400, {{"error", "missing_credentials"}});
            return;
        }
        const auto username = body["username"].get<std::string>();
        const auto password = body["password"].get<std::string>();
        const auto ip = req.remote_addr;
        std::string ua;
        if (req.has_header("User-Agent")) ua = req.get_header_value("User-Agent");

        auto lr = au->login(username, password, ip, ua);
        if (auto* err = std::get_if<liveqx::auth::AuthService::LoginError>(&lr.outcome)) {
            // Маскируем UserNotFound и InvalidPassword под единый "invalid"
            // ответ — снаружи не должно быть сигнала о существовании юзера.
            using LE = liveqx::auth::AuthService::LoginError;
            int status = 401;
            std::string code = "invalid_credentials";
            if (*err == LE::UserDisabled)           { status = 403; code = "user_disabled"; }
            if (*err == LE::InitialPasswordExpired) { status = 403; code = "initial_password_expired"; }
            if (*err == LE::AccountLocked)          { status = 423; code = "account_locked"; }
            if (*err == LE::InternalError)          { status = 500; code = "internal_error"; }
            writeJson(res, status, {{"error", code}});
            return;
        }
        const auto& pair = std::get<liveqx::auth::JwtIssuer::TokenPair>(lr.outcome);
        json out = tokenPairJson(pair);
        if (lr.user) {
            out["user"] = {
                {"id",                   lr.user->id},
                {"username",             lr.user->username},
                {"role",                 liveqx::auth::roleName(lr.user->role)},
                {"must_change_password", lr.user->must_change_password},
            };
        }
        writeJson(res, 200, out);
    });

    s.Post("/api/auth/refresh",
           [au, auth_unavailable, tokenPairJson]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object() ||
            !body.contains("refresh_token") ||
            !body["refresh_token"].is_string()) {
            writeJson(res, 400, {{"error", "missing_refresh_token"}});
            return;
        }
        const auto rt = body["refresh_token"].get<std::string>();
        const auto ip = req.remote_addr;
        std::string ua;
        if (req.has_header("User-Agent")) ua = req.get_header_value("User-Agent");

        auto rr = au->refresh(rt, ip, ua);
        if (auto* err = std::get_if<liveqx::auth::AuthService::RefreshError>(&rr.outcome)) {
            using RE = liveqx::auth::AuthService::RefreshError;
            int status = 401;
            std::string code = liveqx::auth::refreshErrorName(*err);
            if (*err == RE::UserDisabled)     status = 403;
            if (*err == RE::InternalError)    status = 500;
            // c20/24: LdapCacheExpired остаётся 401 — клиент должен
            // перелогиниться, а 401 это и сигнализирует. Имя кода
            // в теле даёт UI возможность показать «LDAP недоступен,
            // войдите снова».
            writeJson(res, status, {{"error", code}});
            return;
        }
        const auto& pair = std::get<liveqx::auth::JwtIssuer::TokenPair>(rr.outcome);
        writeJson(res, 200, tokenPairJson(pair));
    });

    // ─── Admin user CRUD (fix22 c7/24) ──────────────────────────────────────
    // RBAC-middleware (commit 24/24) требует admin-role на всех этих
    // эндпоинтах. На уровне ControlApi check'а нет — мы доверяем, что
    // upstream middleware уже отрезал не-admin.

    auto userJson = [](const liveqx::auth::User& u) {
        json out = {
            {"id",                    u.id},
            {"username",              u.username},
            {"email",                 u.email},
            {"role",                  liveqx::auth::roleName(u.role)},
            {"source",                liveqx::auth::sourceName(u.source)},
            {"must_change_password",  u.must_change_password},
            {"disabled",              u.disabled},
            {"created_at",            u.created_at},
        };
        if (u.last_login_at)               out["last_login_at"]               = *u.last_login_at;
        if (!u.last_login_ip.empty())      out["last_login_ip"]               = u.last_login_ip;
        if (u.password_changed_at)         out["password_changed_at"]         = *u.password_changed_at;
        if (u.initial_password_expires_at) out["initial_password_expires_at"] = *u.initial_password_expires_at;
        // c13/24 — admin-list/get отдаёт состояние lockout'а, чтобы UI
        // показывал «заблокирован до X» и предлагал unlock.
        out["failed_login_count"] = u.failed_login_count;
        if (u.locked_until)                out["locked_until"]                = *u.locked_until;
        return out;
    };

    auto writeAdminError = [](httplib::Response& res,
                              liveqx::auth::AuthService::AdminError e) {
        using AE = liveqx::auth::AuthService::AdminError;
        int status = 400;
        if (e == AE::UsernameTaken) status = 409;
        if (e == AE::UserNotFound)  status = 404;
        if (e == AE::InternalError) status = 500;
        writeJson(res, status,
            {{"error", liveqx::auth::adminErrorName(e)}});
    };

    s.Get("/api/auth/users",
          [au, auth_unavailable, userJson]
          (const httplib::Request&, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        json arr = json::array();
        for (const auto& u : au->adminListUsers()) arr.push_back(userJson(u));
        writeJson(res, 200, {{"users", std::move(arr)}});
    });

    s.Post("/api/auth/users",
           [au, auth_unavailable, userJson, writeAdminError, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object() || !body.contains("username") ||
            !body["username"].is_string()) {
            writeJson(res, 400, {{"error", "missing_username"}});
            return;
        }

        liveqx::auth::AuthService::CreateUserRequest c;
        c.username = body["username"].get<std::string>();
        if (body.contains("email")    && body["email"].is_string())
            c.email    = body["email"].get<std::string>();
        if (body.contains("password") && body["password"].is_string())
            c.password = body["password"].get<std::string>();
        if (body.contains("role") && body["role"].is_string()) {
            auto r = liveqx::auth::roleFromString(body["role"].get<std::string>());
            if (!r) { writeJson(res, 400, {{"error", "invalid_role"}}); return; }
            c.role = *r;
        }
        if (body.contains("must_change_password") && body["must_change_password"].is_boolean()) {
            c.must_change_password = body["must_change_password"].get<bool>();
        }
        auto [actor_id, actor_name] = actorContext(req);
        if (actor_id) c.created_by = *actor_id;

        auto out = au->adminCreateUser(c);
        if (auto* err = std::get_if<liveqx::auth::AuthService::AdminError>(&out)) {
            writeAdminError(res, *err);
            return;
        }
        const auto& cu = std::get<liveqx::auth::AuthService::CreatedUser>(out);
        au->emitAudit("admin.user.created", actor_id, actor_name, req.remote_addr,
            json({{"target_id", cu.user.id},
                  {"target_username", cu.user.username},
                  {"role", liveqx::auth::roleName(cu.user.role)}}).dump());
        json body_out = userJson(cu.user);
        if (!cu.plaintext_password.empty()) {
            // Auto-generated пароль — отдаём admin'у один раз.
            body_out["initial_password"] = cu.plaintext_password;
        }
        writeJson(res, 201, body_out);
    });

    s.Get(R"(/api/auth/users/(\d+))",
          [au, auth_unavailable, userJson]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto u = au->adminGetUser(id);
        if (!u) { writeJson(res, 404, {{"error", "user_not_found"}}); return; }
        writeJson(res, 200, userJson(*u));
    });

    s.Put(R"(/api/auth/users/(\d+))",
          [au, auth_unavailable, userJson, writeAdminError, actorContext]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        json body;
        if (!parseJsonBody(req, res, body)) return;

        // Текущее состояние — патчим только указанные поля. Сначала
        // подгружаем существующего, чтобы пропущенные fields сохранили
        // прежние значения.
        auto cur = au->adminGetUser(id);
        if (!cur) { writeJson(res, 404, {{"error", "user_not_found"}}); return; }

        liveqx::auth::AuthService::UpdateUserRequest u;
        u.email  = body.value("email",  cur->email);
        u.role   = cur->role;
        u.source = cur->source;
        if (body.contains("role") && body["role"].is_string()) {
            auto r = liveqx::auth::roleFromString(body["role"].get<std::string>());
            if (!r) { writeJson(res, 400, {{"error", "invalid_role"}}); return; }
            u.role = *r;
        }
        if (body.contains("source") && body["source"].is_string()) {
            auto sopt = liveqx::auth::sourceFromString(
                body["source"].get<std::string>());
            if (!sopt) { writeJson(res, 400, {{"error", "invalid_source"}}); return; }
            u.source = *sopt;
        }

        auto out = au->adminUpdateUser(id, u);
        if (auto* err = std::get_if<liveqx::auth::AuthService::AdminError>(&out)) {
            writeAdminError(res, *err);
            return;
        }
        const auto& nu = std::get<liveqx::auth::User>(out);
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.updated", actor_id, actor_name, req.remote_addr,
            json({{"target_id", nu.id},
                  {"old_role",  liveqx::auth::roleName(cur->role)},
                  {"new_role",  liveqx::auth::roleName(nu.role)},
                  {"old_email", cur->email},
                  {"new_email", nu.email}}).dump());
        writeJson(res, 200, userJson(nu));
    });

    s.Delete(R"(/api/auth/users/(\d+))",
             [au, auth_unavailable, actorContext]
             (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto target = au->adminGetUser(id);
        if (!target) {
            writeJson(res, 404, {{"error", "user_not_found"}});
            return;
        }
        if (!au->adminSetDisabled(id, true)) {
            writeJson(res, 500, {{"error", "internal_error"}});
            return;
        }
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.disabled", actor_id, actor_name, req.remote_addr,
            json({{"target_id", id},
                  {"target_username", target->username}}).dump());
        writeJson(res, 200, {{"status", "disabled"}, {"id", id}});
    });

    s.Post(R"(/api/auth/users/(\d+)/enable)",
           [au, auth_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto target = au->adminGetUser(id);
        if (!target) {
            writeJson(res, 404, {{"error", "user_not_found"}});
            return;
        }
        if (!au->adminSetDisabled(id, false)) {
            writeJson(res, 500, {{"error", "internal_error"}});
            return;
        }
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.enabled", actor_id, actor_name, req.remote_addr,
            json({{"target_id", id},
                  {"target_username", target->username}}).dump());
        writeJson(res, 200, {{"status", "enabled"}, {"id", id}});
    });

    s.Post(R"(/api/auth/users/(\d+)/reset-password)",
           [au, auth_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto target = au->adminGetUser(id);
        if (!target) { writeJson(res, 404, {{"error", "user_not_found"}}); return; }
        auto pw = au->adminResetPassword(id);
        if (!pw) { writeJson(res, 500, {{"error", "internal_error"}}); return; }
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.password_reset", actor_id, actor_name, req.remote_addr,
            json({{"target_id", id},
                  {"target_username", target->username}}).dump());
        // Plaintext отдаётся ТОЛЬКО в этом ответе — больше нигде он не
        // материализуется. Admin обязан показать пользователю один раз.
        writeJson(res, 200, {
            {"status",           "password_reset"},
            {"id",               id},
            {"initial_password", *pw},
        });
    });

    // c13/24 — admin-unlock сбрасывает brute-force lockout (failed_login_count
    // и locked_until). Идемпотентно: на чистом юзере вернёт 200 без изменений.
    s.Post(R"(/api/auth/users/(\d+)/unlock)",
           [au, auth_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto target = au->adminGetUser(id);
        if (!target) { writeJson(res, 404, {{"error", "user_not_found"}}); return; }
        if (!au->adminUnlockUser(id)) {
            writeJson(res, 500, {{"error", "internal_error"}});
            return;
        }
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.unlocked", actor_id, actor_name, req.remote_addr,
            json({{"target_id", id},
                  {"target_username", target->username},
                  {"prev_failed_count", target->failed_login_count}}).dump());
        writeJson(res, 200, {{"status", "unlocked"}, {"id", id}});
    });

    // hard-delete пользователя. В отличие от DELETE /api/auth/users/{id}
    // (soft-disable), эта ручка физически удаляет users-row + sessions +
    // channel_permissions + password_resets и NULL'ит self-FK в
    // users.created_by, ldap_config.updated_by, smtp_config.updated_by.
    // auth_audit-записи остаются (там username хранится snapshot'ом).
    // 409: cannot_delete_self / last_admin — guards в AuthService.
    s.Post(R"(/api/auth/users/(\d+)/purge)",
           [au, auth_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto [actor_id, actor_name] = actorContext(req);
        const std::int64_t actor = actor_id.value_or(0);

        auto r = au->adminPurgeUser(id, actor);
        if (auto* e = std::get_if<liveqx::auth::AuthService::PurgeError>(&r)) {
            switch (*e) {
                case liveqx::auth::AuthService::PurgeError::UserNotFound:
                    writeJson(res, 404, {{"error", "user_not_found"}});
                    return;
                case liveqx::auth::AuthService::PurgeError::CannotDeleteSelf:
                    writeJson(res, 409, {{"error", "cannot_delete_self"}});
                    return;
                case liveqx::auth::AuthService::PurgeError::LastEnabledAdmin:
                    writeJson(res, 409, {{"error", "last_admin"}});
                    return;
                case liveqx::auth::AuthService::PurgeError::InternalError:
                    writeJson(res, 500, {{"error", "internal_error"}});
                    return;
            }
        }
        const auto& purged = std::get<liveqx::auth::AuthService::PurgedUser>(r);
        au->emitAudit("admin.user.purged", actor_id, actor_name, req.remote_addr,
            json({{"target_id", purged.id},
                  {"target_username", purged.username},
                  {"target_email", purged.email}}).dump());
        writeJson(res, 200, {{"status", "purged"}, {"id", purged.id}});
    });

    s.Post("/api/auth/logout",
           [au, auth_unavailable]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        // Берём access_token из Authorization: Bearer, верифицируем,
        // достаём jti, ревокаем сессию. Если токен невалидный — 401:
        // нечего ревокать.
        std::string bearer;
        if (req.has_header("Authorization")) {
            const auto v = req.get_header_value("Authorization");
            constexpr const char* kPrefix = "Bearer ";
            if (v.rfind(kPrefix, 0) == 0) bearer = v.substr(7);
        }
        if (bearer.empty()) {
            writeJson(res, 401, {{"error", "missing_bearer"}});
            return;
        }
        auto claims = au->verifyActiveAccess(bearer);
        if (!claims) {
            writeJson(res, 401, {{"error", "invalid_token"}});
            return;
        }
        au->logout(claims->jti);
        writeJson(res, 200, {{"status", "logged_out"}});
    });

    // ─── Self-change password (fix22 c8/24) ─────────────────────────────
    // POST /api/auth/me/password
    // body: {"current_password": "...", "new_password": "..."}
    // headers: Authorization: Bearer <access_token>
    //
    // RBAC отдельно whitelist'ит этот endpoint для users с
    // must_change_password=true (commit 24/24): если старый пароль
    // выдан admin'ом и юзер должен сменить — он залогинится и сможет
    // дойти только сюда.
    s.Post("/api/auth/me/password",
           [au, auth_unavailable]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        std::string bearer;
        if (req.has_header("Authorization")) {
            const auto v = req.get_header_value("Authorization");
            constexpr const char* kPrefix = "Bearer ";
            if (v.rfind(kPrefix, 0) == 0) bearer = v.substr(7);
        }
        if (bearer.empty()) {
            writeJson(res, 401, {{"error", "missing_bearer"}});
            return;
        }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object() ||
            !body.contains("current_password") || !body["current_password"].is_string() ||
            !body.contains("new_password")     || !body["new_password"].is_string()) {
            writeJson(res, 400, {{"error", "missing_fields"}});
            return;
        }
        auto err = au->changeOwnPassword(
            bearer,
            body["current_password"].get<std::string>(),
            body["new_password"].get<std::string>());
        if (!err) {
            writeJson(res, 200, {{"status", "password_changed"}});
            return;
        }
        using SE = liveqx::auth::AuthService::SelfPasswordError;
        int status = 400;
        switch (*err) {
            case SE::InvalidSession:       status = 401; break;
            case SE::UserNotFound:         status = 401; break;
            case SE::UserDisabled:         status = 403; break;
            case SE::CurrentPasswordWrong: status = 403; break;
            case SE::NewPasswordWeak:      status = 400; break;
            case SE::InternalError:        status = 500; break;
        }
        writeJson(res, status,
            {{"error", liveqx::auth::selfPasswordErrorName(*err)}});
    });

    // ─── Own sessions (fix32 B2) ────────────────────────────────────────
    //
    // GET /api/auth/me
    //   Возвращает текущего юзера (по JWT). Доступен любой аутентифи-
    //   цированной роли — UI Profile-странице нужны email/last_login_*,
    //   которые viewer/operator через /api/auth/users/{id} (Admin-only)
    //   получить не могут.
    s.Get("/api/auth/me",
          [au, auth_unavailable, userJson]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        std::string bearer;
        if (req.has_header("Authorization")) {
            const auto v = req.get_header_value("Authorization");
            constexpr const char* kPrefix = "Bearer ";
            if (v.rfind(kPrefix, 0) == 0) bearer = v.substr(7);
        }
        if (bearer.empty()) {
            writeJson(res, 401, {{"error", "missing_bearer"}});
            return;
        }
        auto claims = au->verifyActiveAccess(bearer);
        if (!claims) {
            writeJson(res, 401, {{"error", "invalid_token"}});
            return;
        }
        auto u = au->adminGetUser(claims->user_id);
        if (!u) { writeJson(res, 404, {{"error", "user_not_found"}}); return; }
        writeJson(res, 200, userJson(*u));
    });

    // GET /api/auth/me/sessions
    //   Возвращает активные сессии текущего юзера. Поле `current` — true
    //   для сессии, по чьему access_token пришёл запрос (по jti claim).
    //   Бэкенд показывает ТОЛЬКО собственные сессии — даже Admin не видит
    //   чужие через этот endpoint (для админских целей есть отдельный
    //   list — пока не реализован).
    //
    // DELETE /api/auth/me/sessions/{jwt_id}
    //   Идемпотентный revoke. Юзер может ревокать только свои сессии
    //   (фильтр user_id в SQL). Если ревокается текущая — следующий
    //   запрос с тем же токеном вернёт 401 (UI делает logout).
    s.Get("/api/auth/me/sessions",
          [au, auth_unavailable]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        std::string bearer;
        if (req.has_header("Authorization")) {
            const auto v = req.get_header_value("Authorization");
            constexpr const char* kPrefix = "Bearer ";
            if (v.rfind(kPrefix, 0) == 0) bearer = v.substr(7);
        }
        if (bearer.empty()) {
            writeJson(res, 401, {{"error", "missing_bearer"}});
            return;
        }
        auto claims = au->verifyActiveAccess(bearer);
        if (!claims) {
            writeJson(res, 401, {{"error", "invalid_token"}});
            return;
        }
        const auto sessions = au->listOwnActiveSessions(claims->user_id);
        json items = json::array();
        for (const auto& s : sessions) {
            json row = {
                {"jwt_id",     s.jwt_id},
                {"created_at", s.created_at},
                {"expires_at", s.expires_at},
                {"ip",         s.ip},
                {"user_agent", s.user_agent},
                {"current",    s.jwt_id == claims->jti},
            };
            if (s.last_seen_at.has_value())
                row["last_seen_at"] = *s.last_seen_at;
            else
                row["last_seen_at"] = nullptr;
            items.push_back(std::move(row));
        }
        writeJson(res, 200, {{"items", items}});
    });

    s.Delete(R"(/api/auth/me/sessions/([A-Za-z0-9_\-]+))",
             [au, auth_unavailable]
             (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        std::string bearer;
        if (req.has_header("Authorization")) {
            const auto v = req.get_header_value("Authorization");
            constexpr const char* kPrefix = "Bearer ";
            if (v.rfind(kPrefix, 0) == 0) bearer = v.substr(7);
        }
        if (bearer.empty()) {
            writeJson(res, 401, {{"error", "missing_bearer"}});
            return;
        }
        auto claims = au->verifyActiveAccess(bearer);
        if (!claims) {
            writeJson(res, 401, {{"error", "invalid_token"}});
            return;
        }
        const std::string target_jti = req.matches[1].str();
        const bool revoked = au->revokeOwnSession(target_jti, claims->user_id);
        if (!revoked) {
            // Либо чужой jwt_id, либо уже revoked, либо не существует.
            // Все три случая — 404 («такой активной сессии у вас нет»).
            writeJson(res, 404, {{"error", "session_not_found"}});
            return;
        }
        writeJson(res, 200,
                  {{"status",  "revoked"},
                   {"jwt_id",  target_jti},
                   {"current", target_jti == claims->jti}});
    });

    // ─── Master-key info (fix32 B3) ─────────────────────────────────────
    // Admin-only metadata. RBAC pre-handler уже отсёк non-admin до 403,
    // поэтому здесь только чтение и сериализация. Мастер-ключ опционален
    // — если процесс стартанул без auth-стека, отвечаем 503.
    s.Get("/api/auth/master-key/info",
          [mk](const httplib::Request&, httplib::Response& res) {
        if (!mk) {
            writeJson(res, 503, {{"error", "auth_unavailable"}});
            return;
        }
        const auto info = mk->getInfo();
        json out = {
            {"fingerprint", info.fingerprint},
            {"algorithm",   info.algorithm},
            {"source",      info.source},
        };
        if (info.created_at.has_value())
            out["created_at"] = *info.created_at;
        else
            out["created_at"] = nullptr;
        if (info.last_rotated_at.has_value())
            out["last_rotated_at"] = *info.last_rotated_at;
        else
            out["last_rotated_at"] = nullptr;
        writeJson(res, 200, out);
    });

    // ─── Audit log REST (fix22 c12/24) ──────────────────────────────────
    //
    // GET /api/auth/audit
    //   ?from_ts=...        — unix-sec, ts >= from_ts
    //   ?to_ts=...          — unix-sec, ts <  to_ts
    //   ?user_id=...
    //   ?username=...
    //   ?event=login.ok|login.fail|...
    //   ?limit=100          — max 1000
    //   ?offset=0
    //
    // RBAC c24/24 закрепит admin-only. Без RBAC отдаётся всем — но в
    // production deployment route в любом случае идёт через middleware,
    // который сначала фильтрует по роли.
    //
    // POST /api/auth/audit/purge?older_than_days=N
    //   удаляет записи старше N*86400 секунд. Возвращает {removed: N}.
    s.Get("/api/auth/audit",
          [au, auth_unavailable]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }

        liveqx::auth::AuditFilter f;
        if (req.has_param("from_ts")) {
            try { f.from_ts = std::stoll(req.get_param_value("from_ts")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_from_ts"}}); return; }
        }
        if (req.has_param("to_ts")) {
            try { f.to_ts = std::stoll(req.get_param_value("to_ts")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_to_ts"}}); return; }
        }
        if (req.has_param("user_id")) {
            try { f.user_id = std::stoll(req.get_param_value("user_id")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_user_id"}}); return; }
        }
        if (req.has_param("username")) f.username = req.get_param_value("username");
        if (req.has_param("event"))    f.event    = req.get_param_value("event");
        if (req.has_param("limit")) {
            try { f.limit = std::stoi(req.get_param_value("limit")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_limit"}}); return; }
        }
        if (req.has_param("offset")) {
            try { f.offset = std::stoi(req.get_param_value("offset")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_offset"}}); return; }
        }

        auto events = au->listAuditEvents(f);
        json arr = json::array();
        for (const auto& e : events) {
            json one = {
                {"id",    e.id},
                {"ts",    e.ts},
                {"event", e.event},
            };
            if (e.user_id)             one["user_id"]  = *e.user_id;
            if (!e.username.empty())   one["username"] = e.username;
            if (!e.ip.empty())         one["ip"]       = e.ip;
            if (!e.details_json.empty()) {
                // details — уже строка JSON; парсим обратно, чтобы клиент
                // получал структурированный объект, а не escaped string.
                try {
                    one["details"] = json::parse(e.details_json);
                } catch (...) {
                    one["details_raw"] = e.details_json;
                }
            }
            arr.push_back(std::move(one));
        }
        writeJson(res, 200, {{"events", std::move(arr)}});
    });

    s.Post("/api/auth/audit/purge",
           [au, auth_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int days = 0;
        if (req.has_param("older_than_days")) {
            try { days = std::stoi(req.get_param_value("older_than_days")); }
            catch (...) { writeJson(res, 400, {{"error", "bad_older_than_days"}}); return; }
        }
        if (days <= 0) { writeJson(res, 400, {{"error", "missing_older_than_days"}}); return; }
        const int removed = au->purgeAuditOlderThanDays(days);
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("audit.purged", actor_id, actor_name, req.remote_addr,
            json({{"older_than_days", days}, {"removed", removed}}).dump());
        writeJson(res, 200, {{"removed", removed}, {"older_than_days", days}});
    });

    // ── fix22 commit 17/24 — LDAP config GET/PUT ───────────────────────
    //
    // Singleton-конфиг (id=1 в auth.db). bind_password всегда маскируется
    // на ответе («***» если задан, "" если нет): plaintext не уезжает
    // обратно по сети после save'а, даже admin'у — нечего инжектить в
    // browser-historу или прокси-логи. PUT пишет новый пароль ТОЛЬКО если
    // body содержит "bind_password" с не-пустой строкой; иначе сохраняется
    // прежний (omit ≠ delete). Чтобы явно удалить пароль/перейти на
    // anonymous bind — body { "bind_password_unset": true }.
    auto ldap_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "ldap_repo_unavailable"}});
    };
    auto ldapConfigToJson = [](const liveqx::auth::LdapConfig& c) {
        using namespace liveqx::auth;
        json out;
        out["enabled"]                   = c.enabled;
        out["server"]                    = c.server;
        out["tls_mode"]                  = LdapConfigRepo::tlsModeToString(c.tls_mode);
        out["base_dn"]                   = c.base_dn;
        out["bind_dn"]                   = c.bind_dn;
        // bind_password всегда маскируется. Поле bind_password_set
        // даёт UI понять, нужно ли показывать "Change password" чек.
        out["bind_password_set"]         = !c.bind_password.empty();
        out["bind_password"]             = c.bind_password.empty() ? "" : "***";
        out["user_filter"]               = c.user_filter;
        out["group_attribute"]           = c.group_attribute;
        out["email_attribute"]           = c.email_attribute;
        out["network_timeout_sec"]       = c.network_timeout_sec;
        out["recheck_groups_on_refresh"] = c.recheck_groups_on_refresh;
        json grm = json::object();
        for (const auto& [k, v] : c.group_role_map) grm[k] = roleName(v);
        out["group_role_map"] = std::move(grm);
        json acls = json::array();
        for (const auto& acl : c.channel_acl) {
            json item;
            item["channel_id"] = acl.channel_id;
            json gp = json::object();
            for (const auto& [dn, p] : acl.group_perms) {
                gp[dn] = channelPermissionName(p);
            }
            item["groups"] = std::move(gp);
            acls.push_back(std::move(item));
        }
        out["channel_acl"] = std::move(acls);
        return out;
    };

    s.Get("/api/auth/ldap/config",
          [lr, ldap_unavailable, ldapConfigToJson]
          (const httplib::Request&, httplib::Response& res) {
        if (!lr) { ldap_unavailable(res); return; }
        auto cfg = lr->load();
        if (!cfg.has_value()) {
            writeJson(res, 200, {{"configured", false}});
            return;
        }
        json out = ldapConfigToJson(*cfg);
        out["configured"] = true;
        writeJson(res, 200, out);
    });

    s.Put("/api/auth/ldap/config",
          [au, lr, ldap_unavailable, ldapConfigToJson, actorContext]
          (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!lr) { ldap_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object()) {
            writeJson(res, 400, {{"error", "expected_object"}});
            return;
        }

        // Берём текущий конфиг (для preservation bind_password при omit'е).
        LdapConfig cfg;
        if (auto cur = lr->load(); cur.has_value()) cfg = std::move(*cur);

        if (body.contains("enabled") && body["enabled"].is_boolean())
            cfg.enabled = body["enabled"].get<bool>();
        if (body.contains("server") && body["server"].is_string())
            cfg.server = body["server"].get<std::string>();
        if (body.contains("tls_mode") && body["tls_mode"].is_string()) {
            auto m = LdapConfigRepo::tlsModeFromString(
                body["tls_mode"].get<std::string>());
            if (!m.has_value()) {
                writeJson(res, 400, {{"error", "invalid_tls_mode"}});
                return;
            }
            cfg.tls_mode = *m;
        }
        if (body.contains("base_dn") && body["base_dn"].is_string())
            cfg.base_dn = body["base_dn"].get<std::string>();
        if (body.contains("bind_dn") && body["bind_dn"].is_string())
            cfg.bind_dn = body["bind_dn"].get<std::string>();
        if (body.contains("user_filter") && body["user_filter"].is_string())
            cfg.user_filter = body["user_filter"].get<std::string>();
        if (body.contains("group_attribute") && body["group_attribute"].is_string())
            cfg.group_attribute = body["group_attribute"].get<std::string>();
        if (body.contains("email_attribute") && body["email_attribute"].is_string())
            cfg.email_attribute = body["email_attribute"].get<std::string>();
        if (body.contains("network_timeout_sec") &&
            body["network_timeout_sec"].is_number_integer()) {
            int t = body["network_timeout_sec"].get<int>();
            if (t < 1 || t > 60) {
                writeJson(res, 400, {{"error", "network_timeout_sec_out_of_range"}});
                return;
            }
            cfg.network_timeout_sec = t;
        }
        if (body.contains("recheck_groups_on_refresh") &&
            body["recheck_groups_on_refresh"].is_boolean()) {
            cfg.recheck_groups_on_refresh =
                body["recheck_groups_on_refresh"].get<bool>();
        }
        if (body.contains("bind_password_unset") &&
            body["bind_password_unset"].is_boolean() &&
            body["bind_password_unset"].get<bool>()) {
            cfg.bind_password.clear();
        } else if (body.contains("bind_password") &&
                   body["bind_password"].is_string()) {
            const auto pw = body["bind_password"].get<std::string>();
            if (!pw.empty()) cfg.bind_password = pw;
            // Если передали пустую строку — сохраняем прежний пароль
            // (UI «не трогать»). Чистим только через bind_password_unset.
        }
        if (body.contains("group_role_map") && body["group_role_map"].is_object()) {
            cfg.group_role_map.clear();
            for (auto it = body["group_role_map"].begin();
                 it != body["group_role_map"].end(); ++it) {
                if (!it.value().is_string()) continue;
                auto r = roleFromString(it.value().get<std::string>());
                if (!r.has_value()) {
                    writeJson(res, 400, {{"error", "invalid_role_in_map"}});
                    return;
                }
                cfg.group_role_map[it.key()] = *r;
            }
        }
        if (body.contains("channel_acl") && body["channel_acl"].is_array()) {
            cfg.channel_acl.clear();
            for (const auto& item : body["channel_acl"]) {
                if (!item.is_object()) continue;
                LdapConfig::ChannelAcl acl;
                acl.channel_id = item.value("channel_id", std::int64_t{0});
                if (item.contains("groups") && item["groups"].is_object()) {
                    for (auto it = item["groups"].begin();
                         it != item["groups"].end(); ++it) {
                        if (!it.value().is_string()) continue;
                        auto p = channelPermissionFromString(
                            it.value().get<std::string>());
                        if (!p.has_value()) {
                            writeJson(res, 400,
                                {{"error", "invalid_permission_in_acl"}});
                            return;
                        }
                        acl.group_perms[it.key()] = *p;
                    }
                }
                cfg.channel_acl.push_back(std::move(acl));
            }
        }

        // Validate если enabled — иначе разрешаем сохранять пустой
        // disabled-конфиг (admin сейчас отключает LDAP).
        if (cfg.enabled) {
            if (auto err = LdapClient::validate(cfg); !err.empty()) {
                writeJson(res, 400,
                    {{"error", "invalid_config"}, {"detail", err}});
                return;
            }
        }

        auto [actor_id, actor_name] = actorContext(req);
        if (!lr->save(cfg, actor_id)) {
            writeJson(res, 500, {{"error", "save_failed"}});
            return;
        }
        if (au) {
            au->emitAudit("admin.ldap.config_updated",
                          actor_id, actor_name, req.remote_addr,
                          json({{"enabled", cfg.enabled},
                                {"server", cfg.server},
                                {"tls_mode",
                                 LdapConfigRepo::tlsModeToString(cfg.tls_mode)}}).dump());
        }
        // Возвращаем сохранённый конфиг (с маскированным паролем).
        json out = ldapConfigToJson(cfg);
        out["configured"] = true;
        writeJson(res, 200, out);
    });

    // ── fix22 commit 21/24 — LDAP test endpoint ────────────────────────
    //
    // POST /api/auth/ldap/test
    //   Sanity-check для admin'а ДО сохранения config'а: ping()
    //   подключается, опционально authenticate() пробует bind с
    //   указанным юзером и (если ok) пересчитывает mapped_role +
    //   channel_grants по cfg.group_role_map / cfg.channel_acl.
    //
    // Body (все поля optional):
    //   "use_saved": bool, default true. Если true — берётся текущий
    //     сохранённый конфиг как база; override-поля из body
    //     поверх. Если false — base пуст, всё из body.
    //   override-поля совпадают с PUT (server, tls_mode, base_dn,
    //     bind_dn, bind_password, user_filter, group_attribute,
    //     email_attribute, network_timeout_sec, group_role_map,
    //     channel_acl). bind_password=="" игнорируется (как PUT).
    //   "username","password" — если оба заданы и не пусты, гоним
    //     LdapClient::authenticate (полный search+bind). Иначе
    //     только ping.
    //
    // Endpoint НИЧЕГО не пишет в БД (read-only probe). Audit-event
    // эмитится: оператор видит, кто и когда дёргал.
    s.Post("/api/auth/ldap/test",
           [au, lr, ldap_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!lr) { ldap_unavailable(res); return; }

        json body = json::object();
        if (!req.body.empty()) {
            if (!parseJsonBody(req, res, body)) return;
            if (!body.is_object()) {
                writeJson(res, 400, {{"error", "expected_object"}});
                return;
            }
        }

        LdapConfig cfg;
        const bool use_saved = body.value("use_saved", true);
        if (use_saved) {
            if (auto cur = lr->load(); cur.has_value()) cfg = std::move(*cur);
        }

        // Override применяем тем же правилом, что и PUT /api/auth/ldap/config.
        if (body.contains("enabled") && body["enabled"].is_boolean())
            cfg.enabled = body["enabled"].get<bool>();
        if (body.contains("server") && body["server"].is_string())
            cfg.server = body["server"].get<std::string>();
        if (body.contains("tls_mode") && body["tls_mode"].is_string()) {
            auto m = LdapConfigRepo::tlsModeFromString(
                body["tls_mode"].get<std::string>());
            if (!m.has_value()) {
                writeJson(res, 400, {{"error", "invalid_tls_mode"}});
                return;
            }
            cfg.tls_mode = *m;
        }
        if (body.contains("base_dn") && body["base_dn"].is_string())
            cfg.base_dn = body["base_dn"].get<std::string>();
        if (body.contains("bind_dn") && body["bind_dn"].is_string())
            cfg.bind_dn = body["bind_dn"].get<std::string>();
        if (body.contains("bind_password") && body["bind_password"].is_string()) {
            const auto pw = body["bind_password"].get<std::string>();
            if (!pw.empty()) cfg.bind_password = pw;
        }
        if (body.contains("user_filter") && body["user_filter"].is_string())
            cfg.user_filter = body["user_filter"].get<std::string>();
        if (body.contains("group_attribute") && body["group_attribute"].is_string())
            cfg.group_attribute = body["group_attribute"].get<std::string>();
        if (body.contains("email_attribute") && body["email_attribute"].is_string())
            cfg.email_attribute = body["email_attribute"].get<std::string>();
        if (body.contains("network_timeout_sec") &&
            body["network_timeout_sec"].is_number_integer()) {
            int t = body["network_timeout_sec"].get<int>();
            if (t < 1 || t > 60) {
                writeJson(res, 400,
                          {{"error", "network_timeout_sec_out_of_range"}});
                return;
            }
            cfg.network_timeout_sec = t;
        }
        if (body.contains("group_role_map") && body["group_role_map"].is_object()) {
            cfg.group_role_map.clear();
            for (auto it = body["group_role_map"].begin();
                 it != body["group_role_map"].end(); ++it) {
                if (!it.value().is_string()) continue;
                auto r = roleFromString(it.value().get<std::string>());
                if (!r.has_value()) {
                    writeJson(res, 400, {{"error", "invalid_role_in_map"}});
                    return;
                }
                cfg.group_role_map[it.key()] = *r;
            }
        }
        if (body.contains("channel_acl") && body["channel_acl"].is_array()) {
            cfg.channel_acl.clear();
            for (const auto& item : body["channel_acl"]) {
                if (!item.is_object()) continue;
                LdapConfig::ChannelAcl acl;
                acl.channel_id = item.value("channel_id", std::int64_t{0});
                if (item.contains("groups") && item["groups"].is_object()) {
                    for (auto it = item["groups"].begin();
                         it != item["groups"].end(); ++it) {
                        if (!it.value().is_string()) continue;
                        auto p = channelPermissionFromString(
                            it.value().get<std::string>());
                        if (!p.has_value()) {
                            writeJson(res, 400,
                                {{"error", "invalid_permission_in_acl"}});
                            return;
                        }
                        acl.group_perms[it.key()] = *p;
                    }
                }
                cfg.channel_acl.push_back(std::move(acl));
            }
        }

        // Test-endpoint форсирует enabled=true перед validate: оператор
        // обычно тестит config'и до того как сохранил «enabled», и нет
        // смысла отказывать ему по этому полю. Все остальные требования
        // validate() (server / base_dn / user_filter с %s) остаются.
        cfg.enabled = true;
        if (auto err = LdapClient::validate(cfg); !err.empty()) {
            writeJson(res, 400,
                      {{"error", "invalid_config"}, {"detail", err}});
            return;
        }

        LdapClient client(cfg);
        auto p = client.ping();

        json out;
        json ping_obj = {
            {"ok", p.ok},
            {"latency_ms", p.latency_ms},
        };
        if (!p.ok) ping_obj["error"] = p.error;
        out["ping"] = std::move(ping_obj);

        // Optional bind probe — username+password ОБА непустые.
        bool ran_bind = false;
        std::string probed_username;
        if (body.contains("username") && body["username"].is_string() &&
            body.contains("password") && body["password"].is_string()) {
            const auto u  = body["username"].get<std::string>();
            const auto pw = body["password"].get<std::string>();
            if (!u.empty() && !pw.empty()) {
                ran_bind         = true;
                probed_username  = u;
                auto r = client.authenticate(u, pw);
                json bind_obj;
                bind_obj["ok"] = r.ok;
                using AR = LdapClient::AuthResult;
                switch (r.reason) {
                    case AR::Reason::Ok:                 bind_obj["reason"] = "ok"; break;
                    case AR::Reason::ConnectionFailed:   bind_obj["reason"] = "connection_failed"; break;
                    case AR::Reason::BindServiceFailed:  bind_obj["reason"] = "service_bind_failed"; break;
                    case AR::Reason::UserNotFound:       bind_obj["reason"] = "user_not_found"; break;
                    case AR::Reason::InvalidCredentials: bind_obj["reason"] = "invalid_credentials"; break;
                    case AR::Reason::ConfigError:        bind_obj["reason"] = "config_error"; break;
                    default:                             bind_obj["reason"] = "other"; break;
                }
                if (!r.error.empty()) bind_obj["error"] = r.error;
                if (r.ok) {
                    bind_obj["email"]  = r.email;
                    bind_obj["groups"] = r.groups;
                    if (auto role = AuthService::pickRoleForGroups(
                            r.groups, cfg.group_role_map); role) {
                        bind_obj["mapped_role"] = roleName(*role);
                    } else {
                        bind_obj["mapped_role"] = nullptr;
                    }
                    // pickAclGrantsForGroups ждёт AuthService::ChannelAclEntry —
                    // конвертим из LdapConfig::ChannelAcl 1:1.
                    std::vector<AuthService::ChannelAclEntry> acls;
                    acls.reserve(cfg.channel_acl.size());
                    for (const auto& a : cfg.channel_acl) {
                        AuthService::ChannelAclEntry e;
                        e.channel_id  = a.channel_id;
                        e.group_perms = a.group_perms;
                        acls.push_back(std::move(e));
                    }
                    auto grants =
                        AuthService::pickAclGrantsForGroups(r.groups, acls);
                    json garr = json::array();
                    for (const auto& g : grants) {
                        garr.push_back({
                            {"channel_id", g.channel_id},
                            {"permission", channelPermissionName(g.permission)},
                        });
                    }
                    bind_obj["grants"] = std::move(garr);
                }
                out["bind"] = std::move(bind_obj);
            }
        }

        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            json details = {
                {"ping_ok",  p.ok},
                {"ran_bind", ran_bind},
            };
            if (ran_bind) details["username"] = probed_username;
            au->emitAudit("admin.ldap.test",
                          actor_id, actor_name, req.remote_addr,
                          details.dump());
        }
        writeJson(res, 200, out);
    });

    // ── fix22 commit 22/24 — per-channel ACL для local-юзеров ──────────
    //
    // Endpoints:
    //   GET    /api/auth/users/{id}/channels       — список grants юзера
    //   PUT    /api/auth/users/{id}/channels/{cid} {permission}
    //   DELETE /api/auth/users/{id}/channels/{cid} — снять grant
    //   GET    /api/channels/{id}/permissions      — кто что может на канале
    //
    // Permission: "view" | "operate". LDAP-юзеры этим путём не редактируются —
    // их ACL приходит из directory; PUT/DELETE для них вернёт 400
    // user_source_mismatch (это admin-ошибка конфигурации, а не runtime-баг).
    auto channelAclErrStatus =
        [](liveqx::auth::AuthService::ChannelAclError e) {
        using E = liveqx::auth::AuthService::ChannelAclError;
        switch (e) {
            case E::UserNotFound:       return 404;
            case E::UserSourceMismatch: return 400;
            case E::InternalError:      return 500;
        }
        return 500;
    };
    auto channelAclErrName =
        [](liveqx::auth::AuthService::ChannelAclError e) -> const char* {
        using E = liveqx::auth::AuthService::ChannelAclError;
        switch (e) {
            case E::UserNotFound:       return "user_not_found";
            case E::UserSourceMismatch: return "user_source_mismatch";
            case E::InternalError:      return "internal_error";
        }
        return "internal_error";
    };
    auto permissionRowJson =
        [](const liveqx::auth::AuthDb::ChannelPermissionRow& r) {
        return json{
            {"user_id",    r.user_id},
            {"username",   r.username},
            {"channel_id", r.channel_id},
            {"permission", channelPermissionName(r.permission)},
            {"granted_at", r.granted_at},
            {"granted_by", r.granted_by},
        };
    };

    s.Get(R"(/api/auth/users/(\d+)/channels)",
          [au, auth_unavailable, channelAclErrStatus, channelAclErrName,
           permissionRowJson]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int id = 0; if (!parseId(req, res, id)) return;
        auto out = au->adminListChannelPermissionsForUser(id);
        using E = liveqx::auth::AuthService::ChannelAclError;
        if (auto* e = std::get_if<E>(&out)) {
            writeJson(res, channelAclErrStatus(*e),
                      {{"error", channelAclErrName(*e)}});
            return;
        }
        const auto& rows =
            std::get<std::vector<liveqx::auth::AuthDb::ChannelPermissionRow>>(out);
        json arr = json::array();
        for (const auto& r : rows) arr.push_back(permissionRowJson(r));
        writeJson(res, 200, {{"items", arr}});
    });

    s.Put(R"(/api/auth/users/(\d+)/channels/(\d+))",
          [au, auth_unavailable, channelAclErrStatus, channelAclErrName,
           actorContext]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int user_id = 0;
        try {
            user_id = std::stoi(req.matches[1]);
        } catch (...) {
            writeJson(res, 400, {{"error", "invalid_id"}});
            return;
        }
        std::int64_t channel_id = 0;
        try {
            channel_id = std::stoll(req.matches[2]);
        } catch (...) {
            writeJson(res, 400, {{"error", "invalid_channel_id"}});
            return;
        }

        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.contains("permission") || !body["permission"].is_string()) {
            writeJson(res, 400, {{"error", "missing_permission"}});
            return;
        }
        auto perm = liveqx::auth::channelPermissionFromString(
            body["permission"].get<std::string>());
        if (!perm) {
            writeJson(res, 400, {{"error", "invalid_permission"}});
            return;
        }

        auto [actor_id, actor_name] = actorContext(req);
        // granted_by должен быть real id; до wiring'а RBAC (commit 24)
        // тесты ходят без bearer'а — допускаем 0 как «system».
        const std::int64_t granted_by = actor_id.value_or(0);

        auto out = au->adminSetChannelPermission(user_id, channel_id,
                                                 *perm, granted_by);
        using E = liveqx::auth::AuthService::ChannelAclError;
        if (auto* e = std::get_if<E>(&out)) {
            writeJson(res, channelAclErrStatus(*e),
                      {{"error", channelAclErrName(*e)}});
            return;
        }
        au->emitAudit("admin.user.channel_grant_set", actor_id, actor_name,
                      req.remote_addr,
                      json({{"target_id", user_id},
                            {"channel_id", channel_id},
                            {"permission",
                             liveqx::auth::channelPermissionName(*perm)}}).dump());
        writeJson(res, 200, {
            {"user_id",    user_id},
            {"channel_id", channel_id},
            {"permission",
             liveqx::auth::channelPermissionName(*perm)},
        });
    });

    s.Delete(R"(/api/auth/users/(\d+)/channels/(\d+))",
             [au, auth_unavailable, channelAclErrStatus, channelAclErrName,
              actorContext]
             (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        int user_id = 0;
        try { user_id = std::stoi(req.matches[1]); }
        catch (...) { writeJson(res, 400, {{"error","invalid_id"}}); return; }
        std::int64_t channel_id = 0;
        try { channel_id = std::stoll(req.matches[2]); }
        catch (...) { writeJson(res, 400, {{"error","invalid_channel_id"}}); return; }

        auto out = au->adminRemoveChannelPermission(user_id, channel_id);
        using E = liveqx::auth::AuthService::ChannelAclError;
        if (auto* e = std::get_if<E>(&out)) {
            writeJson(res, channelAclErrStatus(*e),
                      {{"error", channelAclErrName(*e)}});
            return;
        }
        const bool removed = std::get<bool>(out);
        if (!removed) {
            writeJson(res, 404, {{"error", "grant_not_found"}});
            return;
        }
        auto [actor_id, actor_name] = actorContext(req);
        au->emitAudit("admin.user.channel_grant_removed",
                      actor_id, actor_name, req.remote_addr,
                      json({{"target_id", user_id},
                            {"channel_id", channel_id}}).dump());
        writeJson(res, 200, {{"removed", true},
                              {"user_id", user_id},
                              {"channel_id", channel_id}});
    });

    s.Get(R"(/api/channels/(\d+)/permissions)",
          [au, auth_unavailable, permissionRowJson, channelAclErrName]
          (const httplib::Request& req, httplib::Response& res) {
        if (!au) { auth_unavailable(res); return; }
        std::int64_t channel_id = 0;
        try { channel_id = std::stoll(req.matches[1]); }
        catch (...) { writeJson(res, 400, {{"error","invalid_channel_id"}}); return; }
        auto out = au->adminListChannelPermissionsForChannel(channel_id);
        using E = liveqx::auth::AuthService::ChannelAclError;
        if (auto* e = std::get_if<E>(&out)) {
            writeJson(res, 500, {{"error", channelAclErrName(*e)}});
            return;
        }
        const auto& rows =
            std::get<std::vector<liveqx::auth::AuthDb::ChannelPermissionRow>>(out);
        json arr = json::array();
        for (const auto& r : rows) arr.push_back(permissionRowJson(r));
        writeJson(res, 200, {{"channel_id", channel_id},
                              {"items", arr}});
    });

    // ── fix22 commit 23/24 — SMTP config + test endpoint ───────────────
    //
    //   GET  /api/auth/smtp/config       admin
    //   PUT  /api/auth/smtp/config       admin {server, port, security,
    //                                           username, password,
    //                                           from_email, from_name,
    //                                           timeout_sec, enabled}
    //   POST /api/auth/smtp/test         admin {to, subject?, body?,
    //                                           use_saved?: true,
    //                                           override-fields...}
    //
    // password в GET не возвращается (write-only). PUT с отсутствующим
    // password сохраняет существующий — иначе UI «обнуляет» пароль при
    // частичном редактировании. PUT с password: "" обнуляет (явный delete).
    auto smtp_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "smtp_repo_unavailable"}});
    };
    auto smtpConfigToJson = [](const liveqx::auth::SmtpConfig& c) {
        json out;
        out["enabled"]     = c.enabled;
        out["server"]      = c.server;
        out["port"]        = c.port;
        out["security"]    =
            liveqx::auth::SmtpConfigRepo::securityToString(c.security);
        out["username"]    = c.username;
        // password — write-only; флаг наличия для UI.
        out["password_set"] = !c.password.empty();
        out["from_email"]  = c.from_email;
        out["from_name"]   = c.from_name;
        out["timeout_sec"] = c.timeout_sec;
        return out;
    };

    s.Get("/api/auth/smtp/config",
          [sr, smtp_unavailable, smtpConfigToJson]
          (const httplib::Request&, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!sr) { smtp_unavailable(res); return; }
        auto cur = sr->load();
        if (!cur.has_value()) {
            // Empty defaults — no row yet.
            SmtpConfig empty;
            writeJson(res, 200, smtpConfigToJson(empty));
            return;
        }
        writeJson(res, 200, smtpConfigToJson(*cur));
    });

    s.Put("/api/auth/smtp/config",
          [au, sr, smtp_unavailable, smtpConfigToJson, actorContext]
          (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!sr) { smtp_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object()) {
            writeJson(res, 400, {{"error", "expected_object"}});
            return;
        }

        // Берём текущий конфиг как базу — partial update без потери
        // password при отсутствии поля. Аналог PUT /api/auth/ldap/config.
        SmtpConfig cfg = sr->load().value_or(SmtpConfig{});

        if (body.contains("enabled") && body["enabled"].is_boolean())
            cfg.enabled = body["enabled"].get<bool>();
        if (body.contains("server") && body["server"].is_string())
            cfg.server = body["server"].get<std::string>();
        if (body.contains("port") && body["port"].is_number_integer()) {
            int p = body["port"].get<int>();
            if (p < 1 || p > 65535) {
                writeJson(res, 400, {{"error", "port_out_of_range"}});
                return;
            }
            cfg.port = static_cast<std::uint16_t>(p);
        }
        if (body.contains("security") && body["security"].is_string()) {
            auto m = SmtpConfigRepo::securityFromString(
                body["security"].get<std::string>());
            if (!m.has_value()) {
                writeJson(res, 400, {{"error", "invalid_security"}});
                return;
            }
            cfg.security = *m;
        }
        if (body.contains("username") && body["username"].is_string())
            cfg.username = body["username"].get<std::string>();
        if (body.contains("password") && body["password"].is_string()) {
            // "" → явное удаление пароля; иначе set новое значение.
            cfg.password = body["password"].get<std::string>();
        }
        if (body.contains("from_email") && body["from_email"].is_string())
            cfg.from_email = body["from_email"].get<std::string>();
        if (body.contains("from_name") && body["from_name"].is_string())
            cfg.from_name = body["from_name"].get<std::string>();
        if (body.contains("timeout_sec") &&
            body["timeout_sec"].is_number_integer()) {
            int t = body["timeout_sec"].get<int>();
            if (t < 1 || t > 300) {
                writeJson(res, 400, {{"error", "timeout_sec_out_of_range"}});
                return;
            }
            cfg.timeout_sec = t;
        }

        // Полный validate имеет смысл только если enabled — отключённый
        // конфиг можно держать частичным до пуска.
        if (cfg.enabled) {
            if (auto err = SmtpClient::validate(cfg); !err.empty()) {
                writeJson(res, 400,
                    {{"error", "invalid_config"}, {"detail", err}});
                return;
            }
        }

        std::optional<std::int64_t> updated_by;
        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            updated_by = actor_id;
        }
        if (!sr->save(cfg, updated_by)) {
            writeJson(res, 500, {{"error", "save_failed"}});
            return;
        }
        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            au->emitAudit("admin.smtp.config_updated", actor_id, actor_name,
                          req.remote_addr,
                          json({{"server",     cfg.server},
                                {"port",       cfg.port},
                                {"security",
                                  SmtpConfigRepo::securityToString(cfg.security)},
                                {"enabled",    cfg.enabled}}).dump());
        }
        // Re-load чтобы вернуть persisted state (round-trip через БД).
        auto fresh = sr->load().value_or(cfg);
        writeJson(res, 200, smtpConfigToJson(fresh));
    });

    // POST /api/auth/smtp/test — отправляет письмо.
    // Body required: { "to": "<addr>", "subject"?, "body"?, "use_saved"?,
    //                  override-fields... }.
    // Если use_saved=true (default) — берётся текущий smtp_config + override.
    // Никаких записей в БД (read-only probe). Audit-event обязателен.
    s.Post("/api/auth/smtp/test",
           [au, sr, smtp_unavailable, actorContext]
           (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!sr) { smtp_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object()) {
            writeJson(res, 400, {{"error", "expected_object"}});
            return;
        }
        if (!body.contains("to") || !body["to"].is_string() ||
            body["to"].get<std::string>().empty()) {
            writeJson(res, 400, {{"error", "missing_to"}});
            return;
        }
        const auto to = body["to"].get<std::string>();

        SmtpConfig cfg;
        const bool use_saved = body.value("use_saved", true);
        if (use_saved) {
            if (auto cur = sr->load(); cur.has_value()) cfg = std::move(*cur);
        }
        // Override полей.
        if (body.contains("server") && body["server"].is_string())
            cfg.server = body["server"].get<std::string>();
        if (body.contains("port") && body["port"].is_number_integer()) {
            int p = body["port"].get<int>();
            if (p < 1 || p > 65535) {
                writeJson(res, 400, {{"error", "port_out_of_range"}});
                return;
            }
            cfg.port = static_cast<std::uint16_t>(p);
        }
        if (body.contains("security") && body["security"].is_string()) {
            auto m = SmtpConfigRepo::securityFromString(
                body["security"].get<std::string>());
            if (!m.has_value()) {
                writeJson(res, 400, {{"error", "invalid_security"}});
                return;
            }
            cfg.security = *m;
        }
        if (body.contains("username") && body["username"].is_string())
            cfg.username = body["username"].get<std::string>();
        if (body.contains("password") && body["password"].is_string()) {
            const auto pw = body["password"].get<std::string>();
            if (!pw.empty()) cfg.password = pw;
        }
        if (body.contains("from_email") && body["from_email"].is_string())
            cfg.from_email = body["from_email"].get<std::string>();
        if (body.contains("from_name") && body["from_name"].is_string())
            cfg.from_name = body["from_name"].get<std::string>();
        if (body.contains("timeout_sec") &&
            body["timeout_sec"].is_number_integer()) {
            int t = body["timeout_sec"].get<int>();
            if (t < 1 || t > 300) {
                writeJson(res, 400, {{"error", "timeout_sec_out_of_range"}});
                return;
            }
            cfg.timeout_sec = t;
        }

        // Test-endpoint игнорирует enabled (admin тестит до save'а).
        cfg.enabled = true;
        if (auto err = SmtpClient::validate(cfg); !err.empty()) {
            writeJson(res, 400,
                      {{"error", "invalid_config"}, {"detail", err}});
            return;
        }
        if (!SmtpClient::isPlausibleEmail(to)) {
            writeJson(res, 400, {{"error", "invalid_to"}});
            return;
        }

        const auto subject = body.value("subject",
            std::string{"LiveQX SMTP test"});
        const auto bodytxt = body.value("body",
            std::string{"This is a test message from LiveQX "
                        "SMTP /api/auth/smtp/test endpoint."});

        SmtpClient client(cfg);
        auto r = client.send(to, subject, bodytxt);
        json out;
        out["ok"]         = r.ok;
        out["latency_ms"] = r.latency_ms;
        if (!r.ok) out["error"] = r.error;

        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            au->emitAudit("admin.smtp.test", actor_id, actor_name,
                          req.remote_addr,
                          json({{"to", to},
                                {"ok", r.ok},
                                {"server", cfg.server}}).dump());
        }
        writeJson(res, 200, out);
    });

    // ── fix33: System time / NTP ──────────────────────────────────────
    //
    //   GET  /api/system/time          read effective config + runtime snapshot
    //   PUT  /api/system/time          merge-patch config (RFC 7396 style)
    //   POST /api/system/time/test     SNTP probe against arbitrary servers
    //                                  (does NOT mutate state)
    //
    // 503 (time_unavailable) when DI deps не выставлены (tests без TimeConfigRepo).
    // RBAC: GET — Operator, PUT/POST — Admin.
    auto time_unavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "time_unavailable"}});
    };
    auto timeConfigToJson = [](const liveqx::auth::TimeConfig& c) {
        using namespace liveqx::auth;
        json out;
        out["source"]          = timeSourceToString(c.source);
        out["server_timezone"] = c.server_timezone;
        json ntp;
        ntp["enabled"]         = c.ntp.enabled;
        ntp["servers"]         = c.ntp.servers;
        ntp["poll_interval_s"] = c.ntp.poll_interval_s;
        if (c.ntp.last_offset_ms) ntp["last_offset_ms"] = *c.ntp.last_offset_ms;
        else                      ntp["last_offset_ms"] = nullptr;
        if (c.ntp.last_sync_at)   ntp["last_sync_at"]   = *c.ntp.last_sync_at;
        else                      ntp["last_sync_at"]   = nullptr;
        out["ntp"]             = std::move(ntp);
        json manual;
        manual["offset_ms"]    = c.manual.offset_ms;
        if (c.manual.set_at)   manual["set_at"]        = *c.manual.set_at;
        else                   manual["set_at"]        = nullptr;
        out["manual"]          = std::move(manual);
        return out;
    };

    s.Get("/api/system/time",
          [tr, ts, time_unavailable, timeConfigToJson]
          (const httplib::Request&, httplib::Response& res) {
        if (!tr || !ts) { time_unavailable(res); return; }
        liveqx::auth::TimeConfig cfg;
        if (auto cur = tr->load(); cur.has_value()) cfg = std::move(*cur);

        json out;
        out["config"] = timeConfigToJson(cfg);

        const auto now_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                ts->now().time_since_epoch()).count();
        const auto sys_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
        json rt;
        rt["source"]          = ts->sourceName();
        rt["offset_ms"]       = ts->offsetMs();
        rt["effective_now"]   = now_ms;
        rt["system_now"]      = sys_ms;
        rt["server_timezone"] = cfg.server_timezone;
        out["runtime"] = std::move(rt);
        writeJson(res, 200, out);
    });

    s.Put("/api/system/time",
          [au, tr, ts, &mgr, time_unavailable, timeConfigToJson, actorContext]
          (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!tr || !ts) { time_unavailable(res); return; }
        json body;
        if (!parseJsonBody(req, res, body)) return;
        if (!body.is_object()) {
            writeJson(res, 400, {{"error", "expected_object"}});
            return;
        }
        // Берём текущий как базу — RFC 7396 merge-patch семантика.
        TimeConfig cfg;
        if (auto cur = tr->load(); cur.has_value()) cfg = std::move(*cur);

        if (body.contains("source") && body["source"].is_string()) {
            auto s = timeSourceFromString(body["source"].get<std::string>());
            if (!s) {
                writeJson(res, 400, {{"error", "invalid_source"}});
                return;
            }
            cfg.source = *s;
        }
        if (body.contains("server_timezone") &&
            body["server_timezone"].is_string()) {
            cfg.server_timezone = body["server_timezone"].get<std::string>();
        }
        if (body.contains("ntp") && body["ntp"].is_object()) {
            const auto& n = body["ntp"];
            if (n.contains("enabled") && n["enabled"].is_boolean())
                cfg.ntp.enabled = n["enabled"].get<bool>();
            if (n.contains("servers") && n["servers"].is_array()) {
                cfg.ntp.servers.clear();
                for (const auto& item : n["servers"]) {
                    if (item.is_string()) cfg.ntp.servers.push_back(
                        item.get<std::string>());
                }
            }
            if (n.contains("poll_interval_s") &&
                n["poll_interval_s"].is_number_integer())
                cfg.ntp.poll_interval_s = n["poll_interval_s"].get<int>();
        }
        if (body.contains("manual") && body["manual"].is_object()) {
            const auto& m = body["manual"];
            // Допускаем два варианта: явный offset_ms или epoch_unix_ms
            // (UI задаёт "вручную выставленное время"). Конвертим epoch
            // в offset как delta от текущего system_clock'а в момент save'а.
            if (m.contains("offset_ms") && m["offset_ms"].is_number_integer()) {
                cfg.manual.offset_ms = m["offset_ms"].get<std::int64_t>();
            } else if (m.contains("epoch_unix_ms") &&
                       m["epoch_unix_ms"].is_number_integer()) {
                const auto epoch_ms = m["epoch_unix_ms"].get<std::int64_t>();
                const auto sys_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                cfg.manual.offset_ms = epoch_ms - sys_ms;
            }
            cfg.manual.set_at =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
        }

        if (auto err = TimeConfigRepo::validate(cfg); !err.empty()) {
            writeJson(res, 400, {{"error", "invalid_config"}, {"detail", err}});
            return;
        }
        if (!tr->save(cfg)) {
            writeJson(res, 500, {{"error", "save_failed"}});
            return;
        }
        ts->reconfigure(cfg);

        // fix33 C — пинаем все inherit-каналы, чтобы их Scheduler'ы подобрали
        // новую серверную TZ. Каналы с explicit channel_timezone игнорируют.
        mgr.notifyServerTimezoneChanged();

        auto [actor_id, actor_name] = actorContext(req);
        if (au) {
            au->emitAudit("admin.system.time_updated",
                          actor_id, actor_name, req.remote_addr,
                          json({{"source",          timeSourceToString(cfg.source)},
                                {"server_timezone", cfg.server_timezone},
                                {"ntp_enabled",     cfg.ntp.enabled},
                                {"manual_offset_ms",cfg.manual.offset_ms}}).dump());
        }
        json out;
        out["config"] = timeConfigToJson(cfg);
        writeJson(res, 200, out);
    });

    s.Post("/api/system/time/test",
           [tr, sn, time_unavailable]
           (const httplib::Request& req, httplib::Response& res) {
        using namespace liveqx::auth;
        if (!sn) { time_unavailable(res); return; }
        json body = json::object();
        if (!req.body.empty()) {
            if (!parseJsonBody(req, res, body)) return;
            if (!body.is_object()) {
                writeJson(res, 400, {{"error", "expected_object"}});
                return;
            }
        }
        // Список серверов: explicit в body, иначе fall back на сохранённый конфиг.
        std::vector<std::string> targets;
        if (body.contains("servers") && body["servers"].is_array()) {
            for (const auto& it : body["servers"])
                if (it.is_string()) targets.push_back(it.get<std::string>());
        } else if (tr) {
            if (auto cur = tr->load(); cur.has_value()) targets = cur->ntp.servers;
        }
        if (targets.empty()) {
            writeJson(res, 400, {{"error", "no_servers"}});
            return;
        }

        int timeout_ms = 2000;
        if (body.contains("timeout_ms") &&
            body["timeout_ms"].is_number_integer()) {
            timeout_ms = std::clamp(body["timeout_ms"].get<int>(), 200, 10000);
        }

        json results = json::array();
        for (const auto& spec : targets) {
            std::string host = spec;
            int port = 123;
            const auto colon = spec.rfind(':');
            if (colon != std::string::npos && spec.front() != '[') {
                try {
                    port = std::stoi(spec.substr(colon + 1));
                    host = spec.substr(0, colon);
                } catch (...) { /* keep defaults */ }
            }
            json item;
            item["server"] = spec;
            auto r = sn->query(host, port, std::chrono::milliseconds(timeout_ms));
            if (r) {
                item["ok"]            = true;
                item["offset_ms"]     = r->offset_ms;
                item["round_trip_ms"] = r->round_trip_ms;
                item["server_unix_ms"]= r->server_unix_ms;
            } else {
                item["ok"]    = false;
                item["error"] = "unreachable";
            }
            results.push_back(std::move(item));
        }
        writeJson(res, 200, {{"results", results}});
    });

    // ── fix38: TLS administration ─────────────────────────────────────
    //
    // /api/tls/info        — public summary of mode + cert/CA metadata
    // /api/tls/ca-bundle   — download internal CA cert (PEM)
    // /api/tls/regenerate-server — re-issue server.crt with current SANs
    // /api/tls/import      — upload operator-supplied cert+key (+ optional CA)
    //
    // Endpoints return 503 (tls_unavailable) when tls_dir is unset —
    // either main.cpp ran in disabled/behind_proxy mode, or the operator
    // launched us without the auto-bootstrap routine. Audit events are
    // written through AuthService::emitAudit when available; absence of
    // auth is non-fatal for these admin-only endpoints (RBAC already
    // gates them — the audit miss only affects forensic logs).
    auto* impl_self = impl_.get();
    auto certInfoToJson = [](const liveqx::tls::CertInfo& ci) -> json {
        json out = json::object();
        out["subject"]               = ci.subject;
        out["issuer"]                = ci.issuer;
        out["serial_hex"]            = ci.serial_hex;
        out["fingerprint_sha256"]    = ci.fingerprint_sha256;
        out["signature_algorithm"]   = ci.signature_algorithm;
        out["public_key_algorithm"]  = ci.public_key_algorithm;
        out["san_dns"]               = ci.san_dns;
        out["san_ip"]                = ci.san_ip;
        out["not_before_unix"]       = ci.not_before_unix;
        out["not_after_unix"]        = ci.not_after_unix;
        out["days_remaining"]        = ci.days_remaining;
        out["is_ca"]                 = ci.is_ca;
        out["self_signed"]           = ci.self_signed;
        return out;
    };
    auto tlsUnavailable = [](httplib::Response& res) {
        writeJson(res, 503, {{"error", "tls_unavailable"},
                             {"detail",
                              "tls_dir is not configured on this instance"}});
    };

    s.Get("/api/tls/info",
          [impl_self, certInfoToJson, au, actorContext]
          (const httplib::Request&, httplib::Response& res) {
        (void)au; (void)actorContext;
        const auto& tls = impl_self->tls_bindings;
        json body;
        body["mode"]        = tls.mode;
        body["tls_enabled"] = impl_self->tls_enabled;
        body["bind"]        = tls.bind;
        body["san_extra"]   = tls.san_extra;
        body["cert_path"]   = tls.cert_path.string();
        body["key_path"]    = tls.key_path.string();
        body["tls_dir"]     = tls.tls_dir.string();

        if (!tls.cert_path.empty()) {
            auto ci = liveqx::tls::readCertInfo(tls.cert_path);
            body["server"] = ci.subject.empty() ? json{} : certInfoToJson(ci);
        } else {
            body["server"] = json{};
        }

        if (!tls.tls_dir.empty()) {
            auto ca_path = tls.tls_dir / "ca.crt";
            std::error_code ec;
            if (std::filesystem::exists(ca_path, ec)) {
                auto ci = liveqx::tls::readCertInfo(ca_path);
                body["ca"] = ci.subject.empty() ? json{} : certInfoToJson(ci);
            } else {
                body["ca"] = json{};
            }
        } else {
            body["ca"] = json{};
        }
        writeJson(res, 200, body);
    });

    s.Get("/api/tls/ca-bundle",
          [impl_self, tlsUnavailable]
          (const httplib::Request&, httplib::Response& res) {
        const auto& tls = impl_self->tls_bindings;
        if (tls.tls_dir.empty()) { tlsUnavailable(res); return; }
        auto ca_path = tls.tls_dir / "ca.crt";
        std::error_code ec;
        if (!std::filesystem::exists(ca_path, ec)) {
            writeJson(res, 404, {{"error", "ca_not_found"},
                                 {"path", ca_path.string()}});
            return;
        }
        std::ifstream f(ca_path, std::ios::binary);
        if (!f) {
            writeJson(res, 500, {{"error", "ca_read_failed"}});
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        res.status = 200;
        res.set_header("Content-Disposition",
                       "attachment; filename=\"liveqx-ca.pem\"");
        res.set_content(ss.str(), "application/x-pem-file");
    });

    s.Post("/api/tls/regenerate-server",
           [impl_self, certInfoToJson, au, actorContext, tlsUnavailable]
           (const httplib::Request& req, httplib::Response& res) {
        const auto& tls = impl_self->tls_bindings;
        if (tls.tls_dir.empty()) { tlsUnavailable(res); return; }

        json body;
        if (!parseJsonBody(req, res, body)) return;

        // Compose SAN list: config-supplied + body-supplied + auto-detect.
        std::vector<std::string> extras = tls.san_extra;
        if (body.is_object() && body.contains("san_extra") &&
            body["san_extra"].is_array()) {
            for (const auto& v : body["san_extra"]) {
                if (v.is_string()) extras.push_back(v.get<std::string>());
            }
        }
        auto sans = liveqx::tls::autoDetectSans(extras);
        if (sans.empty()) {
            writeJson(res, 400, {{"error", "no_sans"},
                                 {"detail",
                                  "auto-detection produced empty SAN list"}});
            return;
        }

        auto srv = liveqx::tls::issueServerCert(
            tls.tls_dir, "liveqx", sans);
        if (!srv.ok()) {
            writeJson(res, 500, {{"error", "issue_failed"},
                                 {"detail", srv.error}});
            return;
        }

        json out;
        out["ok"]                  = true;
        out["restart_required"]    = true;
        out["fingerprint_sha256"]  = srv.fingerprint_sha256;
        out["not_after_unix"]      = srv.not_after_unix;
        auto info = liveqx::tls::readCertInfo(srv.cert_path);
        if (!info.subject.empty()) out["server"] = certInfoToJson(info);

        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            au->emitAudit("tls.regenerate", actor_id, actor_name,
                          req.remote_addr,
                          json({{"san_dns", sans.dns_names},
                                {"san_ip_v4", sans.ip_v4},
                                {"san_ip_v6", sans.ip_v6},
                                {"fingerprint_sha256", srv.fingerprint_sha256},
                                {"not_after_unix", srv.not_after_unix}}).dump());
        }

        writeJson(res, 200, out);

        if (impl_self->on_tls_reload) {
            try { impl_self->on_tls_reload(); }
            catch (const std::exception& e) {
                LOG_ERROR("on_tls_reload threw: {}", e.what());
            }
        }
    });

    s.Post("/api/tls/import",
           [impl_self, certInfoToJson, au, actorContext, tlsUnavailable]
           (const httplib::Request& req, httplib::Response& res) {
        const auto& tls = impl_self->tls_bindings;
        if (tls.tls_dir.empty()) { tlsUnavailable(res); return; }

        std::string cert_pem, key_pem, ca_pem;
        if (req.is_multipart_form_data()) {
            if (req.has_file("cert")) cert_pem = req.get_file_value("cert").content;
            if (req.has_file("key"))  key_pem  = req.get_file_value("key").content;
            if (req.has_file("ca"))   ca_pem   = req.get_file_value("ca").content;
        } else {
            json body;
            if (!parseJsonBody(req, res, body)) return;
            if (body.is_object()) {
                if (body.contains("cert") && body["cert"].is_string())
                    cert_pem = body["cert"].get<std::string>();
                if (body.contains("key") && body["key"].is_string())
                    key_pem = body["key"].get<std::string>();
                if (body.contains("ca") && body["ca"].is_string())
                    ca_pem = body["ca"].get<std::string>();
            }
        }
        if (cert_pem.empty() || key_pem.empty()) {
            writeJson(res, 400, {{"error", "missing_cert_or_key"}});
            return;
        }

        // Stage to <tls_dir>/import.<rand>/{cert,key,ca}.pem so verifyKeyPair
        // and verifyChain can run against real files. Promote with rename
        // only after both checks pass.
        namespace fs = std::filesystem;
        auto stamp = std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());
        fs::path stage = tls.tls_dir / ("import." + stamp);
        std::error_code ec;
        fs::create_directories(stage, ec);
        if (ec) {
            writeJson(res, 500, {{"error", "stage_dir_failed"},
                                 {"detail", ec.message()}});
            return;
        }
        auto stage_cert = stage / "server.crt";
        auto stage_key  = stage / "server.key";
        auto stage_ca   = stage / "ca.crt";

        auto cleanup_stage = [&]() {
            std::error_code rec;
            fs::remove_all(stage, rec);
        };

        if (auto err = liveqx::tls::writePem(stage_cert, cert_pem, false);
            !err.empty()) {
            cleanup_stage();
            writeJson(res, 400, {{"error", "cert_write_failed"}, {"detail", err}});
            return;
        }
        if (auto err = liveqx::tls::writePem(stage_key, key_pem, true);
            !err.empty()) {
            cleanup_stage();
            writeJson(res, 400, {{"error", "key_write_failed"}, {"detail", err}});
            return;
        }
        auto kp = liveqx::tls::verifyKeyPair(stage_cert, stage_key);
        if (!kp.ok) {
            cleanup_stage();
            writeJson(res, 400, {{"error", "keypair_invalid"},
                                 {"detail", kp.error}});
            return;
        }
        if (!ca_pem.empty()) {
            if (auto err = liveqx::tls::writePem(stage_ca, ca_pem, false);
                !err.empty()) {
                cleanup_stage();
                writeJson(res, 400, {{"error", "ca_write_failed"},
                                     {"detail", err}});
                return;
            }
            auto vc = liveqx::tls::verifyChain(stage_cert, stage_ca);
            if (!vc.ok) {
                cleanup_stage();
                writeJson(res, 400, {{"error", "chain_invalid"},
                                     {"detail", vc.error}});
                return;
            }
        }

        // Promote to final paths atomically. Atomic-write semantics are
        // already provided by writePem internally; here we read the
        // staged bytes back and rewrite at the final path so chmod 0600
        // applies even if the operator imported cert+key concatenated.
        auto promote = [&](const fs::path& src, const fs::path& dst,
                           bool secret, std::string& err) -> bool {
            std::ifstream f(src, std::ios::binary);
            if (!f) { err = "read " + src.string(); return false; }
            std::stringstream ss; ss << f.rdbuf();
            err = liveqx::tls::writePem(dst, ss.str(), secret);
            return err.empty();
        };
        std::string err;
        if (!promote(stage_cert, tls.tls_dir / "server.crt", false, err)) {
            cleanup_stage();
            writeJson(res, 500, {{"error", "promote_cert_failed"},
                                 {"detail", err}});
            return;
        }
        if (!promote(stage_key, tls.tls_dir / "server.key", true, err)) {
            cleanup_stage();
            writeJson(res, 500, {{"error", "promote_key_failed"},
                                 {"detail", err}});
            return;
        }
        if (!ca_pem.empty()) {
            if (!promote(stage_ca, tls.tls_dir / "ca.crt", false, err)) {
                cleanup_stage();
                writeJson(res, 500, {{"error", "promote_ca_failed"},
                                     {"detail", err}});
                return;
            }
        }
        cleanup_stage();

        auto info = liveqx::tls::readCertInfo(tls.tls_dir / "server.crt");
        json out;
        out["ok"]                 = true;
        out["restart_required"]   = true;
        out["ca_imported"]        = !ca_pem.empty();
        if (!info.subject.empty()) {
            out["server"]               = certInfoToJson(info);
            out["fingerprint_sha256"]   = info.fingerprint_sha256;
            out["not_after_unix"]       = info.not_after_unix;
        }

        if (au) {
            auto [actor_id, actor_name] = actorContext(req);
            au->emitAudit("tls.import", actor_id, actor_name,
                          req.remote_addr,
                          json({{"ca_imported", !ca_pem.empty()},
                                {"fingerprint_sha256", info.fingerprint_sha256},
                                {"not_after_unix", info.not_after_unix}}).dump());
        }

        writeJson(res, 200, out);

        if (impl_self->on_tls_reload) {
            try { impl_self->on_tls_reload(); }
            catch (const std::exception& e) {
                LOG_ERROR("on_tls_reload threw: {}", e.what());
            }
        }
    });

    // Audit-only event for /api/tls/ca-bundle access. Emitted lazily via
    // post_routing_handler so we don't clutter every endpoint above with
    // duplicate audit calls. Only the CA-bundle GET is captured because
    // /api/tls/info reads small metadata that is not by itself sensitive.
    if (au) {
        s.set_post_routing_handler(
            [au, actorContext](const httplib::Request& req,
                               const httplib::Response& res) {
                if (req.method != "GET" || req.path != "/api/tls/ca-bundle") return;
                if (res.status / 100 != 2) return;
                auto [actor_id, actor_name] = actorContext(req);
                au->emitAudit("tls.ca_export", actor_id, actor_name,
                              req.remote_addr,
                              json({{"bytes", res.body.size()}}).dump());
            });
    }

    // ── RBAC pre-handler (commit 24/24) ───────────────────────────────
    //
    // Если main.cpp передал rbac — регистрируем правила и ставим pre-
    // routing handler. Если nullptr — RBAC выключен (тесты, dev-режим
    // без auth). Эта ветка тоже должна оставаться в проде валидной:
    // системы наблюдаемости (probes / Prometheus) не должны требовать
    // токен. Open-правила выше это покрывают.
    if (auto* rbac = impl_->rbac) {
        registerRbacRules(*rbac);
        installRbacPreHandler(s, *rbac);
    }
}

// fix35 A3.5–A3.9 — static UI serving with SPA fallback + cache headers.
// Mount ordering rule: must be called AFTER all /api/*, /healthz, /readyz,
// /livez, /metrics handlers are registered (they are, in the constructor) and
// BEFORE start(). cpp-httplib resolves explicit handlers before mount points,
// so the API never gets shadowed by static assets.
void ControlApi::mountUi(const std::filesystem::path& ui_dir) {
    namespace fs = std::filesystem;
    auto& s = *impl_->server;

    // A3.9 — headless mode: no ui_dir configured. Make GET / return a JSON
    // hint instead of a generic 404 so curl-based health probes have an
    // unambiguous signal that this is a headless instance.
    if (ui_dir.empty()) {
        s.Get("/", [](const httplib::Request&, httplib::Response& res) {
            writeJson(res, 503, {
                {"error",   "ui_not_deployed"},
                {"message", "This is a headless instance. Use /api/* endpoints."},
            });
        });
        LOG_WARN("UI directory not deployed — operating in headless mode");
        return;
    }

    // A3.8 — sanity check ui_dir. Don't crash if misconfigured: degrade
    // to headless so the rest of the service stays up.
    std::error_code ec;
    if (!fs::is_directory(ui_dir, ec)) {
        LOG_ERROR("ui_dir is not a directory: {} — falling back to headless",
                  ui_dir.string());
        s.Get("/", [](const httplib::Request&, httplib::Response& res) {
            writeJson(res, 503, {
                {"error",   "ui_dir_invalid"},
                {"message", "Configured ui_dir does not exist or is not a directory."},
            });
        });
        return;
    }

    // A3.5 — mount static files. cpp-httplib protects against `..` traversal
    // internally; A3.8 above adds the existence guard on top.
    if (!s.set_mount_point("/", ui_dir.string())) {
        LOG_ERROR("set_mount_point failed for ui_dir={}", ui_dir.string());
        return;
    }

    // A3.7 — cache headers. Vite emits hashed filenames under /assets/
    // (e.g. /assets/index-abc123.js); those are content-addressable and
    // safe to pin for a year. index.html itself must always be revalidated
    // so deploys propagate without users hard-refreshing.
    s.set_file_request_handler([](const httplib::Request& req,
                                  httplib::Response& res) {
        const std::string& path = req.path;
        if (path.starts_with("/assets/")) {
            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        } else if (path == "/" || path == "/index.html") {
            res.set_header("Cache-Control", "no-cache");
        } else {
            res.set_header("Cache-Control", "public, max-age=300");
        }
    });

    // A3.6 — SPA fallback. React-router owns paths like /channels/42 and
    // /settings/users; the server has no handler for them, so cpp-httplib
    // returns 404. We intercept GET 404s outside the API namespaces and
    // hand back index.html so the client-side router can resolve. API
    // 404s (unknown /api/* endpoint) are kept as-is.
    auto ui_dir_copy = ui_dir;  // capture by value — handler outlives caller
    s.set_error_handler([ui_dir_copy](const httplib::Request& req,
                                      httplib::Response& res) {
        if (res.status != 404 || req.method != "GET") return;
        static constexpr std::array<std::string_view, 5> api_prefixes = {
            "/api/", "/healthz", "/readyz", "/livez", "/metrics",
        };
        const std::string_view path{req.path};
        for (auto p : api_prefixes) {
            if (path.starts_with(p)) return;  // keep the 404
        }
        const auto index = ui_dir_copy / "index.html";
        std::error_code fec;
        if (!fs::exists(index, fec)) return;
        std::ifstream ifs(index, std::ios::binary);
        if (!ifs) return;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        res.status = 200;
        res.set_content(std::move(content), "text/html; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });

    LOG_INFO("UI mounted from {}", ui_dir.string());
}

ControlApi::~ControlApi() { stop(); }

void ControlApi::start() {
    impl_->thread = std::thread([this] {
        LOG_INFO("ControlApi listening on {}://{}:{}",
                 impl_->tls_enabled ? "https" : "http",
                 impl_->bind_addr, port_);
        impl_->server->listen(impl_->bind_addr.c_str(), port_);
    });
}

void ControlApi::stop() {
    if (impl_->stopped.exchange(true)) return;
    if (impl_->server) impl_->server->stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void ControlApi::setOnTlsReload(std::function<void()> cb) {
    impl_->on_tls_reload = std::move(cb);
}
