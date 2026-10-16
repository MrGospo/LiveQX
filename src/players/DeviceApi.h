#pragma once
//
// The device port (A6): a separate HTTPS server where players authenticate
// with their client certificate (mTLS). It carries heartbeats and, later,
// certificate renewal. It must never be put behind a reverse proxy: the proxy
// would terminate the TLS the identity is read from.
//
// One gate runs before every route and fails closed (A8):
//
//   no certificate / not ours ........ 401 cert_untrusted
//   expired .......................... 401 cert_expired
//   CN differs from the id in the path 403 cn_mismatch
//   player no longer exists .......... 404 player_not_found
//   revoked .......................... 401 cert_revoked
//
// The TLS handshake itself always completes (the verify callback accepts and
// the outcome is read afterwards), because a hard abort looks like a network
// fault to the player's HTTP client and it could not tell "revoked" from "no
// route". Chain and expiry are checked by OpenSSL; revocation by the
// in-process RevocationList (instant) and by the database (authoritative).
// Handlers can only be reached through the gate: they are registered on the
// same server, and the gate is its pre-routing handler.

#include "events/EventBus.h"
#include "players/PlayersDb.h"
#include "players/PresenceService.h"
#include "players/RenewService.h"
#include "players/RevocationList.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httplib { class SSLServer; }

namespace liveqx::players {

// One entry for the audit trail. Successful heartbeats are NOT reported (they
// are the normal state, kept in the status table); refusals and renewals are.
struct DeviceAuditEvent {
    std::string action;           // player.device_denied | player.renew | player.renew_denied | player.renew_failed
    bool        access_denial{false};   // true -> category access, false -> player
    std::string player_id;        // may be empty (no usable identity)
    std::string client_ip;
    std::string http_method;
    std::string http_path;
    int         http_status{0};
    std::string reason;           // machine code or short note, never a secret
    int         suppressed{0};    // identical events dropped since the last one reported
};

struct DeviceApiOptions {
    std::string           bind{"0.0.0.0"};
    int                   port{0};                 // 0 = any free port (tests)
    std::filesystem::path cert_path;               // server TLS certificate (+ chain)
    std::filesystem::path key_path;
    std::string           trust_pem;               // device root + intermediate, PEM; may be empty (add later)
    std::function<std::int64_t()> now;             // unix seconds; null = real clock
    events::EventBus*     events{nullptr};
    std::int64_t          min_heartbeat_interval_s{10};
    std::size_t           workers{16};
    // Audit sink (null = no audit). Refusals are throttled per (action,
    // identity, reason): a revoked screen retrying every minute is one row per
    // window with a counter, not a flood.
    std::function<void(const DeviceAuditEvent&)> audit;
    std::int64_t          audit_throttle_s{600};
    // Who is online and what changed. Null: the port keeps a private
    // write-through one (every heartbeat stored at once, no offline sweeper) —
    // fine for tests, not for a fleet.
    PresenceService*      presence{nullptr};
    // Certificate renewal; null = the route answers 503.
    RenewService*         renew{nullptr};
    std::int64_t          min_renew_interval_s{10};
};

class DeviceApi {
public:
    DeviceApi(PlayersDb& db, RevocationList& revocations, DeviceApiOptions opts);
    ~DeviceApi();
    DeviceApi(const DeviceApi&)            = delete;
    DeviceApi& operator=(const DeviceApi&) = delete;

    bool start();
    void stop();
    int  port() const noexcept { return port_; }

    // Adds trust anchors (PEM, any number of certificates) to the running
    // server: the device CA may appear after the server started (the root
    // ceremony needs no restart). Add-only and safe with requests in flight;
    // returns how many certificates were read. Anchors can only be removed by
    // a restart, which is acceptable because revocation is decided separately.
    std::size_t addTrustAnchors(const std::string& pem);

    // Picks up a re-issued server TLS certificate/key from disk. Implemented as
    // a quick restart of the listener on the same port with the same trust
    // anchors (changing a live SSL_CTX under concurrent handshakes is not
    // safe); in-flight requests are dropped and devices retry. False if the
    // new files are unusable — then the old listener is NOT torn down.
    bool reloadServerCertificate();

    // "METHOD /path/{id}/..." for every route behind the gate.
    std::vector<std::string> routes() const;
    // How many times a route handler has run (tests: must stay 0 for
    // unauthenticated requests).
    std::size_t handlerInvocations() const noexcept { return handler_calls_.load(); }

private:
    struct Impl;
    std::int64_t nowSeconds() const;

    PlayersDb&           db_;
    RevocationList&      revocations_;
    DeviceApiOptions     opts_;
    std::unique_ptr<httplib::SSLServer> server_;
    std::thread          thread_;
    int                  port_{0};
    std::atomic<std::size_t> handler_calls_{0};

    void emitAudit(DeviceAuditEvent ev);
    bool startListener();

    PresenceService*                             presence_{nullptr};
    std::unique_ptr<PresenceService>             own_presence_;

    std::mutex                                   audit_mu_;
    struct Throttle { std::int64_t since{0}; int suppressed{0}; };
    std::unordered_map<std::string, Throttle>    audit_throttle_;
    std::string                                  trust_all_;       // every anchor ever added

    std::mutex                                   hb_mu_;
    std::unordered_map<std::string, std::int64_t> last_heartbeat_;
    std::unordered_map<std::string, std::int64_t> last_renew_;
};

}  // namespace liveqx::players
