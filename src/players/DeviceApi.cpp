#include "players/DeviceApi.h"

#include "utils/Log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace liveqx::players {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxBodyBytes = 16 * 1024;

std::string rfc3339(std::int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void reply(httplib::Response& res, int status, const char* error) {
    res.status = status;
    res.set_content(json{{"error", error}}.dump(), "application/json");
}

// Verification outcome is read after the handshake (see the header), so the
// callback accepts everything and leaves the verdict in SSL_get_verify_result.
int acceptAndRecord(int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) { return 1; }

std::string commonName(X509* x) {
    char buf[256]{};
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, buf, sizeof buf) < 0) return {};
    return buf;
}

std::string serialHex(X509* x) {
    BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(x), nullptr);
    if (!bn) return {};
    char* hex = BN_bn2hex(bn);
    std::string out = hex ? hex : "";
    OPENSSL_free(hex);
    BN_free(bn);
    return out;
}

// "/api/players/<id>/..." -> <id>; empty if the path has no id segment.
std::string idInPath(const std::string& path) {
    constexpr std::string_view kPrefix = "/api/players/";
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) return {};
    const auto rest = path.substr(kPrefix.size());
    return rest.substr(0, rest.find('/'));
}

bool looksLikeUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Printable text only, cut to `max` bytes.
std::string cleanText(const std::string& s, std::size_t max) {
    std::string out;
    out.reserve(std::min(s.size(), max));
    for (unsigned char c : s) {
        if (out.size() >= max) break;
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
    }
    return out;
}

// The heartbeat body is untrusted: keep known fields of the right type, cut
// strings, clamp numbers, drop everything else.
json sanitiseHeartbeat(const json& in) {
    json out = json::object();
    auto str = [&](const char* k, std::size_t max) {
        if (in.contains(k) && in[k].is_string()) out[k] = cleanText(in[k].get<std::string>(), max);
    };
    auto oneOf = [&](const char* k, std::initializer_list<const char*> allowed) {
        if (!in.contains(k) || !in[k].is_string()) return;
        const auto v = in[k].get<std::string>();
        for (auto* a : allowed) if (v == a) { out[k] = v; return; }
    };
    auto integer = [&](const char* k) {
        if (in.contains(k) && in[k].is_number_integer()) {
            const auto v = in[k].get<std::int64_t>();
            if (v >= 0 && v < (1LL << 53)) out[k] = v;
        }
    };
    integer("uptime_seconds");
    integer("os_uptime_seconds");
    integer("position_ms");
    // The result of the last command: it must name a real command id and carry
    // a known status, otherwise it is dropped.
    if (in.contains("last_command_result") && in["last_command_result"].is_object()) {
        const auto& r = in["last_command_result"];
        if (r.contains("id") && r["id"].is_string() && r.contains("status") && r["status"].is_string()) {
            const auto id = r["id"].get<std::string>();
            const auto st = r["status"].get<std::string>();
            const bool known = st == "executed" || st == "expired" || st == "duplicate" ||
                               st == "unsupported" || st == "failed";
            if (looksLikeUuid(id) && known) out["last_command_result"] = {{"id", id}, {"status", st}};
        }
    }
    oneOf("watchdog_state", {"ok", "stale", "not_running"});
    oneOf("health", {"ok", "warning", "critical"});
    str("current_source", 256);
    str("media_status", 64);
    str("last_error", 512);
    str("last_renew_error", 512);
    str("last_command_id", 64);
    if (in.contains("buffer_progress") && in["buffer_progress"].is_number())
        out["buffer_progress"] = std::clamp(in["buffer_progress"].get<double>(), 0.0, 1.0);
    return out;
}

}  // namespace

DeviceApi::DeviceApi(PlayersDb& db, RevocationList& revocations, DeviceApiOptions opts)
    : db_(db), revocations_(revocations), opts_(std::move(opts)) {}

DeviceApi::~DeviceApi() { stop(); }

