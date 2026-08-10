// fix22 commit 5/24 — login/logout/refresh оркестратор.
//
// Refresh rotation:
//   refresh(rt) → найти session по SHA-256(rt) → проверить revoked_at IS
//   NULL и expires_at > now → revoke этой сессии → создать новую
//   (новый jti, новый refresh, новая пара). Атакующий, успевший украсть
//   refresh, на первом же его использовании выдаёт себя — старый refresh
//   уже revoked, легитимный клиент при попытке использовать тот же
//   получит 401 (replay detection).
//
// Логирование:
//   - login OK / refresh OK     → INFO
//   - login fail (wrong creds)  → WARN (нужно для brute-force detection
//                                 в commit 13)
//   - refresh не нашёлся / revoked → WARN (сигнал атаки)
//   - DB ошибки                 → ERROR

#include "auth/AuthService.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include "auth/AuthDb.h"
#include "auth/PasswordHasher.h"
#include "utils/Log.h"

namespace liveqx::auth {
namespace {

std::int64_t nowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// fix35 (A3.16) — write the bootstrap admin's plaintext password into
// `path` with mode 0600. Atomic via O_CREAT|O_EXCL temp + rename so an
// operator's `cat initial_admin_password.txt` never sees a half-written
// file. Best-effort: on failure we LOG_ERROR and let the caller fall
// back to logging the password (which is what bootstrap did pre-fix35).
bool writeInitialAdminPasswordFile(const std::filesystem::path& path,
                                   std::string_view plaintext) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        // create_directories failing isn't necessarily fatal — open()
        // below will surface a useful errno if the dir is genuinely
        // unwritable. We just don't want to skip it.
    }

    const fs::path tmp = path.string() + ".new";
    fs::remove(tmp, ec);  // sweep any leftover from a crashed previous bootstrap

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        LOG_ERROR("AuthService: cannot create {}: {}",
                  tmp.string(), std::strerror(errno));
        return false;
    }
    // Single newline at end so `cat` prints cleanly. We deliberately
    // do NOT add a comment header — keeping the file content == the
    // password (plus \n) means an operator can pipe it straight into
    // a paste buffer or env var without manual stripping.
    std::string body(plaintext);
    body.push_back('\n');
    const ssize_t written = ::write(fd, body.data(), body.size());
    if (written != static_cast<ssize_t>(body.size())) {
        LOG_ERROR("AuthService: short write to {}", tmp.string());
        ::close(fd);
        fs::remove(tmp, ec);
        return false;
    }
    if (::fsync(fd) != 0) {
        LOG_WARN("AuthService: fsync({}) failed: {}",
                 tmp.string(), std::strerror(errno));
    }
    ::close(fd);
    ::chmod(tmp.c_str(), 0600);

    fs::rename(tmp, path, ec);
    if (ec) {
        LOG_ERROR("AuthService: rename({} -> {}): {}",
                  tmp.string(), path.string(), ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

const char* loginErrorName(AuthService::LoginError e) noexcept {
    switch (e) {
        case AuthService::LoginError::UserNotFound:           return "user_not_found";
        case AuthService::LoginError::InvalidPassword:        return "invalid_password";
        case AuthService::LoginError::UserDisabled:           return "user_disabled";
        case AuthService::LoginError::InitialPasswordExpired: return "initial_password_expired";
        case AuthService::LoginError::AccountLocked:          return "account_locked";
        case AuthService::LoginError::InternalError:          return "internal_error";
    }
    return "unknown";
}

const char* refreshErrorName(AuthService::RefreshError e) noexcept {
    switch (e) {
        case AuthService::RefreshError::TokenNotFound:    return "token_not_found";
        case AuthService::RefreshError::TokenRevoked:     return "token_revoked";
        case AuthService::RefreshError::TokenExpired:     return "token_expired";
        case AuthService::RefreshError::UserDisabled:     return "user_disabled";
        case AuthService::RefreshError::LdapCacheExpired: return "ldap_cache_expired";
        case AuthService::RefreshError::InternalError:    return "internal_error";
    }
    return "unknown";
}

const char* adminErrorName(AuthService::AdminError e) noexcept {
    switch (e) {
        case AuthService::AdminError::UsernameInvalid: return "username_invalid";
        case AuthService::AdminError::EmailInvalid:    return "email_invalid";
        case AuthService::AdminError::PasswordInvalid: return "password_invalid";
        case AuthService::AdminError::UsernameTaken:   return "username_taken";
        case AuthService::AdminError::UserNotFound:    return "user_not_found";
        case AuthService::AdminError::InternalError:   return "internal_error";
    }
    return "unknown";
}

const char* selfPasswordErrorName(AuthService::SelfPasswordError e) noexcept {
    switch (e) {
        case AuthService::SelfPasswordError::InvalidSession:       return "invalid_session";
        case AuthService::SelfPasswordError::UserNotFound:         return "user_not_found";
        case AuthService::SelfPasswordError::UserDisabled:         return "user_disabled";
        case AuthService::SelfPasswordError::CurrentPasswordWrong: return "current_password_wrong";
        case AuthService::SelfPasswordError::NewPasswordWeak:      return "new_password_weak";
        case AuthService::SelfPasswordError::InternalError:        return "internal_error";
    }
    return "unknown";
}

std::string AuthService::hashRefreshToken(std::string_view plaintext) {
    // SHA-256 хватает: refresh уже 32 random bytes, поиск по hash'у
    // должен быть deterministic + быстрый (без salt). Argon2id здесь
    // не нужен — нет brute-force-поверхности.
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest,
        reinterpret_cast<const unsigned char*>(plaintext.data()),
        plaintext.size());

    // hex encoding (64 chars) — читается глазами в дебаге.
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(crypto_hash_sha256_BYTES * 2);
    for (std::size_t i = 0; i < crypto_hash_sha256_BYTES; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[digest[i]        & 0xF];
    }
    return out;
}

// fix22 c11/24: 24h TTL для bootstrap/admin-reset паролей. Юзер обязан
// логинуться и сменить пароль за это окно; иначе login отвергается, и
// admin должен повторно ресетнуть. Сделано на уровне service'а
// (а не migration default), чтобы было перенастраиваемо в будущем.
constexpr std::int64_t kInitialPasswordTtlSec = 24 * 60 * 60;

AuthService::AuthService(AuthDb& db, JwtIssuer& jwt, GrantsResolver g)
    : db_(db), jwt_(jwt), grants_for_user_(std::move(g)) {}

AuthService::AuthService(AuthDb& db, JwtIssuer& jwt,
                         GrantsResolver g, LdapAuthenticator a)
    : db_(db), jwt_(jwt),
      grants_for_user_(std::move(g)),
      ldap_authenticator_(std::move(a)) {}

namespace {

std::string lowercaseAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    return out;
}

int rolePriority(Role r) noexcept {
    switch (r) {
        case Role::Admin:    return 3;
        case Role::Operator: return 2;
        case Role::Viewer:   return 1;
    }
    return 0;
}

}  // namespace

