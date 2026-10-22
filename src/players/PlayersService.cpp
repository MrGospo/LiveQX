#include "players/PlayersService.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

namespace liveqx::players {
namespace {

constexpr std::size_t kMaxUrlBytes = 512;

bool plainChars(const std::string& s) {
    return std::none_of(s.begin(), s.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f || c == '?' || c == '#' || c == '@' || c == '\\';
    });
}

bool validPort(const std::string& s) {
    if (s.empty() || s.size() > 5) return false;
    if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    const int v = std::stoi(s);
    return v >= 1 && v <= 65535;
}

// scheme://host[:port][/]  — no credentials, query, fragment or path.
bool validUrl(const std::string& url, const std::string& scheme) {
    if (url.empty() || url.size() > kMaxUrlBytes || !plainChars(url)) return false;
    const std::string prefix = scheme + "://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    std::string auth = url.substr(prefix.size());
    if (!auth.empty() && auth.back() == '/') auth.pop_back();
    if (auth.empty() || auth.find('/') != std::string::npos) return false;

    std::string host, port;
    if (auth.front() == '[') {
        const auto close = auth.find(']');
        if (close == std::string::npos) return false;
        host = auth.substr(1, close - 1);
        const std::string rest = auth.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            port = rest.substr(1);
        }
        if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
                return std::isxdigit(c) || c == ':' || c == '.'; }))
            return false;
    } else {
        const auto colon = auth.rfind(':');
        host = colon == std::string::npos ? auth : auth.substr(0, colon);
        if (colon != std::string::npos) port = auth.substr(colon + 1);
        if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '.' || c == '-' || c == '_'; }))
            return false;
    }
    return port.empty() ? auth.back() != ':' : validPort(port);
}

bool parsesAsCertificate(const std::string& pem) {
    if (pem.empty() || pem.size() > PlayersDb::kMaxSettingBytes) return false;
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!b) return false;
    X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    const bool ok = x != nullptr;
    X509_free(x);
    BIO_free_all(b);
    return ok;
}

