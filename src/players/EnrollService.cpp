#include "players/EnrollService.h"

#include "auth/SealedServer.h"

#include <algorithm>
#include <ctime>

namespace liveqx::players {
namespace {

bool looksLikeUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

EnrollOutcome fail(int status, const char* code, const char* action, std::string reason,
                   std::string player_id = {}) {
    EnrollOutcome o;
    o.status       = status;
    o.body         = {{"error", code}};
    o.audit_action = action;
    o.audit_reason = std::move(reason);
    o.player_id    = std::move(player_id);
    return o;
}

const char* reasonName(ClaimFailure f) {
    switch (f) {
        case ClaimFailure::NoMatch:       return "token_mismatch";
        case ClaimFailure::Expired:       return "token_expired";
        case ClaimFailure::AlreadyUsed:   return "token_used";
        case ClaimFailure::UnknownPlayer: return "unknown_player";
        case ClaimFailure::WrongState:    return "player_not_pending";
        case ClaimFailure::None:          break;
    }
    return "token_rejected";
}

}  // namespace

EnrollService::EnrollService(PlayersDb& db, EnrollOptions opts)
    : db_(db), opts_(std::move(opts)) {
    // 3 free attempts, then 2, 4, 8 ... up to 5 minutes.
    limiter_ = std::make_unique<auth::UnlockLimiter>(
        [this] { return nowSeconds(); });
}

EnrollService::~EnrollService() = default;

std::int64_t EnrollService::nowSeconds() const {
    return opts_.now ? opts_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

EnrollOutcome EnrollService::enroll(const EnrollRequest& req) {
    const std::int64_t now = nowSeconds();
    const bool id_ok = looksLikeUuid(req.player_id);
    // Ids that are not ids share one bucket, so the table cannot be inflated
    // with junk keys.
    const std::string ip_key     = "ip:" + (req.client_ip.empty() ? std::string("unknown") : req.client_ip);
    const std::string player_key = "player:" + (id_ok ? req.player_id : std::string("invalid"));

    // 1. Rate limit — before anything is looked up.
    const int wait = std::max(limiter_->retryAfter(ip_key), limiter_->retryAfter(player_key));
    if (wait > 0) {
        auto o = fail(429, "rate_limited", "player.enroll_denied", "rate_limited",
                      id_ok ? req.player_id : std::string{});
        o.retry_after_s = wait;
        return o;
    }

    // 2. Cheap shape checks that need no state.
    if (req.csr_pem.empty() || req.csr_pem.size() > kMaxCsrBytes)
        return fail(400, "invalid_csr", "player.enroll_denied", "csr_size", id_ok ? req.player_id : std::string{});

    // 3. Server-side readiness, before the token is touched: a token must not
    //    burn because of OUR problem.
    auto ca = opts_.ca ? opts_.ca() : nullptr;
    if (!ca)
        return fail(503, "ca_unavailable", "player.enroll_failed", "ca_unavailable",
                    id_ok ? req.player_id : std::string{});
    if (opts_.clock_offset_s) {
        if (auto off = opts_.clock_offset_s(); off && std::llabs(*off) > opts_.max_clock_skew_s)
            return fail(503, "server_clock_unsynced", "player.enroll_failed",
                        "clock_offset_s=" + std::to_string(*off), id_ok ? req.player_id : std::string{});
    }

    // 4. The token.
    ClaimResult claim;
    if (id_ok) claim = db_.claimToken(req.player_id, req.bootstrap_token, now);
    else       claim.reason = ClaimFailure::UnknownPlayer;
    if (!claim.ok) {
        limiter_->recordFailure(ip_key);
        limiter_->recordFailure(player_key);
        return fail(401, "invalid_token", "player.enroll_denied",
                    id_ok ? reasonName(claim.reason) : "malformed_player_id",
                    id_ok ? req.player_id : std::string{});
    }

    // 5. Sign. The CN is the player id whatever the CSR says.
    tls::IssuedClientCert cert;
    std::string err;
    if (!ca->signClientCsr(req.csr_pem, req.player_id, opts_.validity_days, cert, err, now)) {
        db_.releaseClaim(claim.token_id);
        return fail(400, "invalid_csr", "player.enroll_denied", "csr_rejected: " + err, req.player_id);
    }

    // 6. Record: token, state, certificate, event — one transaction.
    CertRow row;
    row.serial_hex = cert.serial_hex;
    row.generation = cert.generation;
    row.player_id  = req.player_id;
    row.issued_at  = now;
    row.not_before = cert.not_before_unix;
    row.not_after  = cert.not_after_unix;
    if (!db_.finishEnrollment(claim.token_id, row, now)) {
        db_.releaseClaim(claim.token_id);
        return fail(500, "internal_error", "player.enroll_failed", "storage_error", req.player_id);
    }

    limiter_->recordSuccess(ip_key);
    limiter_->recordSuccess(player_key);

    EnrollOutcome o;
    o.status = 200;
    o.body = {{"schema", kSchema}, {"client_cert_pem", cert.cert_pem}};
    if (!cert.chain_pem.empty()) o.body["client_chain_pem"] = cert.chain_pem;
    o.audit_action = "player.enroll";
    o.audit_reason = "serial=" + cert.serial_hex;
    o.player_id    = req.player_id;
    return o;
}

}  // namespace liveqx::players