namespace {

int permissionPriority(ChannelPermission p) noexcept {
    switch (p) {
        case ChannelPermission::Operate: return 2;
        case ChannelPermission::View:    return 1;
    }
    return 0;
}

}  // namespace

std::vector<ChannelGrant> AuthService::pickAclGrantsForGroups(
    const std::vector<std::string>& groups,
    const std::vector<ChannelAclEntry>& channel_acl) {
    std::vector<ChannelGrant> out;
    if (groups.empty() || channel_acl.empty()) return out;

    // Для каждой ACL-записи ищем matching-группы юзера и берём
    // максимальную permission. Между несколькими ACL-записями с одним
    // channel_id мерджим аналогично (Operate > View).
    std::vector<std::string> groups_lc;
    groups_lc.reserve(groups.size());
    for (const auto& g : groups) groups_lc.push_back(lowercaseAscii(g));

    // channel_id → текущая выбранная permission.
    std::map<std::int64_t, ChannelPermission> picked;
    for (const auto& acl : channel_acl) {
        std::optional<ChannelPermission> best;
        for (const auto& [k, perm] : acl.group_perms) {
            const auto k_lc = lowercaseAscii(k);
            for (const auto& g_lc : groups_lc) {
                if (g_lc == k_lc) {
                    if (!best ||
                        permissionPriority(perm) > permissionPriority(*best)) {
                        best = perm;
                    }
                    break;
                }
            }
        }
        if (!best) continue;
        auto it = picked.find(acl.channel_id);
        if (it == picked.end() ||
            permissionPriority(*best) > permissionPriority(it->second)) {
            picked[acl.channel_id] = *best;
        }
    }
    out.reserve(picked.size());
    for (const auto& [cid, perm] : picked) {
        out.push_back(ChannelGrant{cid, perm});
    }
    return out;
}

std::optional<Role> AuthService::pickRoleForGroups(
    const std::vector<std::string>& groups,
    const std::map<std::string, Role>& group_role_map) {
    if (groups.empty() || group_role_map.empty()) return std::nullopt;
    // Нормализуем ключи map'а один раз.
    std::vector<std::pair<std::string, Role>> normalized;
    normalized.reserve(group_role_map.size());
    for (const auto& [k, r] : group_role_map) {
        normalized.emplace_back(lowercaseAscii(k), r);
    }
    std::optional<Role> best;
    for (const auto& g : groups) {
        const auto g_lc = lowercaseAscii(g);
        for (const auto& [k_lc, role] : normalized) {
            if (g_lc == k_lc) {
                if (!best || rolePriority(role) > rolePriority(*best)) {
                    best = role;
                }
            }
        }
    }
    return best;
}

// ── Audit log (commit 12/24) ──────────────────────────────────────────

void AuthService::emitAudit(std::string_view event,
                            std::optional<std::int64_t> user_id,
                            std::string_view username,
                            std::string_view ip,
                            std::string_view details_json) {
    AuditEvent e;
    e.ts           = nowUnixSec();
    e.event        = std::string(event);
    e.user_id      = user_id;
    e.username     = std::string(username);
    e.ip           = std::string(ip);
    e.details_json = std::string(details_json);
    if (!db_.insertAuditEvent(e)) {
        LOG_WARN("AuthService: failed to write audit event '{}'", e.event);
    }
}

int AuthService::purgeAuditOlderThan(std::int64_t cutoff_ts) {
    const int n = db_.purgeAuditOlderThan(cutoff_ts);
    if (n > 0) {
        LOG_INFO("AuthService::purgeAuditOlderThan removed {} entries (cutoff_ts={})",
                 n, cutoff_ts);
    }
    return n;
}

int AuthService::purgeAuditOlderThanDays(int days) {
    if (days <= 0) return 0;
    const auto cutoff = nowUnixSec() -
        static_cast<std::int64_t>(days) * 86400;
    return purgeAuditOlderThan(cutoff);
}

std::vector<AuditEvent> AuthService::listAuditEvents(const AuditFilter& f) {
    return db_.listAuditEvents(f);
}

std::optional<JwtIssuer::TokenPair>
AuthService::issueAndPersist(const User& u,
                             std::string_view ip,
                             std::string_view ua,
                             std::optional<std::vector<ChannelGrant>>
                                 grants_override) {
    std::vector<ChannelGrant> grants;
    if (grants_override) {
        grants = std::move(*grants_override);
    } else {
        grants = grants_for_user_ ? grants_for_user_(u.id)
                                  : std::vector<ChannelGrant>{};
    }
    auto pair = jwt_.issue(u, grants);

    Session s;
    s.user_id            = u.id;
    s.jwt_id             = pair.jwt_id;
    s.refresh_token_hash = hashRefreshToken(pair.refresh_token);
    s.ip                 = std::string(ip);
    s.user_agent         = std::string(ua);
    s.created_at         = nowUnixSec();
    s.expires_at         = pair.refresh_expires_at;

    if (!db_.insertSession(s)) {
        LOG_ERROR("AuthService: insertSession failed for user_id={}", u.id);
        return std::nullopt;
    }
    return pair;
}

