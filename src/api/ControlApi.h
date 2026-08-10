#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ChannelManager;
class MetricsCollector;

namespace liveqx::gateway { class GatewayManager; }
namespace liveqx::auth    {
class AuthService;
class LdapConfigRepo;
class SmtpConfigRepo;
class RbacMiddleware;
class MasterKey;
class TimeConfigRepo;
class TimeSourceManager;
class ISntpClient;
}
namespace liveqx::events  { class EventBus; }
namespace liveqx::preview { class PreviewManager; }
namespace liveqx::stress  { class StressService; }
namespace liveqx::plugins { class PluginManager; }
namespace liveqx::mounts  { class MountManager; }

// fix16: liveness probe options. stuck_threshold_seconds is the max age,
// in seconds, of a channel's last render tick before /livez calls it
// stuck. Default 10s ≈ 250 missed ticks at 25fps target (clearly hung).
struct LivezOptions {
    double      stuck_threshold_seconds = 10.0;
    // When non-empty, GET /api/metrics requires "Authorization: Bearer <token>".
    // Leave empty to keep the endpoint open (legacy / trusted-network setups).
    std::string metrics_bearer_token;
    // Called when the token is changed via POST/DELETE /api/metrics/token.
    // Receives the new token (empty = token cleared). Returns true on
    // successful persistence; false → ControlApi rolls back the in-memory
    // change and responds 500 so the caller is not misled into thinking
    // the new state survived a restart.
    std::function<bool(const std::string&)> on_metrics_token_changed;
};

// fix38 — TLS bindings for the Control API listener. When cert_path and
// key_path are both non-empty, ControlApi constructs an httplib::SSLServer
// and serves HTTPS on the configured bind/port. When either is empty,
// it falls back to plain httplib::Server (HTTP). main.cpp populates this
// from TlsConfig + the auto-bootstrap routine in utils/Tls.
//
// tls_dir + mode + san_extra are surfaced through /api/tls/* admin
// endpoints (info, ca-bundle, regenerate-server, import). When tls_dir
// is empty those endpoints return 503 (no PKI to manage).
struct TlsBindings {
    std::filesystem::path    cert_path;            // empty = plain HTTP
    std::filesystem::path    key_path;             // empty = plain HTTP
    std::filesystem::path    tls_dir;              // ca.{crt,key} + server.{crt,key}
    std::string              mode{"disabled"};     // auto|provided|behind_proxy|disabled
    std::vector<std::string> san_extra;            // operator-supplied SANs
    std::string              bind{"0.0.0.0"};      // listen address (v4 or v6)
};

// HTTP front-end for ChannelManager. Endpoints:
//   GET    /api/health
//   GET    /api/channels
//   POST   /api/channels                       body: ChannelConfig
//   GET    /api/channels/{id}
//   DELETE /api/channels/{id}
//   PUT    /api/channels/{id}/config           body: patch
//   POST   /api/channels/{id}/play
//   POST   /api/channels/{id}/stop
//   POST   /api/channels/{id}/next
//   GET    /api/channels/{id}/playlist
//   POST   /api/channels/{id}/playlist                body: array of items
//   POST   /api/channels/{id}/playlist/append         body: array of items
//   DELETE /api/channels/{id}/playlist/{idx}
//   DELETE /api/channels/{id}/playlist
//   POST   /api/channels/{id}/playlist/notify-deleted body: { path }
//
// fix18 — packet-level UDP/multicast forwarder CRUD:
//   GET    /api/gateways
//   POST   /api/gateways                           body: { name, input, outputs }
//   GET    /api/gateways/{id}
//   DELETE /api/gateways/{id}
//   PATCH  /api/gateways/{id}                      body: { name?, outputs? }
//   POST   /api/gateways/{id}/play
//   POST   /api/gateways/{id}/stop
//
//   GET    /api/system/interfaces                 NIC list for UI dropdowns
class ControlApi {
public:
    // metrics may be nullptr — /api/metrics then returns 503. main.cpp
    // always passes a real collector; tests that exercise channel CRUD only
    // typically pass nullptr. gateways may be nullptr — gateway endpoints
    // then return 503 (compile-out is overkill for a few stubs).
    ControlApi(int port, ChannelManager& manager,
               MetricsCollector* metrics = nullptr,
               LivezOptions livez = {},
               liveqx::gateway::GatewayManager* gateways = nullptr,
               liveqx::auth::AuthService*       auth     = nullptr,
               liveqx::auth::LdapConfigRepo*    ldap_repo = nullptr,
               liveqx::auth::SmtpConfigRepo*    smtp_repo = nullptr,
               liveqx::auth::RbacMiddleware*    rbac      = nullptr,
               liveqx::events::EventBus*        events    = nullptr,
               liveqx::preview::PreviewManager* preview   = nullptr,
               liveqx::stress::StressService*   stress    = nullptr,
               liveqx::plugins::PluginManager*  plugins   = nullptr,
               liveqx::auth::MasterKey*         master_key = nullptr,
               liveqx::mounts::MountManager*    mounts    = nullptr,
               TlsBindings                              tls       = {},
               liveqx::auth::TimeConfigRepo*    time_repo = nullptr,
               liveqx::auth::TimeSourceManager* time_src  = nullptr,
               liveqx::auth::ISntpClient*       sntp      = nullptr);
    ~ControlApi();

    // fix35 A3.5–A3.9 — mount static UI (index.html + /assets/*) and wire
    // SPA fallback for react-router paths. Pass an empty path to operate in
    // headless mode (GET / then returns a 503 hint with `error:"ui_not_deployed"`).
    // Must be called before start(); calling after has no effect on already-
    // bound routes.
    void mountUi(const std::filesystem::path& ui_dir);

    // fix38 — registered when /api/tls/regenerate-server or /api/tls/import
    // succeed. main.cpp wires this to a debounced rebuild that swaps the
    // process's listener with a new SSLContext built from the just-written
    // cert/key. Called from the request thread; the handler is responsible
    // for not blocking on the request itself (typical impl: set an atomic
    // flag and drive the rebuild from the main loop).
    void setOnTlsReload(std::function<void()> cb);

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_;
};