std::int64_t DeviceApi::nowSeconds() const {
    return opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

std::vector<std::string> DeviceApi::routes() const {
    return {"PATCH /api/players/{id}/heartbeat", "POST /api/players/{id}/renew"};
}

bool DeviceApi::start() {
    if (!startListener()) return false;
    // A port chosen by the system stays the same across reloads.
    opts_.port = port_;
    return true;
}

bool DeviceApi::startListener() {
    if (!presence_) {
        if (opts_.presence) {
            presence_ = opts_.presence;
        } else {
            PresenceOptions po;
            po.flush_interval_s = 0;
            po.events           = opts_.events;
            po.now              = opts_.now;
            own_presence_ = std::make_unique<PresenceService>(db_, std::move(po));
            presence_     = own_presence_.get();
        }
    }
    server_ = std::make_unique<httplib::SSLServer>([this](SSL_CTX& ctx) {
        SSL_CTX_set_min_proto_version(&ctx, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_chain_file(&ctx, opts_.cert_path.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(&ctx, opts_.key_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(&ctx) != 1) {
            LOG_ERROR("device api: cannot load the server certificate/key");
            return false;
        }
        // Trust anchors: the device root and the intermediate. PARTIAL_CHAIN
        // lets a leaf that chains to the intermediate verify even when the
        // client did not send the whole chain. The store may start empty
        // (no ceremony yet): then every certificate is simply untrusted.
        X509_STORE* store = X509_STORE_new();
        X509_STORE_set_flags(store, X509_V_FLAG_PARTIAL_CHAIN);
        SSL_CTX_set_cert_store(&ctx, store);                // takes ownership
        SSL_CTX_set_verify(&ctx, SSL_VERIFY_PEER, acceptAndRecord);
        // Every connection gets a full verification: no resumed sessions.
        SSL_CTX_set_session_cache_mode(&ctx, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(&ctx, SSL_OP_NO_TICKET);
        return true;
    });
    if (!server_->is_valid()) {
        server_.reset();
        return false;
    }

    if (!opts_.trust_pem.empty()) addTrustAnchors(opts_.trust_pem);
    if (trust_all_.empty()) LOG_WARN("device api: no trust anchors yet — every client certificate is refused until the device CA is set up");

    server_->set_keep_alive_max_count(1);      // one request per connection (B5)
    server_->set_payload_max_length(kMaxBodyBytes);
    server_->set_read_timeout(10, 0);
    server_->set_write_timeout(10, 0);
    const std::size_t workers = std::max<std::size_t>(opts_.workers, 2);
    server_->new_task_queue = [workers] { return new httplib::ThreadPool(workers); };

    // ── the gate ────────────────────────────────────────────────────────
    server_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Server-Time", rfc3339(nowSeconds()));
        std::string who = idInPath(req.path);       // best guess of the identity, refined below
        auto deny = [&](int status, const char* code) {
            reply(res, status, code);
            DeviceAuditEvent ev;
            ev.action        = "player.device_denied";
            ev.access_denial = true;
            ev.player_id     = who;
            ev.client_ip     = req.remote_addr;
            ev.http_method   = req.method;
            ev.http_path     = req.path;
            ev.http_status   = status;
            ev.reason        = code;
            emitAudit(std::move(ev));
            return httplib::Server::HandlerResponse::Handled;
        };

        X509* cert = req.ssl ? SSL_get1_peer_certificate(req.ssl) : nullptr;
        if (!cert) return deny(401, "cert_untrusted");
        struct Free { X509* c; ~Free() { X509_free(c); } } guard{cert};

        if (const long vr = SSL_get_verify_result(req.ssl); vr != X509_V_OK)
            return deny(401, vr == X509_V_ERR_CERT_HAS_EXPIRED ? "cert_expired" : "cert_untrusted");

        const std::string cn     = commonName(cert);
        const std::string serial = serialHex(cert);
        if (cn.empty() || serial.empty()) return deny(401, "cert_untrusted");
        who = cn;

        if (const auto seg = idInPath(req.path); !seg.empty() && seg != cn) return deny(403, "cn_mismatch");

        const auto player = db_.findPlayer(cn);
        if (!player) return deny(404, "player_not_found");
        if (player->state == PlayerState::Revoked || revocations_.isRevoked(serial))
            return deny(401, "cert_revoked");

        // The database decides: a certificate it never issued is not ours,
        // however well it chains.
        const auto want = RevocationList::normalise(serial);
        const CertRow* row = nullptr;
        const auto certs = db_.certsOf(cn);
        for (const auto& c : certs)
            if (RevocationList::normalise(c.serial_hex) == want) { row = &c; break; }
        if (!row) return deny(401, "cert_untrusted");
        if (row->revoked_at) return deny(401, "cert_revoked");

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ── routes (reachable only through the gate) ────────────────────────
    server_->Patch(R"(/api/players/([^/]+)/heartbeat)",
        [this](const httplib::Request& req, httplib::Response& res) {
        handler_calls_.fetch_add(1);
        const std::string id = req.matches[1];
        const std::int64_t now = nowSeconds();

        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) { reply(res, 400, "invalid_heartbeat"); return; }

        // "startup": true marks the first heartbeat after a process start. Only a
        // real boolean true counts.
        const bool startup = body.contains("startup") && body["startup"].is_boolean() && body["startup"].get<bool>();

        {   // slow down floods: one heartbeat per interval and player
            std::lock_guard<std::mutex> lk(hb_mu_);
            if (last_heartbeat_.size() > 20000) {
                for (auto it = last_heartbeat_.begin(); it != last_heartbeat_.end();)
                    it = (now - it->second > 3600) ? last_heartbeat_.erase(it) : std::next(it);
                for (auto it = last_startup_.begin(); it != last_startup_.end();)
                    it = (now - it->second > 3600) ? last_startup_.erase(it) : std::next(it);
            }
            // A start is exempt from the ordinary spacing (a player that restarts
            // within 10 s must still be heard) but has a spacing of its own.
            const auto& table = startup ? last_startup_ : last_heartbeat_;
            const std::int64_t spacing = startup ? opts_.min_startup_interval_s : opts_.min_heartbeat_interval_s;
            auto it = table.find(id);
            if (it != table.end() && now - it->second < spacing) {
                res.status = 429;
                res.set_header("Retry-After", std::to_string(spacing - (now - it->second)));
                res.set_content(json{{"error", "rate_limited"}}.dump(), "application/json");
                return;
            }
            if (startup) last_startup_[id] = now;
            last_heartbeat_[id] = now;
        }

        std::string version;
        if (body.contains("version") && body["version"].is_string())
            version = cleanText(body["version"].get<std::string>(), 64);

        // Online/offline, version changes and the batched status write all
        // live in PresenceService.
        presence_->onHeartbeat(id, version, sanitiseHeartbeat(body).dump(), startup);
        res.status = 204;
    });

    server_->Post(R"(/api/players/([^/]+)/renew)",
        [this](const httplib::Request& req, httplib::Response& res) {
        handler_calls_.fetch_add(1);
        if (!opts_.renew) { reply(res, 503, "renew_unavailable"); return; }
        const std::string id = req.matches[1];
        const std::int64_t now = nowSeconds();

        {   // renewal is rare: a short pause between attempts is plenty
            std::lock_guard<std::mutex> lk(hb_mu_);
            auto it = last_renew_.find(id);
            if (it != last_renew_.end() && now - it->second < opts_.min_renew_interval_s) {
                res.status = 429;
                res.set_header("Retry-After", std::to_string(opts_.min_renew_interval_s - (now - it->second)));
                res.set_content(json{{"error", "rate_limited"}}.dump(), "application/json");
                return;
            }
            last_renew_[id] = now;
        }

        json body = json::parse(req.body, nullptr, false);
        std::string csr;
        if (body.is_object() && body.contains("csr_pem") && body["csr_pem"].is_string())
            csr = body["csr_pem"].get<std::string>();

        // The gate has already shown the certificate is valid and is this player's.
        std::string serial;
        if (X509* cert = req.ssl ? SSL_get1_peer_certificate(req.ssl) : nullptr) {
            serial = serialHex(cert);
            X509_free(cert);
        }
        const auto out = opts_.renew->renew({id, serial, csr});
        {
            DeviceAuditEvent ev;
            ev.action        = out.audit_action;
            ev.access_denial = out.audit_action.size() > 7 &&
                               out.audit_action.compare(out.audit_action.size() - 7, 7, "_denied") == 0;
            ev.player_id     = id;
            ev.client_ip     = req.remote_addr;
            ev.http_method   = req.method;
            ev.http_path     = req.path;
            ev.http_status   = out.status;
            ev.reason        = out.audit_reason;
            emitAudit(std::move(ev));
        }
        res.status = out.status;
        res.set_content(out.body.dump(), "application/json");
    });

    // ── listen ──────────────────────────────────────────────────────────
    port_ = opts_.port == 0 ? server_->bind_to_any_port(opts_.bind)
                            : (server_->bind_to_port(opts_.bind, opts_.port) ? opts_.port : -1);
    if (port_ <= 0) {
        LOG_ERROR("device api: cannot listen on {}:{}", opts_.bind, opts_.port);
        server_.reset();
        port_ = 0;
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    for (int i = 0; i < 200 && !server_->is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    LOG_INFO("device api: mTLS listening on {}:{}", opts_.bind, port_);
    return true;
}

std::size_t DeviceApi::addTrustAnchors(const std::string& pem) {
    if (!server_ || pem.empty()) return 0;
    if (trust_all_.find(pem) == std::string::npos) trust_all_ += pem;
    X509_STORE* store = SSL_CTX_get_cert_store(server_->ssl_context());
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    std::size_t loaded = 0;
    while (bio) {
        X509* c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!c) break;
        if (X509_STORE_add_cert(store, c) == 1) ++loaded;
        X509_free(c);
    }
    ERR_clear_error();                 // the end of the PEM bundle raises a benign error
    if (bio) BIO_free_all(bio);
    return loaded;
}

void DeviceApi::emitAudit(DeviceAuditEvent ev) {
    if (!opts_.audit) return;
    // Only the normal outcome of a renewal is reported every time; refusals and
    // failures share one row per window.
    if (ev.action != "player.renew") {
        const std::int64_t now = nowSeconds();
        const std::string key = ev.action + '|' + (ev.player_id.empty() ? ev.client_ip : ev.player_id) + '|' + ev.reason;
        std::lock_guard<std::mutex> lk(audit_mu_);
        if (audit_throttle_.size() > 5000) {
            for (auto it = audit_throttle_.begin(); it != audit_throttle_.end();)
                it = (now - it->second.since > opts_.audit_throttle_s) ? audit_throttle_.erase(it) : std::next(it);
        }
        auto& t = audit_throttle_[key];
        if (t.since != 0 && now - t.since < opts_.audit_throttle_s) { ++t.suppressed; return; }
        ev.suppressed = t.suppressed;
        t.since = now;
        t.suppressed = 0;
    }
    opts_.audit(ev);
}

bool DeviceApi::reloadServerCertificate() {
    // Check the new files first: a broken pair must not take the port down.
    {
        SSL_CTX* probe = SSL_CTX_new(TLS_server_method());
        const bool usable = probe &&
            SSL_CTX_use_certificate_chain_file(probe, opts_.cert_path.c_str()) == 1 &&
            SSL_CTX_use_PrivateKey_file(probe, opts_.key_path.c_str(), SSL_FILETYPE_PEM) == 1 &&
            SSL_CTX_check_private_key(probe) == 1;
        SSL_CTX_free(probe);
        ERR_clear_error();
        if (!usable) {
            LOG_ERROR("device api: new server certificate/key are unusable — keeping the old one");
            return false;
        }
    }
    const std::string trust = trust_all_;
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
    opts_.trust_pem = trust;
    if (!startListener()) {
        LOG_ERROR("device api: could not restart the listener after the certificate change");
        return false;
    }
    LOG_INFO("device api: server certificate reloaded on port {}", port_);
    return true;
}

void DeviceApi::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
}

}  // namespace liveqx::players