AuthService::LoginResult
AuthService::login(std::string_view username,
                   std::string_view password,
                   std::string_view ip,
                   std::string_view ua) {
    LoginResult lr;

    // c18/24: пустой пароль никогда не должен ходить ни в Argon2id, ни
    // в LDAP-bind (anonymous-bind RFC 4513 §5.1.2). Сразу 401.
    if (password.empty()) {
        LOG_WARN("AuthService::login: empty password rejected for '{}' from {}",
                 std::string(username), std::string(ip));
        emitAudit("login.fail", std::nullopt, username, ip,
                  R"({"reason":"empty_password"})");
        lr.outcome = LoginError::InvalidPassword;
        return lr;
    }

    auto user = db_.findUserByUsername(username);

    // c18/24: маршрутизация Local vs LDAP.
    //   - user в БД с source=Local           → текущий локальный flow
    //   - user в БД с source=Ldap            → ОБЯЗАТЕЛЬНО через LDAP
    //   - user не найден + LDAP enabled      → LDAP с автопровиженингом
    //   - user не найден + LDAP отсутствует  → UserNotFound
    const bool ldap_available = static_cast<bool>(ldap_authenticator_);
    const bool route_ldap = ldap_available &&
        ((user && user->source == Source::Ldap) ||
         (!user));

    if (route_ldap) {
        // Local юзер с тем же именем сюда не попадает — он остаётся
        // на локальной ветке. Disabled-проверка для LDAP-юзера в БД —
        // ниже после успешного bind'а.
        if (user && user->disabled) {
            LOG_WARN("AuthService::login: disabled LDAP user '{}' from {}",
                     user->username, std::string(ip));
            emitAudit("login.fail", user->id, user->username, ip,
                      R"({"reason":"user_disabled"})");
            lr.outcome = LoginError::UserDisabled;
            return lr;
        }
        return loginLdap_(username, password, ip, ua, user);
    }

    if (!user) {
        LOG_WARN("AuthService::login: unknown username '{}' from {}",
                 std::string(username), std::string(ip));
        emitAudit("login.fail", std::nullopt, username, ip,
                  R"({"reason":"user_not_found"})");
        lr.outcome = LoginError::UserNotFound;
        return lr;
    }
    if (user->disabled) {
        LOG_WARN("AuthService::login: disabled user '{}' from {}",
                 user->username, std::string(ip));
        emitAudit("login.fail", user->id, user->username, ip,
                  R"({"reason":"user_disabled"})");
        lr.outcome = LoginError::UserDisabled;
        return lr;
    }
    if (user->source != Source::Local) {
        // LDAP-юзер, но authenticator не выставлен — конфигурация
        // сервера сломана: оставлять lockout-поверхность Local'у мы не
        // можем. Возвращаем InternalError, чтобы оператор увидел причину.
        LOG_ERROR("AuthService::login: LDAP user '{}' but no authenticator wired",
                  user->username);
        emitAudit("login.fail", user->id, user->username, ip,
                  R"({"reason":"ldap_not_wired"})");
        lr.outcome = LoginError::InternalError;
        return lr;
    }
    // c13/24: brute-force lockout. Проверка ДО password verify — учётка
    // в локапе не должна "отвечать" даже валидным паролем (иначе атакующий
    // продолжает подбирать таймингом). После истечения lock'а флаг сам
    // не сбросится (это сделает успешный логин или admin-unlock); просто
    // locked_until уйдёт в прошлое и условие ниже даст false.
    {
        const auto now = nowUnixSec();
        if (user->locked_until.has_value() && *user->locked_until > now) {
            LOG_WARN("AuthService::login: account '{}' locked until {} (failed_count={})",
                     user->username, *user->locked_until, user->failed_login_count);
            std::string details =
                R"({"reason":"account_locked","locked_until":)" +
                std::to_string(*user->locked_until) +
                R"(,"failed_count":)" + std::to_string(user->failed_login_count) + "}";
            emitAudit("login.locked", user->id, user->username, ip, details);
            lr.outcome = LoginError::AccountLocked;
            return lr;
        }
    }

    if (!PasswordHasher::verify(password, user->password_hash)) {
        // c13/24: инкремент счётчика + расчёт нового locked_until по
        // экспоненциальной политике. Локап применяется ровно один раз —
        // при достижении threshold; дальнейшие провалы удваивают окно.
        const auto now = nowUnixSec();
        const int new_count = user->failed_login_count + 1;
        std::optional<std::int64_t> new_lock;
        if (new_count >= policy_.threshold) {
            int over = new_count - policy_.threshold;
            if (over < 0)  over = 0;
            if (over > 20) over = 20;  // защита от переполнения сдвига
            std::int64_t delay = static_cast<std::int64_t>(policy_.base_delay_sec) << over;
            if (delay > policy_.max_lock_sec) delay = policy_.max_lock_sec;
            new_lock = now + delay;
        }
        db_.recordFailedLogin(user->id, new_count, new_lock);

        LOG_WARN("AuthService::login: bad password for '{}' from {} (failed_count={}{})",
                 user->username, std::string(ip), new_count,
                 new_lock ? std::string(", locked")
                          : std::string());
        std::string details =
            R"({"reason":"invalid_password","failed_count":)" +
            std::to_string(new_count) + "}";
        emitAudit("login.fail", user->id, user->username, ip, details);
        if (new_lock) {
            std::string lock_details =
                R"({"locked_until":)" + std::to_string(*new_lock) +
                R"(,"failed_count":)" + std::to_string(new_count) + "}";
            emitAudit("login.locked", user->id, user->username, ip, lock_details);
        }
        lr.outcome = LoginError::InvalidPassword;
        return lr;
    }

    // Пароль корректен — счётчик неудач сбрасываем НЕЗАВИСИМО от того,
    // что произойдёт дальше (post-checks могут отклонить вход, но факт
    // знания пароля доказан, и удерживать lockout уже нет оснований).
    if (user->failed_login_count != 0 || user->locked_until.has_value()) {
        db_.clearFailedLogin(user->id);
    }

    // fix22 c11: bootstrap/admin-reset пароли живут не больше TTL.
    // Проверяем ПОСЛЕ password verification — иначе мы выдаём info о
    // существовании юзера атакующему без пароля.
    if (user->must_change_password &&
        user->initial_password_expires_at.has_value() &&
        *user->initial_password_expires_at < nowUnixSec()) {
        LOG_WARN("AuthService::login: initial password expired for '{}' "
                 "(expired_at={}, now={})",
                 user->username, *user->initial_password_expires_at,
                 nowUnixSec());
        emitAudit("login.fail", user->id, user->username, ip,
                  R"({"reason":"initial_password_expired"})");
        lr.outcome = LoginError::InitialPasswordExpired;
        return lr;
    }

    auto pair = issueAndPersist(*user, ip, ua);
    if (!pair) {
        lr.outcome = LoginError::InternalError;
        return lr;
    }

    db_.setLastLogin(user->id, nowUnixSec(), ip);

    LOG_INFO("AuthService::login OK user='{}' id={} jti={}",
             user->username, user->id, pair->jwt_id);
    emitAudit("login.ok", user->id, user->username, ip,
              R"({"jti":")" + pair->jwt_id + R"("})");
    lr.outcome = *pair;
    lr.user    = user;
    return lr;
}

