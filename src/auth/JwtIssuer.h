#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "auth/AuthTypes.h"

namespace liveqx::auth {

// HS256 JWT issuer/verifier (jwt-cpp). Signing secret comes encrypted
// from auth.db, decrypted via MasterKey at startup, held in RAM here.
//
// Implementation lands in commit 4/24 of fix22.
class JwtIssuer {
public:
    struct TokenPair {
        std::string access_token;     // 1h TTL
        std::string refresh_token;    // 30d TTL, opaque random; hash persisted
        std::string jwt_id;           // jti claim, persisted in sessions.jwt_id
        std::int64_t access_expires_at{0};
        std::int64_t refresh_expires_at{0};
    };

    struct Claims {
        std::int64_t user_id{0};
        std::string  username;
        Role         role{Role::Viewer};
        bool         must_change_password{false};
        std::int64_t exp{0};
        std::string  jti;
        std::vector<ChannelGrant> channel_grants;
    };

    explicit JwtIssuer(std::string signing_secret);

    // Issue a fresh (access, refresh) pair for the given user.
    TokenPair issue(const User& user, const std::vector<ChannelGrant>& grants);

    // Verify signature + expiry. Caller still has to check
    // sessions.revoked_at against jti.
    std::optional<Claims> verify(std::string_view access_token);

    static constexpr auto kAccessTtl  = std::chrono::hours(1);
    static constexpr auto kRefreshTtl = std::chrono::hours(24 * 30);

private:
    std::string secret_;
};

}  // namespace liveqx::auth
