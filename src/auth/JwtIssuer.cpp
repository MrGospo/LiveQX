// fix22 commit 4/24 — HS256 JWT issuer/verifier через jwt-cpp + libsodium RNG.
//
// Что в access-токене (claims):
//   - sub:        user_id (string-ified — jwt-cpp хранит claims как строки)
//   - username:   логин (для удобства в audit без второго look-up)
//   - role:       "admin" | "operator" | "viewer"
//   - mcp:        must_change_password (bool) — middleware режет mutating
//                 endpoints до self-password-change, не дёргая БД
//   - jti:        UUID-like 16-байт случайных, base64 — persisted в
//                 sessions.jwt_id для logout/refresh revocation
//   - grants:     [{cid, perm}, ...] — per-channel ACL прямо в токене
//                 (избегает БД lookup'а на каждом запросе)
//   - iat / exp:  стандартные, secs since epoch
//
// Refresh-токен — opaque random 32 bytes (URL-safe base64), не JWT.
// Это сознательно: refresh нужен только в БД (sessions.refresh_token_hash
// argon2id), JWT-структура там избыточна. На каждый refresh старая
// строка revoked, новая issued.

#include "auth/JwtIssuer.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

// jwt-cpp использует picojson по умолчанию — он внутри jwt-cpp/picojson.h.
// Мы могли бы перевести на nlohmann/json (default traits), но это не стоит
// возни — picojson здесь stays внутри одного .cpp.
#include <jwt-cpp/jwt.h>
#include <sodium.h>

#include "utils/Log.h"

namespace liveqx::auth {
namespace {

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 16 random bytes → URL-safe base64 без padding ≈ 22 ascii chars.
// Достаточно для UUID-like ID; коллизия ~ 2^-128.
std::string makeOpaqueId(std::size_t n_bytes) {
    std::vector<unsigned char> buf(n_bytes);
    randombytes_buf(buf.data(), buf.size());
    const std::size_t enc_max = sodium_base64_ENCODED_LEN(
        n_bytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::vector<char> out(enc_max);
    sodium_bin2base64(out.data(), out.size(),
                      buf.data(), buf.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_memzero(buf.data(), buf.size());
    return std::string(out.data());
}

// Грантс пакуем в массив объектов {cid:int, perm:string}, чтобы JSON
// был самодостаточен и читаем глазами при дебаге.
picojson::value grantsToJson(const std::vector<ChannelGrant>& grants) {
    picojson::array arr;
    arr.reserve(grants.size());
    for (const auto& g : grants) {
        picojson::object o;
        o["cid"]  = picojson::value(static_cast<double>(g.channel_id));
        o["perm"] = picojson::value(std::string(channelPermissionName(g.permission)));
        arr.push_back(picojson::value(o));
    }
    return picojson::value(arr);
}

std::vector<ChannelGrant> grantsFromJson(const picojson::value& v) {
    std::vector<ChannelGrant> out;
    if (!v.is<picojson::array>()) return out;
    for (const auto& item : v.get<picojson::array>()) {
        if (!item.is<picojson::object>()) continue;
        const auto& o = item.get<picojson::object>();
        ChannelGrant g;
        auto it_cid  = o.find("cid");
        auto it_perm = o.find("perm");
        if (it_cid == o.end() || !it_cid->second.is<double>())  continue;
        if (it_perm == o.end() || !it_perm->second.is<std::string>()) continue;
        g.channel_id = static_cast<std::int64_t>(it_cid->second.get<double>());
        auto perm = channelPermissionFromString(it_perm->second.get<std::string>());
        if (!perm) continue;
        g.permission = *perm;
        out.push_back(g);
    }
    return out;
}

}  // namespace

JwtIssuer::JwtIssuer(std::string signing_secret)
    : secret_(std::move(signing_secret)) {
    if (secret_.size() < 32) {
        // 256-битный secret — минимум для HS256. Меньше — security smell.
        // Throw здесь намеренно: ConstructionError в startup лучше, чем
        // тихо подписывать слабым ключом.
        throw std::invalid_argument(
            "JwtIssuer: signing secret must be at least 32 bytes for HS256");
    }
}

JwtIssuer::TokenPair JwtIssuer::issue(
    const User& user,
    const std::vector<ChannelGrant>& grants) {

    const std::int64_t now      = nowUnixSec();
    const std::int64_t access_e = now +
        std::chrono::duration_cast<std::chrono::seconds>(kAccessTtl).count();
    const std::int64_t refresh_e = now +
        std::chrono::duration_cast<std::chrono::seconds>(kRefreshTtl).count();

    const std::string jti = makeOpaqueId(16);

    auto token = jwt::create()
        .set_issuer("liveqx")
        .set_subject(std::to_string(user.id))
        .set_id(jti)
        .set_issued_at(std::chrono::system_clock::from_time_t(now))
        .set_expires_at(std::chrono::system_clock::from_time_t(access_e))
        .set_payload_claim("username",
            jwt::claim(user.username))
        .set_payload_claim("role",
            jwt::claim(std::string(roleName(user.role))))
        .set_payload_claim("mcp",
            jwt::claim(picojson::value(user.must_change_password)))
        .set_payload_claim("grants",
            jwt::claim(grantsToJson(grants)))
        .sign(jwt::algorithm::hs256{secret_});

    TokenPair out;
    out.access_token       = token;
    out.refresh_token      = makeOpaqueId(32);  // ~43 ascii chars
    out.jwt_id             = jti;
    out.access_expires_at  = access_e;
    out.refresh_expires_at = refresh_e;
    return out;
}

std::optional<JwtIssuer::Claims> JwtIssuer::verify(std::string_view access_token) {
    if (access_token.empty()) return std::nullopt;

    try {
        auto decoded = jwt::decode(std::string(access_token));

        auto verifier = jwt::verify()
            .with_issuer("liveqx")
            .allow_algorithm(jwt::algorithm::hs256{secret_});

        // throws на signature / exp / iss mismatch.
        verifier.verify(decoded);

        Claims c;
        c.exp = std::chrono::duration_cast<std::chrono::seconds>(
            decoded.get_expires_at().time_since_epoch()).count();
        c.jti = decoded.get_id();

        try {
            c.user_id = std::stoll(decoded.get_subject());
        } catch (...) {
            return std::nullopt;
        }

        c.username = decoded.get_payload_claim("username").as_string();

        const auto role_str = decoded.get_payload_claim("role").as_string();
        auto role_opt = roleFromString(role_str);
        if (!role_opt) return std::nullopt;
        c.role = *role_opt;

        const auto mcp_v = decoded.get_payload_claim("mcp").as_boolean();
        c.must_change_password = mcp_v;

        c.channel_grants = grantsFromJson(
            decoded.get_payload_claim("grants").to_json());

        return c;
    } catch (const std::exception& e) {
        // Неверная подпись / истёкший токен / поломанный формат —
        // ожидаемые случаи. DEBUG, не error.
        LOG_DEBUG("JwtIssuer::verify rejected token: {}", e.what());
        return std::nullopt;
    }
}

}  // namespace liveqx::auth