// ─── LDAP login flow (commit 18/24) ───────────────────────────────────
//
// Brute-force lockout (c13/24) для LDAP не активируем: у директории
// своя политика блокировок, и наш счётчик стал бы DoS-вектором (атакующий
// может заблокировать любую LDAP-учётку, спамя нашему API). Аудит-запись
// мы пишем — оператор видит fail в SIEM.
AuthService::LoginResult
AuthService::loginLdap_(std::string_view username,
                        std::string_view password,
                        std::string_view ip,
                        std::string_view ua,
                        const std::optional<User>& existing_user) {
    LoginResult lr;
    if (!ldap_authenticator_) {
        // Не должно случиться (router проверил), но defense-in-depth.
        lr.outcome = LoginError::InternalError;
        return lr;
    }

    auto ctx = ldap_authenticator_(username, password);

    if (ctx.reason == LdapReason::Disabled) {
        // LDAP отключился между routing'ом и authenticator'ом (admin
        // успел PUT'нуть config). Для существующего LDAP-юзера это
        // misconfig, для unknown — UserNotFound.
        if (existing_user) {
            LOG_ERROR("AuthService::loginLdap: LDAP disabled but user '{}' "
                      "is source=ldap", existing_user->username);
            emitAudit("login.fail", existing_user->id, existing_user->username,
                      ip, R"({"reason":"ldap_disabled_but_user_is_ldap"})");
            lr.outcome = LoginError::InternalError;
            return lr;
        }
        emitAudit("login.fail", std::nullopt, username, ip,
                  R"({"reason":"user_not_found"})");
        lr.outcome = LoginError::UserNotFound;
        return lr;
    }

    if (!ctx.ok) {
        // Маппинг reason → LoginError. Сетевые/конфиг-ошибки оператор
        // видит как InternalError; неверный логин/пароль/отсутствие
        // в каталоге — InvalidPassword (ровный отлуп без leak'а).
        std::optional<std::int64_t> uid;
        std::string uname(username);
        if (existing_user) { uid = existing_user->id; uname = existing_user->username; }

        std::string reason_str;
        LoginError out_err = LoginError::InvalidPassword;
        switch (ctx.reason) {
            case LdapReason::ConnectionFailed:
                reason_str = "ldap_connection_failed";
                out_err    = LoginError::InternalError;
                break;
            case LdapReason::BindServiceFailed:
                reason_str = "ldap_service_bind_failed";
                out_err    = LoginError::InternalError;
                break;
            case LdapReason::ConfigError:
                reason_str = "ldap_config_error";
                out_err    = LoginError::InternalError;
                break;
            case LdapReason::UserNotFound:
                reason_str = "ldap_user_not_found";
                out_err    = LoginError::InvalidPassword;
                break;
            case LdapReason::InvalidCredentials:
                reason_str = "ldap_invalid_credentials";
                out_err    = LoginError::InvalidPassword;
                break;
            case LdapReason::NoRoleMapped:
                // Юзер залогинился в LDAP, но ни одна его группа не
                // выдала роль в нашем приложении — отдельный код,
                // снаружи 403 user_disabled.
                reason_str = "ldap_no_role_mapped";
                out_err    = LoginError::UserDisabled;
                break;
            default:
                reason_str = "ldap_other";
                out_err    = LoginError::InvalidPassword;
                break;
        }

        LOG_WARN("AuthService::loginLdap: fail '{}' reason={} ({})",
                 std::string(username), reason_str, ctx.error);
        std::string details =
            R"({"reason":")" + reason_str + R"("})";
        emitAudit("login.fail", uid, uname, ip, details);
        lr.outcome = out_err;
        return lr;
    }

    // Bind ok + role замаплен. Провижн или обновление учётной записи.
    std::optional<User> user = existing_user;
    if (!user) {
        // Авто-провиженинг: создаём local-row для LDAP-юзера. password_hash
        // оставляем пустым — local password verify ему недоступен по
        // определению (source=Ldap отбит в ветке выше).
        User u;
        u.username             = std::string(username);
        u.email                = ctx.email;
        u.role                 = ctx.role;
        u.source               = Source::Ldap;
        u.must_change_password = false;
        u.created_at           = nowUnixSec();
        auto id = db_.insertUser(u);
        if (!id) {
            // Гонка: кто-то успел создать юзера. Перечитаем из БД и
            // продолжим — это safe.
            user = db_.findUserByUsername(username);
            if (!user) {
                LOG_ERROR("AuthService::loginLdap: insertUser failed for '{}'",
                          std::string(username));
                emitAudit("login.fail", std::nullopt, username, ip,
                          R"({"reason":"ldap_provision_failed"})");
                lr.outcome = LoginError::InternalError;
                return lr;
            }
        } else {
            user = db_.findUserById(*id);
            if (!user) {
                LOG_ERROR("AuthService::loginLdap: post-insert lookup failed id={}",
                          *id);
                lr.outcome = LoginError::InternalError;
                return lr;
            }
            LOG_INFO("AuthService::loginLdap: provisioned LDAP user '{}' id={} role={}",
                     user->username, user->id, roleName(user->role));
            emitAudit("ldap.provisioned", user->id, user->username, ip,
                      R"({"role":")" + std::string(roleName(user->role)) + R"("})");
        }
    }

    // Если данные изменились — пишем в БД (email/role могут поменяться
    // в каталоге между логинами).
    if (user->email != ctx.email || user->role != ctx.role) {
        if (!db_.updateUser(user->id, ctx.email, ctx.role, Source::Ldap)) {
            LOG_ERROR("AuthService::loginLdap: updateUser failed id={}", user->id);
            // Не валим логин — продолжаем с тем, что есть в БД.
        } else {
            user->email = ctx.email;
            user->role  = ctx.role;
            user->source = Source::Ldap;
        }
    }

    if (user->disabled) {
        LOG_WARN("AuthService::loginLdap: disabled LDAP user '{}'", user->username);
        emitAudit("login.fail", user->id, user->username, ip,
                  R"({"reason":"user_disabled"})");
        lr.outcome = LoginError::UserDisabled;
        return lr;
    }

    // commit 20/24: кэшируем группы юзера в БД, чтобы refresh смог
    // пересчитать grants без обращения к директории. Сериализуем как
    // JSON-массив строк — компактно и легко парсится. Сбой записи
    // кэша не валит логин (пара уже выдана); refresh потом отдаст
    // LdapCacheExpired и юзер перелогинится — это OK.
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& g : ctx.groups) arr.push_back(g);
        const auto serialized = arr.dump();
        if (!db_.updateLdapGroupsCache(user->id, serialized, nowUnixSec())) {
            LOG_WARN("AuthService::loginLdap: updateLdapGroupsCache failed id={}",
                     user->id);
        }
    }

    // commit 19/24: per-channel grants — пробрасываем напрямую в
    // issueAndPersist, чтобы JWT для LDAP-юзера получил channel_grants
    // из cfg.channel_acl (а не пустой вектор от local-resolver'а).
    auto pair = issueAndPersist(*user, ip, ua,
                                std::optional<std::vector<ChannelGrant>>{
                                    ctx.channel_grants});
    if (!pair) {
        lr.outcome = LoginError::InternalError;
        return lr;
    }
    db_.setLastLogin(user->id, nowUnixSec(), ip);

    LOG_INFO("AuthService::loginLdap OK user='{}' id={} role={} grants={} jti={}",
             user->username, user->id, roleName(user->role),
             ctx.channel_grants.size(), pair->jwt_id);
    emitAudit("login.ok", user->id, user->username, ip,
              R"({"source":"ldap","grants":)" +
              std::to_string(ctx.channel_grants.size()) +
              R"(,"jti":")" + pair->jwt_id + R"("})");
    lr.outcome = *pair;
    lr.user    = user;
    return lr;
}