std::string rfc3339(std::int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

bool validPolicy(const std::string& p) { return p == "strict" || p == "relaxed"; }

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// nullopt = not a number in range.
std::optional<int> parseTtlDays(const std::string& v) {
    if (v.empty() || v.size() > 3 ||
        !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    const int d = std::stoi(v);
    if (d < 1 || d > 365) return std::nullopt;
    return d;
}

// Seconds, 30 s .. 1 day.
std::optional<int> parseSeconds(const std::string& v) {
    if (v.empty() || v.size() > 6 ||
        !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    const int n = std::stoi(v);
    if (n < 30 || n > 86400) return std::nullopt;
    return n;
}

std::optional<int> parseRange(const std::string& v, int lo, int hi) {
    if (v.empty() || v.size() > 4 ||
        !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    const int n = std::stoi(v);
    if (n < lo || n > hi) return std::nullopt;
    return n;
}

}  // namespace

PlayersService::PlayersService(PlayersDb& db, PlayersServiceOptions opts)
    : db_(db), opts_(std::move(opts)) {}

PlayersSettings PlayersService::settings() {
    const auto m = db_.settings();
    auto get = [&](const char* k) {
        auto it = m.find(k);
        return it == m.end() ? std::string{} : it->second;
    };
    PlayersSettings s;
    s.server_url      = get("server_url");
    s.device_api_url  = get("device_api_url");
    s.mqtt_broker_url = get("mqtt_broker_url");
    s.ca_cert_pem     = get("ca_cert_pem");
    if (auto d = parseTtlDays(get("token_ttl_days"))) s.token_ttl_days = *d;
    if (auto p = get("clock_policy"); validPolicy(p)) s.clock_policy = p;
    if (auto n = parseSeconds(get("command_ttl_s"))) s.command_ttl_s = *n;
    if (auto n = parseSeconds(get("confirm_timeout_restart_s"))) s.confirm_timeout_restart_s = *n;
    if (auto n = parseSeconds(get("confirm_timeout_reboot_s"))) s.confirm_timeout_reboot_s = *n;
    if (auto n = parseSeconds(get("confirm_timeout_reload_s"))) s.confirm_timeout_reload_s = *n;
    if (auto n = parseRange(get("command_batch_size"), 1, 100)) s.command_batch_size = *n;
    if (auto n = parseRange(get("command_batch_stop_percent"), 1, 100)) s.command_batch_stop_percent = *n;
    if (auto n = parseRange(get("events_retention_days"), 7, 3650)) s.events_retention_days = *n;
    if (auto n = parseRange(get("commands_retention_days"), 7, 3650)) s.commands_retention_days = *n;
    return s;
}

nlohmann::json PlayersService::toJson(const PlayersSettings& s) {
    return {{"server_url", s.server_url},
            {"device_api_url", s.device_api_url},
            {"mqtt_broker_url", s.mqtt_broker_url},
            {"ca_cert_pem", s.ca_cert_pem},
            {"token_ttl_days", s.token_ttl_days},
            {"clock_policy", s.clock_policy},
            {"command_ttl_s", s.command_ttl_s},
            {"confirm_timeout_restart_s", s.confirm_timeout_restart_s},
            {"confirm_timeout_reboot_s", s.confirm_timeout_reboot_s},
            {"confirm_timeout_reload_s", s.confirm_timeout_reload_s},
            {"command_batch_size", s.command_batch_size},
            {"command_batch_stop_percent", s.command_batch_stop_percent},
            {"events_retention_days", s.events_retention_days},
            {"commands_retention_days", s.commands_retention_days}};
}

std::string PlayersService::updateSettings(const std::map<std::string, std::string>& patch) {
    for (const auto& [k, v] : patch) {
        if (k == "server_url")           { if (!v.empty() && !validUrl(v, "https"))  return "invalid_server_url"; }
        else if (k == "device_api_url")  { if (!v.empty() && !validUrl(v, "https"))  return "invalid_device_api_url"; }
        else if (k == "mqtt_broker_url") { if (!v.empty() && !validUrl(v, "mqtts"))  return "invalid_mqtt_broker_url"; }
        else if (k == "ca_cert_pem")     { if (!v.empty() && !parsesAsCertificate(v)) return "invalid_ca_cert_pem"; }
        else if (k == "token_ttl_days")  { if (!v.empty() && !parseTtlDays(v))        return "invalid_token_ttl_days"; }
        else if (k == "clock_policy")    { if (!v.empty() && !validPolicy(v))         return "invalid_clock_policy"; }
        else if (k == "command_ttl_s" || k == "confirm_timeout_restart_s" ||
                 k == "confirm_timeout_reboot_s" || k == "confirm_timeout_reload_s") {
            if (!v.empty() && !parseSeconds(v)) return "invalid_" + k;
        }
        else if (k == "command_batch_size" || k == "command_batch_stop_percent") {
            if (!v.empty() && !parseRange(v, 1, 100)) return "invalid_" + k;
        }
        else if (k == "events_retention_days" || k == "commands_retention_days") {
            if (!v.empty() && !parseRange(v, 7, 3650)) return "invalid_" + k;
        }
        else return "unknown_setting";
    }
    return db_.setSettings(patch) ? "" : "storage_error";
}

RetentionReport PlayersService::applyRetention(std::int64_t now) {
    if (now == 0) now = nowSeconds();
    const PlayersSettings s = settings();
    constexpr std::int64_t kDay = 86400;
    RetentionReport r;
    r.events       = db_.purgeEventsOlderThan(now - s.events_retention_days * kDay);
    r.commands     = db_.purgeSettledCommandsOlderThan(now - s.commands_retention_days * kDay);
    r.jobs         = db_.purgeFinishedJobsOlderThan(now - s.commands_retention_days * kDay);
    // A certificate that expired is no use to anyone; a month's margin keeps the
    // record around for any question about a connection that was refused for it.
    r.certificates = db_.purgeExpiredCerts(now - 30 * kDay);
    return r;
}

mqtt::BrokerConnection PlayersService::brokerConnection(const mqtt::BrokerConnection& base) {
    const auto m = db_.settings();
    mqtt::BrokerConnection c = base;
    auto get = [&](const char* k) -> const std::string* {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    };
    if (auto* v = get("broker_host")) c.host = *v;
    if (auto* v = get("broker_port"); v && !v->empty() && v->size() <= 5 &&
        std::all_of(v->begin(), v->end(), [](unsigned char ch) { return std::isdigit(ch); }))
        c.port = std::stoi(*v);
    if (auto* v = get("broker_verify_hostname")) c.verify_hostname = (*v != "false");
    if (auto* v = get("broker_reconcile_interval_s"); v && !v->empty() && v->size() <= 6 &&
        std::all_of(v->begin(), v->end(), [](unsigned char ch) { return std::isdigit(ch); }))
        c.reconcile_interval_s = std::stoi(*v);
    return mqtt::validateBrokerConnection(c).empty() ? c : base;
}

std::string PlayersService::storeBrokerConnection(const mqtt::BrokerConnection& c) {
    if (auto e = mqtt::validateBrokerConnection(c); !e.empty()) return e;
    return db_.setSettings({{"broker_host", c.host},
                            {"broker_port", std::to_string(c.port)},
                            {"broker_verify_hostname", c.verify_hostname ? "true" : "false"},
                            {"broker_reconcile_interval_s", std::to_string(c.reconcile_interval_s)}})
               ? "" : "storage_error";
}

std::string PlayersService::caCertPem() {
    const PlayersSettings s = settings();
    std::string pem = !s.ca_cert_pem.empty() ? s.ca_cert_pem : slurp(opts_.ca_cert_path);
    return parsesAsCertificate(pem) ? pem : std::string{};
}

std::string PlayersService::checkReady(const PlayersSettings& s, std::string& ca_pem) {
    if (s.server_url.empty() || s.device_api_url.empty() || s.mqtt_broker_url.empty())
        return "not_configured";
    ca_pem = !s.ca_cert_pem.empty() ? s.ca_cert_pem : slurp(opts_.ca_cert_path);
    if (!parsesAsCertificate(ca_pem)) return "ca_unavailable";
    return "";
}

std::string PlayersService::buildPackage(const PlayerRow& p, const std::string& token,
                                         std::int64_t expires_at, nlohmann::json& out) {
    const PlayersSettings s = settings();
    std::string ca;
    if (auto e = checkReady(s, ca); !e.empty()) return e;
    out = {{"schema", kPackageSchema},
           {"player_id", p.id},
           {"server_url", s.server_url},
           {"ca_cert_pem", ca},
           {"device_api_url", s.device_api_url},
           {"mqtt_broker_url", s.mqtt_broker_url},
           {"bootstrap_token", token},
           {"token_expires_at", rfc3339(expires_at)},
           {"clock_policy", p.clock_policy}};
    if (opts_.ntp_hint) {
        auto hint = opts_.ntp_hint();
        if (!hint.empty()) out["ntp_hint"] = std::move(hint);
    }
    return "";
}

PairingResult PlayersService::create(const std::string& name, std::optional<std::int64_t> created_by,
                                     std::optional<std::string> clock_policy, std::int64_t now,
                                     std::optional<std::int64_t> group_id) {
    PairingResult r;
    const PlayersSettings s = settings();
    const std::string policy = clock_policy ? *clock_policy : s.clock_policy;
    if (!validPolicy(policy)) { r.error = "invalid_clock_policy"; return r; }
    // Where a player sits decides who may see and command it, so it is never
    // created "nowhere".
    if (!group_id) { r.error = "group_required"; return r; }
    if (!db_.findGroup(*group_id)) { r.error = "group_not_found"; return r; }
    std::string ca;
    if (auto e = checkReady(s, ca); !e.empty()) { r.error = e; return r; }

    if (now == 0) now = nowSeconds();
    auto created = db_.createPlayer(name, created_by, policy,
                                    static_cast<std::int64_t>(s.token_ttl_days) * 86400, now, group_id);
    if (!created) { r.error = "invalid_name"; return r; }

    if (auto e = buildPackage(created->player, created->token, created->token_expires_at, r.package);
        !e.empty()) {
        // Settings changed under us: do not leave a player nobody can pair.
        db_.deletePlayer(created->player.id, "package_failed", now);
        r.error = e;
        return r;
    }
    r.ok = true;
    r.player = created->player;
    if (opts_.on_player_created) opts_.on_player_created(r.player.id);
    if (opts_.events) {
        nlohmann::json payload = {{"player_id", r.player.id}, {"name", r.player.name}};
        if (auto place = db_.placeOfPlayer(r.player.id)) {
            payload["group_id"]  = place->group_id;
            payload["object_id"] = place->object_id;
        }
        opts_.events->publish(events::EventType::PlayerRegistered, -1, std::move(payload));
    }
    return r;
}

PairingResult PlayersService::reissueToken(const std::string& player_id, std::int64_t now) {
    PairingResult r;
    auto p = db_.findPlayer(player_id);
    if (!p) { r.error = "not_found"; return r; }
    if (p->state != PlayerState::Pending) { r.error = "not_pending"; return r; }
    const PlayersSettings s = settings();
    std::string ca;
    if (auto e = checkReady(s, ca); !e.empty()) { r.error = e; return r; }

    if (now == 0) now = nowSeconds();
    auto t = db_.reissueToken(player_id, static_cast<std::int64_t>(s.token_ttl_days) * 86400, now);
    if (!t) { r.error = "storage_error"; return r; }
    if (auto e = buildPackage(*p, t->token, t->token_expires_at, r.package); !e.empty()) {
        r.error = e;
        return r;
    }
    r.ok = true;
    r.player = *p;
    return r;
}

PairingResult PlayersService::reEnroll(const std::string& player_id, const std::string& reason,
                                       std::int64_t now) {
    PairingResult r;
    auto p = db_.findPlayer(player_id);
    if (!p) { r.error = "not_found"; return r; }
    if (p->state == PlayerState::Pending) { r.error = "already_pending"; return r; }
    // Checked BEFORE anything changes: a screen must not be cut off when the
    // package that re-binds it cannot be made.
    const PlayersSettings s = settings();
    std::string ca;
    if (auto e = checkReady(s, ca); !e.empty()) { r.error = e; return r; }

    if (now == 0) now = nowSeconds();
    auto done = db_.reEnrollPlayer(player_id, reason, static_cast<std::int64_t>(s.token_ttl_days) * 86400, now);
    if (!done) { r.error = "storage_error"; return r; }
    // The locks first (in the server and at the broker), then the rebuild of the record.
    if (!done->revoked.empty() && opts_.on_certs_revoked) opts_.on_certs_revoked(done->revoked);
    if (opts_.on_player_reset) opts_.on_player_reset(player_id);

    p = db_.findPlayer(player_id);
    if (!p) { r.error = "not_found"; return r; }
    if (auto e = buildPackage(*p, done->token.token, done->token.token_expires_at, r.package); !e.empty()) {
        r.error = e;                                    // settings changed under us; the screen is "pending", a new token can be issued
        return r;
    }
    r.ok = true;
    r.player = *p;
    r.revoked_certs = done->revoked.size();
    r.previous_state = done->previous;
    return r;
}

RemovalResult PlayersService::revoke(const std::string& player_id, const std::string& reason,
                                     std::int64_t now) {
    RemovalResult out;
    auto certs = db_.revokePlayer(player_id, reason, now);
    if (!certs) return out;
    out.found = true;
    out.revoked_certs = certs->size();
    if (opts_.on_player_gone) opts_.on_player_gone(player_id);
    if (!certs->empty() && opts_.on_certs_revoked) opts_.on_certs_revoked(*certs);
    return out;
}

RemovalResult PlayersService::remove(const std::string& player_id, const std::string& reason,
                                     std::int64_t now) {
    // Revoke first: the certificates must be on the CRL before the player
    // (and with it the link player -> certificate) disappears.
    RemovalResult out = revoke(player_id, reason, now);
    if (!out.found) return out;
    if (!db_.deletePlayer(player_id, reason, now)) out.found = false;
    return out;
}

}  // namespace liveqx::players
