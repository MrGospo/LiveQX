#include "players/RenewService.h"

#include "players/RevocationList.h"

#include <ctime>

namespace liveqx::players {
namespace {

RenewOutcome fail(int status, const char* code, const char* action, std::string reason) {
    RenewOutcome o;
    o.status       = status;
    o.body         = {{"error", code}};
    o.audit_action = action;
    o.audit_reason = std::move(reason);
    return o;
}

}  // namespace

RenewService::RenewService(PlayersDb& db, RenewOptions opts) : db_(db), opts_(std::move(opts)) {}

std::int64_t RenewService::nowSeconds() const {
    return opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

RenewOutcome RenewService::renew(const RenewRequest& req) {
    const std::int64_t now = nowSeconds();

    if (req.csr_pem.empty() || req.csr_pem.size() > kMaxCsrBytes)
        return fail(400, "invalid_csr", "player.renew_denied", "csr_size");

    auto ca = opts_.ca ? opts_.ca() : nullptr;
    if (!ca) return fail(503, "ca_unavailable", "player.renew_failed", "ca_unavailable");
    if (opts_.clock_offset_s) {
        if (auto off = opts_.clock_offset_s(); off && std::llabs(*off) > opts_.max_clock_skew_s)
            return fail(503, "server_clock_unsynced", "player.renew_failed",
                        "clock_offset_s=" + std::to_string(*off));
    }

    // The presented certificate must be one we issued to this player.
    const auto want = RevocationList::normalise(req.presented_serial);
    std::optional<CertRow> current;
    for (auto& c : db_.certsOf(req.player_id))
        if (RevocationList::normalise(c.serial_hex) == want) { current = c; break; }
    if (!current || current->revoked_at)
        return fail(401, "cert_untrusted", "player.renew_denied", "presented_certificate_unknown");

    const auto player = db_.findPlayer(req.player_id);
    if (!player || player->state != PlayerState::Active)
        return fail(409, "player_not_active", "player.renew_denied", "player_not_active");

    if (current->not_after - now > static_cast<std::int64_t>(opts_.window_days) * 86400)
        return fail(409, "renew_too_early", "player.renew_denied",
                    "not_after=" + std::to_string(current->not_after));

    tls::IssuedClientCert cert;
    std::string err;
    if (!ca->signClientCsr(req.csr_pem, req.player_id, opts_.validity_days, cert, err, now))
        return fail(400, "invalid_csr", "player.renew_denied", "csr_rejected: " + err);

    CertRow row;
    row.serial_hex = cert.serial_hex;
    row.generation = cert.generation;
    row.player_id  = req.player_id;
    row.issued_at  = now;
    row.not_before = cert.not_before_unix;
    row.not_after  = cert.not_after_unix;
    if (!db_.recordRenewal(row, now))
        return fail(500, "internal_error", "player.renew_failed", "storage_error");

    RenewOutcome o;
    o.status = 200;
    o.body = {{"schema", kSchema}, {"client_cert_pem", cert.cert_pem},
              {"ca_cert_pem", opts_.ca_cert_pem ? opts_.ca_cert_pem() : std::string{}}};
    if (!cert.chain_pem.empty()) o.body["client_chain_pem"] = cert.chain_pem;
    o.audit_action = "player.renew";
    o.audit_reason = "serial=" + cert.serial_hex;
    return o;
}

}  // namespace liveqx::players