AuthService::RefreshResult
AuthService::refresh(std::string_view refresh_token,
                     std::string_view ip,
                     std::string_view ua) {
    RefreshResult rr;
    if (refresh_token.empty()) {
        rr.outcome = RefreshError::TokenNotFound;
        return rr;
    }

    const auto hash = hashRefreshToken(refresh_token);
    auto sess = db_.findSessionByRefreshTokenHash(hash);
    if (!sess) {
        LOG_WARN("AuthService::refresh: unknown refresh from {}", std::string(ip));
        rr.outcome = RefreshError::TokenNotFound;
        return rr;
    }
    if (sess->revoked_at.has_value()) {
        // Replay! легитимный клиент успел сделать refresh, а кто-то
        // переиспользует старый. Логируем громко.
        LOG_WARN("AuthService::refresh: REPLAY detected for jti={} user_id={} from {}",
                 sess->jwt_id, sess->user_id, std::string(ip));
        emitAudit("refresh.replay", sess->user_id, "", ip,
                  R"({"jti":")" + sess->jwt_id + R"("})");
        rr.outcome = RefreshError::TokenRevoked;
        return rr;
    }
    if (sess->expires_at <= nowUnixSec()) {
        emitAudit("refresh.fail", sess->user_id, "", ip,
                  R"({"reason":"expired"})");
        rr.outcome = RefreshError::TokenExpired;
        return rr;
    }

    auto user = db_.findUserById(sess->user_id);
    if (!user) {
        LOG_ERROR("AuthService::refresh: session points to missing user_id={}",
                  sess->user_id);
        rr.outcome = RefreshError::InternalError;
        return rr;
    }
    if (user->disabled) {
        emitAudit("refresh.fail", user->id, user->username, ip,
                  R"({"reason":"user_disabled"})");
        rr.outcome = RefreshError::UserDisabled;
        return rr;
    }

    // commit 20/24: для LDAP-юзера refresh пересчитывает grants по
    // кэшированным группам — без обращения к директории. Это даёт
    // read-only доступ во время downtime'а LDAP, не ломая refresh.
    // Окно ограничено LdapCachePolicy.max_age_sec; за окном — клиент
    // получает LdapCacheExpired и обязан перелогиниться (живой bind).
    std::optional<std::vector<ChannelGrant>> grants_override;
    if (user->source == Source::Ldap) {
        const auto now = nowUnixSec();
        if (!user->ldap_groups_cached_at.has_value()) {
            // legacy LDAP-юзер без кэша (логинился до schema v3) —
            // принудительный re-login.
            LOG_WARN("AuthService::refresh: LDAP user '{}' has no cached groups",
                     user->username);
            emitAudit("refresh.fail", user->id, user->username, ip,
                      R"({"reason":"ldap_cache_missing"})");
            rr.outcome = RefreshError::LdapCacheExpired;
            return rr;
        }
        const auto age = now - *user->ldap_groups_cached_at;
        if (age > ldap_cache_policy_.max_age_sec) {
            LOG_WARN("AuthService::refresh: LDAP cache expired for '{}' "
                     "(age={}s, max={}s)",
                     user->username, age, ldap_cache_policy_.max_age_sec);
            std::string details =
                R"({"reason":"ldap_cache_expired","age_sec":)" +
                std::to_string(age) + "}";
            emitAudit("refresh.fail", user->id, user->username, ip, details);
            rr.outcome = RefreshError::LdapCacheExpired;
            return rr;
        }
        // Парсим кэшированные группы и пересчитываем grants. Если
        // resolver не выставлен — оставляем grants пустым (роль
        // user.role остаётся, per-channel доступ выключен).
        std::vector<std::string> cached_groups;
        try {
            auto j = nlohmann::json::parse(user->ldap_groups_json);
            if (j.is_array()) {
                for (const auto& v : j) {
                    if (v.is_string()) cached_groups.push_back(v.get<std::string>());
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("AuthService::refresh: cannot parse cached groups for '{}': {}",
                      user->username, ex.what());
            emitAudit("refresh.fail", user->id, user->username, ip,
                      R"({"reason":"ldap_cache_parse_failed"})");
            rr.outcome = RefreshError::InternalError;
            return rr;
        }
        std::vector<ChannelGrant> grants;
        if (ldap_grants_resolver_) grants = ldap_grants_resolver_(cached_groups);
        grants_override = std::move(grants);
    }

    // Rotation: ревок старой ДО issuance новой, чтобы partial failure
    // не оставил две действующие пары. Если insertSession ниже упадёт —
    // legitimate user перелогинится, это OK.
    db_.revokeSessionByJwtId(sess->jwt_id, nowUnixSec());

    auto pair = issueAndPersist(*user, ip, ua, std::move(grants_override));
    if (!pair) {
        rr.outcome = RefreshError::InternalError;
        return rr;
    }

    LOG_INFO("AuthService::refresh OK user='{}' id={} old_jti={} new_jti={}",
             user->username, user->id, sess->jwt_id, pair->jwt_id);
    emitAudit("refresh.ok", user->id, user->username, ip,
              R"({"old_jti":")" + sess->jwt_id +
              R"(","new_jti":")" + pair->jwt_id + R"("})");
    rr.outcome = *pair;
    rr.user    = user;
    return rr;
}

bool AuthService::logout(std::string_view jwt_id) {
    if (jwt_id.empty()) return false;
    // user_id для аудита берём из sessions ДО ревока — после revoke
    // запись остаётся, но логически это интересно для трассировки.
    auto sess = db_.findSessionByJwtId(jwt_id);
    const bool ok = db_.revokeSessionByJwtId(jwt_id, nowUnixSec());
    if (ok) {
        LOG_INFO("AuthService::logout jti={}", std::string(jwt_id));
        std::optional<std::int64_t> uid;
        if (sess) uid = sess->user_id;
        emitAudit("logout", uid, "", "",
                  R"({"jti":")" + std::string(jwt_id) + R"("})");
    }
    return ok;
}

// ─── Admin user CRUD (commit 7/24) ─────────────────────────────────────

namespace {

// Очень либеральная валидация — мы не пытаемся реализовывать полноценный
// e-mail spec; задача — отлуп явно мусорных значений.
bool emailLooksValid(std::string_view e) {
    if (e.empty()) return true;  // empty allowed (optional)
    if (e.size() > 254) return false;
    auto at = e.find('@');
    if (at == std::string_view::npos) return false;
    if (at == 0 || at == e.size() - 1) return false;
    if (e.find('@', at + 1) != std::string_view::npos) return false;
    return true;
}

bool usernameValid(std::string_view u) {
    if (u.empty() || u.size() > 64) return false;
    for (char c : u) {
        // Запрет whitespace + control chars + '/' + ':' (audit-friendly).
        if (c <= ' ' || c == '/' || c == ':' || c == 0x7F) return false;
    }
    return true;
}

}  // namespace

std::vector<User> AuthService::adminListUsers() {
    return db_.listUsers();
}

std::optional<User> AuthService::adminGetUser(std::int64_t id) {
    return db_.findUserById(id);
}

std::variant<AuthService::CreatedUser, AuthService::AdminError>
AuthService::adminCreateUser(const CreateUserRequest& req) {
    if (!usernameValid(req.username)) return AdminError::UsernameInvalid;
    if (!emailLooksValid(req.email))  return AdminError::EmailInvalid;

    // Если пароль пуст — генерируем случайный, чтобы admin не подсунул
    // accidentally empty.
    std::string plaintext = req.password.empty()
        ? PasswordHasher::generateRandom()
        : req.password;
    if (plaintext.empty()) return AdminError::PasswordInvalid;
    auto hash = PasswordHasher::hash(plaintext);
    if (!hash) return AdminError::PasswordInvalid;

    User u;
    u.username             = req.username;
    u.email                = req.email;
    u.password_hash        = *hash;
    u.role                 = req.role;
    u.source               = Source::Local;
    u.must_change_password = req.must_change_password;
    u.created_at           = nowUnixSec();
    u.created_by           = req.created_by;
    // c11/24: must_change=true означает «временный пароль» — заводим TTL.
    // Если admin честно создал юзера и сразу выдал ему рабочий пароль
    // (must_change=false) — никакого окна не нужно, пароль постоянный.
    if (req.must_change_password) {
        u.initial_password_expires_at = u.created_at + kInitialPasswordTtlSec;
    }

    auto id = db_.insertUser(u);
    if (!id) {
        // Самая частая причина — UNIQUE-violation на username. AuthDb
        // уже залогировал DEBUG; мы превращаем в UsernameTaken.
        if (db_.findUserByUsername(req.username)) return AdminError::UsernameTaken;
        return AdminError::InternalError;
    }
    auto inserted = db_.findUserById(*id);
    if (!inserted) return AdminError::InternalError;

    LOG_INFO("AuthService::adminCreateUser '{}' id={} role={}",
             inserted->username, inserted->id, roleName(inserted->role));

    CreatedUser out;
    out.user = *inserted;
    // Plaintext возвращаем только если админ его НЕ указал — тогда мы
    // его сгенерили и больше нигде не сохранили. Если admin указал свой
    // — он его и так знает, plaintext не возвращаем (минимум следов).
    if (req.password.empty()) out.plaintext_password = plaintext;
    return out;
}

std::variant<User, AuthService::AdminError>
AuthService::adminUpdateUser(std::int64_t id, const UpdateUserRequest& req) {
    auto existing = db_.findUserById(id);
    if (!existing) return AdminError::UserNotFound;
    if (!emailLooksValid(req.email)) return AdminError::EmailInvalid;

    if (!db_.updateUser(id, req.email, req.role, req.source)) {
        return AdminError::InternalError;
    }
    auto updated = db_.findUserById(id);
    if (!updated) return AdminError::InternalError;
    LOG_INFO("AuthService::adminUpdateUser id={} role={} source={}",
             id, roleName(updated->role), sourceName(updated->source));
    return *updated;
}

bool AuthService::adminSetDisabled(std::int64_t id, bool disabled) {
    if (!db_.setDisabled(id, disabled)) return false;
    if (disabled) {
        // Disabling — выкидываем активные сессии, чтобы текущий JWT не
        // дотянул до конца своего часа.
        const int n = db_.revokeAllSessionsForUser(id, nowUnixSec());
        LOG_INFO("AuthService::adminSetDisabled id={} disabled={} revoked_sessions={}",
                 id, disabled, n);
    } else {
        LOG_INFO("AuthService::adminSetDisabled id={} disabled=false", id);
    }
    return true;
}

std::variant<AuthService::PurgedUser, AuthService::PurgeError>
AuthService::adminPurgeUser(std::int64_t id, std::int64_t actor_id) {
    if (id == actor_id) return PurgeError::CannotDeleteSelf;

    auto target = db_.findUserById(id);
    if (!target) return PurgeError::UserNotFound;

    // Last-enabled-admin guard. Snapshot before delete: если удаляемый —
    // единственный enabled admin, отказываем. Disabled-admin'ы не считаются
    // (сами по себе уже не дают доступ). Юзер сам может быть disabled —
    // тогда он не учитывается в countEnabledAdmins, но это не страшно:
    // если всё ещё хотя бы один enabled admin есть — удаляем.
    if (target->role == Role::Admin && !target->disabled) {
        const int enabled_admins = db_.countEnabledAdmins();
        if (enabled_admins <= 1) return PurgeError::LastEnabledAdmin;
    }

    if (!db_.purgeUser(id)) return PurgeError::InternalError;

    LOG_INFO("AuthService::adminPurgeUser id={} username='{}' role={} by_actor={}",
             id, target->username, roleName(target->role), actor_id);

    PurgedUser out;
    out.id       = target->id;
    out.username = target->username;
    out.email    = target->email;
    out.role     = target->role;
    return out;
}

std::optional<std::string> AuthService::adminResetPassword(std::int64_t id) {
    auto user = db_.findUserById(id);
    if (!user) return std::nullopt;

    std::string plaintext = PasswordHasher::generateRandom();
    if (plaintext.empty()) return std::nullopt;
    auto hash = PasswordHasher::hash(plaintext);
    if (!hash) return std::nullopt;

    const auto now = nowUnixSec();
    if (!db_.updatePasswordHash(id, *hash, now,
                                /*clear_must_change=*/false)) {
        return std::nullopt;
    }
    if (!db_.setMustChangePassword(id, true)) return std::nullopt;
    // c11/24: новое 24-часовое окно. Без него admin'у пришлось бы
    // вручную чистить старые expires_at из БД, иначе юзер с уже
    // протухшим reset не смог бы перелогиниться даже после нового сброса.
    if (!db_.setInitialPasswordExpiry(id, now + kInitialPasswordTtlSec)) {
        return std::nullopt;
    }

    // Все активные сессии — out. Юзер обязан перелогиниться и сразу
    // сменить пароль (RBAC заблокирует всё кроме self-password change).
    const int revoked = db_.revokeAllSessionsForUser(id, now);
    LOG_INFO("AuthService::adminResetPassword id={} revoked_sessions={}",
             id, revoked);
    return plaintext;
}

// ─── Brute-force lockout admin-unlock (commit 13/24) ──────────────────

bool AuthService::adminUnlockUser(std::int64_t id) {
    auto u = db_.findUserById(id);
    if (!u) return false;
    // Идемпотентно: clearFailedLogin без условия — на чистом юзере sqlite
    // просто перепишет нули.
    if (!db_.clearFailedLogin(id)) {
        LOG_ERROR("AuthService::adminUnlockUser: db.clearFailedLogin failed id={}", id);
        return false;
    }
    LOG_INFO("AuthService::adminUnlockUser id={} username='{}'", id, u->username);
    return true;
}

// ─── Bootstrap initial admin (commit 9/24) ────────────────────────────

AuthService::BootstrapResult
AuthService::bootstrapInitialAdmin(std::string_view username) {
    BootstrapResult out;
    if (db_.hasAdminUser()) {
        // Уже есть ≥1 не-disabled admin — bootstrap не нужен.
        return out;
    }

    // Generate password ОДИН раз; тот же plaintext попадает в hash + в
    // BootstrapResult. Никуда больше — никто не должен материализовать
    // этот пароль повторно.
    std::string plaintext = PasswordHasher::generateRandom();
    if (plaintext.empty()) {
        LOG_ERROR("AuthService::bootstrapInitialAdmin generateRandom failed");
        return out;
    }
    auto hash = PasswordHasher::hash(plaintext);
    if (!hash) {
        LOG_ERROR("AuthService::bootstrapInitialAdmin hash failed");
        return out;
    }

    User u;
    u.username             = std::string(username);
    u.password_hash        = *hash;
    u.role                 = Role::Admin;
    u.source               = Source::Local;
    u.must_change_password = true;
    u.created_at           = nowUnixSec();
    u.initial_password_expires_at = u.created_at + kInitialPasswordTtlSec;

    auto id = db_.insertUser(u);
    if (!id) {
        // Возможно, конкурентный startup успел создать. Лог + выход.
        LOG_ERROR("AuthService::bootstrapInitialAdmin insertUser failed for '{}'",
                  u.username);
        return out;
    }
    LOG_WARN("AuthService::bootstrapInitialAdmin created admin '{}' id={} "
             "(must_change_password=true).",
             u.username, *id);

    out.created            = true;
    out.username           = u.username;
    out.plaintext_password = plaintext;  // copy — file write may need it too
    out.user_id            = *id;

    // fix35 (A3.16) — when a state-dir was wired in via
    // setInitialAdminPasswordFile(), drop the plaintext into that file
    // (mode 0600) so the operator can recover it without scraping logs.
    // The file is auto-deleted on first successful self-change of the
    // password (changeOwnPassword), or by `--reset-admin-password` if
    // the operator removes it by hand and forgets the value.
    if (!initial_admin_password_file_.empty()) {
        if (writeInitialAdminPasswordFile(initial_admin_password_file_, plaintext)) {
            LOG_WARN("Bootstrap admin password written to {} (mode 0600). "
                     "It is auto-removed on first password change.",
                     initial_admin_password_file_.string());
            out.password_file_path = initial_admin_password_file_;
        } else {
            LOG_ERROR("Bootstrap admin password file write failed; "
                      "BootstrapResult.plaintext_password is the only copy.");
        }
    }

    sodium_memzero(plaintext.data(), plaintext.size());
    emitAudit("bootstrap.created", out.user_id, out.username, "", "");
    return out;
}

// ─── Self-change password (commit 8/24) ───────────────────────────────

std::optional<AuthService::SelfPasswordError>
AuthService::changeOwnPassword(std::string_view access_token,
                               std::string_view current_password,
                               std::string_view new_password) {
    // Проверка bearer'а — но в обход must_change_password gate, который
    // RBAC обычно проверяет: этот flow специально для смены пароля под
    // must_change. Поэтому используем jwt_.verify + db_.findSession (не
    // verifyActiveAccess, который ничего лишнего не проверяет, но мы
    // хотим контроль над тем как именно отвергаем).
    auto claims = jwt_.verify(access_token);
    if (!claims) return SelfPasswordError::InvalidSession;

    auto sess = db_.findSessionByJwtId(claims->jti);
    if (!sess || sess->revoked_at.has_value()) {
        return SelfPasswordError::InvalidSession;
    }

    auto user = db_.findUserById(claims->user_id);
    if (!user) return SelfPasswordError::UserNotFound;
    if (user->disabled) return SelfPasswordError::UserDisabled;

    // LDAP-юзер не имеет local password_hash → менять нечего; их пароль
    // живёт в LDAP. RBAC должен это маркировать source-aware'но; здесь
    // мы просто откажемся.
    if (user->source != Source::Local) {
        return SelfPasswordError::UserDisabled;  // best fit без новой кодовой
    }

    // Минимум 8 символов — единственный sanity-check. Полноценный
    // password-policy появится в commit 13/24 (lockout) если потребуется.
    constexpr std::size_t kMinLen = 8;
    if (new_password.size() < kMinLen) return SelfPasswordError::NewPasswordWeak;
    if (new_password == current_password) return SelfPasswordError::NewPasswordWeak;

    // Verify current password — не пускаем сменить пароль владельцу
    // украденного access-token'а без знания текущего пароля.
    if (!PasswordHasher::verify(current_password, user->password_hash)) {
        LOG_WARN("AuthService::changeOwnPassword wrong current password for user_id={}",
                 user->id);
        return SelfPasswordError::CurrentPasswordWrong;
    }

    auto new_hash = PasswordHasher::hash(new_password);
    if (!new_hash) return SelfPasswordError::InternalError;

    // fix35 (A3.16) — snapshot the bootstrap-admin marker BEFORE we
    // flip must_change_password to false in the DB. The auto-delete
    // below depends on (was_admin && previous_must_change), and we
    // can't read either of those off the User after the update.
    const bool was_admin_with_must_change =
        (user->role == Role::Admin) && user->must_change_password;

    // updatePasswordHash с clear_must_change=true — самостоятельная
    // смена пароля закрывает gate автоматически.
    if (!db_.updatePasswordHash(user->id, *new_hash, nowUnixSec(),
                                /*clear_must_change=*/true)) {
        return SelfPasswordError::InternalError;
    }

    // fix35 (A3.16) — auto-delete state/initial_admin_password.txt
    // after the bootstrap admin's first password change. Best-effort:
    // if removal fails (FS readonly, file already gone) the password
    // change is still successful from the user's POV. We rely on
    // ENOENT being a non-error in std::filesystem::remove (returns
    // false + clean ec).
    if (was_admin_with_must_change && !initial_admin_password_file_.empty()) {
        std::error_code ec;
        const bool removed =
            std::filesystem::remove(initial_admin_password_file_, ec);
        if (ec) {
            LOG_WARN("Failed to remove {}: {}",
                     initial_admin_password_file_.string(), ec.message());
        } else if (removed) {
            LOG_INFO("{} removed after first password change "
                     "(user_id={})",
                     initial_admin_password_file_.string(), user->id);
        }
        // removed==false && !ec → file was already gone (operator
        // wiped it manually). Silent: not worth a log line.
    }

    // Revoke other sessions — оставляем текущий jti в живых, чтобы юзер
    // не получил "logged out" сразу после смены пароля на этом же
    // девайсе. Все ОСТАЛЬНЫЕ сессии (другой браузер, мобильный) — out:
    // если злоумышленник где-то залогинился с украденным паролем,
    // настоящий владелец лишает его доступа сменой пароля.
    const int revoked = db_.revokeOtherSessionsForUser(
        user->id, claims->jti, nowUnixSec());
    LOG_INFO("AuthService::changeOwnPassword user_id={} revoked_other_sessions={}",
             user->id, revoked);
    emitAudit("password.changed", user->id, user->username, "",
              R"({"revoked_other":)" + std::to_string(revoked) + "}");
    return std::nullopt;
}

// ─── Per-channel ACL for local users (commit 22/24) ──────────────────

std::variant<std::vector<AuthService::ChannelPermissionRow>,
             AuthService::ChannelAclError>
AuthService::adminListChannelPermissionsForUser(std::int64_t user_id) {
    auto u = db_.findUserById(user_id);
    if (!u) return ChannelAclError::UserNotFound;
    return db_.listChannelPermissionsForUser(user_id);
}

std::variant<std::vector<AuthService::ChannelPermissionRow>,
             AuthService::ChannelAclError>
AuthService::adminListChannelPermissionsForChannel(std::int64_t channel_id) {
    return db_.listChannelPermissionsForChannel(channel_id);
}

std::variant<bool, AuthService::ChannelAclError>
AuthService::adminSetChannelPermission(std::int64_t user_id,
                                       std::int64_t channel_id,
                                       ChannelPermission permission,
                                       std::int64_t granted_by) {
    auto u = db_.findUserById(user_id);
    if (!u)                       return ChannelAclError::UserNotFound;
    if (u->source != Source::Local)
        return ChannelAclError::UserSourceMismatch;
    if (!db_.setChannelPermission(user_id, channel_id, permission,
                                  granted_by, nowUnixSec())) {
        return ChannelAclError::InternalError;
    }
    return true;
}

std::variant<bool, AuthService::ChannelAclError>
AuthService::adminRemoveChannelPermission(std::int64_t user_id,
                                          std::int64_t channel_id) {
    auto u = db_.findUserById(user_id);
    if (!u)                       return ChannelAclError::UserNotFound;
    if (u->source != Source::Local)
        return ChannelAclError::UserSourceMismatch;
    return db_.removeChannelPermission(user_id, channel_id);
}

// ── fix32 B2: own-sessions API ────────────────────────────────────────

std::vector<Session>
AuthService::listOwnActiveSessions(std::int64_t user_id) {
    return db_.listActiveSessionsForUser(user_id, nowUnixSec());
}

bool AuthService::revokeOwnSession(std::string_view jwt_id,
                                   std::int64_t user_id) {
    const auto when = nowUnixSec();
    const bool ok = db_.revokeSessionByJwtIdForUser(jwt_id, user_id, when);
    if (ok) {
        LOG_INFO("AuthService::revokeOwnSession user={} jti={}",
                 user_id, std::string(jwt_id));
        emitAudit("session_revoked",
                  user_id, /*username=*/"", /*ip=*/"",
                  R"({"jti":")" + std::string(jwt_id) +
                  R"(","by":"self"})");
    }
    return ok;
}

std::optional<JwtIssuer::Claims>
AuthService::verifyActiveAccess(std::string_view access_token) {
    auto claims = jwt_.verify(access_token);
    if (!claims) return std::nullopt;

    auto sess = db_.findSessionByJwtId(claims->jti);
    if (!sess) return std::nullopt;       // sigatured, но в БД нет
    if (sess->revoked_at.has_value()) return std::nullopt;

    // fix32 B2: для UI Profile/Sessions page. SQL throttle 60s — сама
    // запись выполняется только если предыдущий timestamp старше или
    // NULL. Cost: один UPDATE по UNIQUE jwt_id, дёшево.
    db_.touchSession(claims->jti, nowUnixSec(), /*throttle_sec=*/60);
    return claims;
}

}  // namespace liveqx::auth
