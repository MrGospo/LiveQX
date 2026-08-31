#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "auth/AuthDb.h"
#include "auth/AuthTypes.h"
#include "auth/JwtIssuer.h"

namespace liveqx::events { class EventBus; }
namespace liveqx::audit  { class AuditLogger; }

namespace liveqx::auth {

// fix22 commit 5/24 — оркестратор login/logout/refresh.
//
// Поверхность:
//   login(username, password, ip, ua) — проверка argon2id, rotation
//                                       сессии в БД, выдача (access,
//                                       refresh) пары.
//   refresh(refresh_token, ip, ua)   — поиск по hash, проверка срока,
//                                       ревок старой, выдача новой пары
//                                       (rotation refresh-токена).
//   logout(jwt_id)                   — пометить сессию revoked_at;
//                                       идемпотентно.
//
// Refresh-токен в БД хранится не в plaintext, а в SHA-256-hex hash'е.
// Argon2id здесь оверкилл: на входе уже 32 random bytes, brute-force
// невозможен; SHA-256 даёт O(1) lookup. Атакующий, читающий БД,
// не может предъявить refresh без plaintext'а.
//
// `getGrantsForUser` коллбек заполняет channel_grants. На commit 5
// возвращаем пустой vector; commit 22/24 проводит локальную ACL.
class AuthService {
public:
    enum class LoginError {
        UserNotFound,            // 401 (но снаружи маскируем под invalid creds)
        InvalidPassword,         // 401
        UserDisabled,            // 403
        InitialPasswordExpired,  // 403 — admin раздал bootstrap/reset пароль,
                                 //       юзер не сменил его за 24h. Только
                                 //       admin-reset разблокирует логин.
        AccountLocked,           // 423 — слишком много неудач подряд,
                                 //       brute-force lockout активен. Снаружи
                                 //       видно: вернёт retry-after подсказку,
                                 //       пароль НЕ проверяется до истечения.
        InternalError,           // 500
    };

    enum class RefreshError {
        TokenNotFound,       // 401
        TokenRevoked,        // 401
        TokenExpired,        // 401
        UserDisabled,        // 403
        // commit 20/24 — LDAP-юзер не может обновить токен: кэш групп
        // протух (последний успешный LDAP login был >max_age_sec назад),
        // а директория сейчас могла быть нетронутой или лежать. В обоих
        // случаях клиенту нужно перелогиниться, чтобы пройти live bind.
        LdapCacheExpired,    // 401 — re-login required
        InternalError,       // 500
    };

    struct LoginResult {
        std::variant<JwtIssuer::TokenPair, LoginError> outcome;
        std::optional<User> user;  // заполнен только при Ok — для audit
    };

    struct RefreshResult {
        std::variant<JwtIssuer::TokenPair, RefreshError> outcome;
        std::optional<User> user;
    };

    using GrantsResolver = std::function<
        std::vector<ChannelGrant>(std::int64_t user_id)>;

    // ── LDAP login flow (commit 18/24) ─────────────────────────────────
    //
    // AuthService не линкуется с libldap — LdapClient инжектируется через
    // callback. В production lambda берёт LdapConfigRepo + LdapClient и
    // мапит группы→role; в тестах подсовываем stub-функцию, возвращающую
    // canned-результаты. Так login() становится проверяемой без живого DC.
    //
    // Если LDAP отключён или authenticator не выставлен — login()
    // работает в режиме «только Local», как до commit 18.
    enum class LdapReason {
        Disabled,           // LDAP не настроен — AuthService уйдёт в Local.
        Ok,                 // bind ok, user найден, role замаплен
        ConnectionFailed,   // сеть/TLS — operator увидит InternalError
        BindServiceFailed,  // service-account creds битые — InternalError
        UserNotFound,       // юзера нет в директории — снаружи InvalidPassword
        InvalidCredentials, // user-bind отклонён паролем
        ConfigError,        // мы конфигурили мусор — InternalError
        NoRoleMapped,       // bind ok, но ни одна группа не дала роли — отказ
        Other,
    };

    struct LdapLoginContext {
        bool                       ok{false};
        Role                       role{Role::Viewer};
        std::string                email;
        std::vector<std::string>   groups;
        // commit 19/24 — per-channel grants, рассчитанные production-
        // лямбдой из cfg.channel_acl + groups (с помощью
        // pickAclGrantsForGroups). Передаются «сырыми» в JWT, чтобы
        // RBAC мог проверять channel_id-permissions без второго запроса
        // в БД. Пустой вектор означает «нет per-channel доступа», что
        // нормально — роль из group_role_map по-прежнему действует.
        std::vector<ChannelGrant>  channel_grants;
        LdapReason                 reason{LdapReason::Disabled};
        std::string                error;
    };

    using LdapAuthenticator = std::function<
        LdapLoginContext(std::string_view username, std::string_view password)>;

    // commit 20/24 — резолвер для пересчёта grants на refresh'е по
    // кэшированным группам. В production ходит в LdapConfigRepo за
    // channel_acl и зовёт pickAclGrantsForGroups; тесты подставляют
    // лямбду-стаб. Если не выставлен — refresh для LDAP-юзера выдаёт
    // grants=[] (роль из user.role остаётся, per-channel доступа нет).
    using LdapGrantsResolver = std::function<
        std::vector<ChannelGrant>(const std::vector<std::string>&)>;

    // commit 20/24 — TTL кэша LDAP-групп. Когда LDAP лежит, refresh
    // пересчитывает grants по кэшу не дольше max_age_sec; за окном —
    // LdapCacheExpired (юзер должен перелогиниться). Дефолт 7 суток —
    // компромисс между «директория может лежать неделю» и «не
    // оставлять stale-permissions висеть месяцами».
    struct LdapCachePolicy {
        std::int64_t max_age_sec{7 * 24 * 3600};
    };

    AuthService(AuthDb& db, JwtIssuer& jwt, GrantsResolver grants_for_user);
    AuthService(AuthDb& db, JwtIssuer& jwt,
                GrantsResolver  grants_for_user,
                LdapAuthenticator ldap_authenticator);

    // Иммутабельный после ctor'а — но удобно дать возможность подсунуть
    // stub из тестов уже после конструирования (старый ctor оставляем
    // совместимым).
    void setLdapAuthenticator(LdapAuthenticator a) { ldap_authenticator_ = std::move(a); }

    void setLdapGrantsResolver(LdapGrantsResolver r) {
        ldap_grants_resolver_ = std::move(r);
    }
    LdapCachePolicy ldapCachePolicy() const noexcept { return ldap_cache_policy_; }
    void setLdapCachePolicy(LdapCachePolicy p) noexcept { ldap_cache_policy_ = p; }

    // fix35 (A3.16) — path of the bootstrap-admin password file. When
    // set, bootstrapInitialAdmin() writes the freshly-generated plaintext
    // password into this file (mode 0600), and changeOwnPassword()
    // removes it once the bootstrap admin successfully changes their
    // password for the first time. Empty path disables both — the
    // operator must capture the BootstrapResult.plaintext_password
    // themselves (used by unit tests that don't care about the file).
    void setInitialAdminPasswordFile(std::filesystem::path p) noexcept {
        initial_admin_password_file_ = std::move(p);
    }
    const std::filesystem::path& initialAdminPasswordFile() const noexcept {
        return initial_admin_password_file_;
    }

    // Public для тестов и LdapDirectoryService: case-insensitive lookup
    // group→role с приоритетом Admin > Operator > Viewer (если юзер
    // в нескольких matched-группах — берём максимальную привилегию).
    static std::optional<Role> pickRoleForGroups(
        const std::vector<std::string>& groups,
        const std::map<std::string, Role>& group_role_map);

    // commit 19/24 — per-channel ACL для LDAP: для каждой записи
    // channel_acl ищем у юзера matching-группы и берём максимальную
    // permission (Operate > View). Возвращаем список ChannelGrant,
    // готовый к запеканию в JWT claims. case-insensitive по DN.
    //
    // Каналы, для которых ни одна группа не подошла, в результат не
    // попадают вообще. Дубликатов channel_id в результате нет — каждый
    // ChannelAcl считаем отдельно, между записями с одним channel_id
    // permission'ы тоже мерджатся (Operate > View).
    struct ChannelAclEntry {
        std::int64_t                              channel_id{0};
        std::map<std::string, ChannelPermission>  group_perms;
    };
    static std::vector<ChannelGrant> pickAclGrantsForGroups(
        const std::vector<std::string>&        groups,
        const std::vector<ChannelAclEntry>&    channel_acl);

    LoginResult   login(std::string_view username,
                        std::string_view password,
                        std::string_view ip,
                        std::string_view user_agent);

    RefreshResult refresh(std::string_view refresh_token,
                          std::string_view ip,
                          std::string_view user_agent);

    // Returns true if a session row was updated. False if jwt_id unknown
    // (token already expired and reaped, or never existed).
    bool logout(std::string_view jwt_id);

    // Сheck both signature/exp AND revocation status. RBAC middleware
    // должен использовать именно это, не jwt.verify() напрямую.
    std::optional<JwtIssuer::Claims> verifyActiveAccess(std::string_view access_token);

    // ── fix32 B2: own-sessions API ──────────────────────────────────────
    //
    // Активные (revoked_at IS NULL, expires_at > now) сессии пользователя.
    // Caller — handler GET /api/auth/me/sessions; user_id берётся из
    // verifyActiveAccess(claims->user_id).
    std::vector<Session> listOwnActiveSessions(std::int64_t user_id);

    // Идемпотентный revoke сессии, ограниченный user_id — пользователь
    // не может ревокать чужую (юридически чужой jwt_id вернёт false).
    // Returns: true если ровно одна строка перешла из non-revoked в
    // revoked (повторный вызов на ту же сессию или чужой jwt_id → false).
    bool revokeOwnSession(std::string_view jwt_id, std::int64_t user_id);

    // ── Admin user CRUD (commit 7/24) ───────────────────────────────────
    //
    // Эти методы — обёртки над AuthDb + PasswordHasher. Live в AuthService
    // потому что (а) валидация username/email живёт в одном месте,
    // (б) reset-password требует генерации plaintext + hash в связке.
    // RBAC ограничивает их только админу — но это responsibility caller'а
    // (RbacMiddleware), AuthService сам не проверяет.

    enum class AdminError {
        UsernameInvalid,     // 400 — пусто/whitespace/длина
        EmailInvalid,        // 400 — есть @ но невалиден
        PasswordInvalid,     // 400 — пусто или не получилось хэшнуть
        UsernameTaken,       // 409
        UserNotFound,        // 404
        InternalError,       // 500
    };

    // Outcomes of adminPurgeUser. UserNotFound — id'а нет в БД.
    // CannotDeleteSelf — admin пытается удалить собственную учётку
    // (мы запрещаем, иначе текущая JWT-сессия окажется без user_id'а).
    // LastEnabledAdmin — единственный non-disabled admin; удаление
    // оставило бы control-plane без рутового доступа.
    enum class PurgeError {
        UserNotFound,        // 404
        CannotDeleteSelf,    // 409
        LastEnabledAdmin,    // 409
        InternalError,       // 500
    };

    struct CreateUserRequest {
        std::string username;
        std::string email;       // optional
        std::string password;    // empty → auto-generate
        Role        role{Role::Viewer};
        bool        must_change_password{false};
        // For commit 9 (bootstrap) — even non-bootstrap creates may want
        // initial-password TTL via separate setter; we keep simple here.
        std::optional<std::int64_t> created_by;
    };

    struct CreatedUser {
        User        user;
        // Plaintext password — выдаётся caller'у ОДИН раз. Если admin
        // указал свой пароль в request'e, plaintext пустой (admin его и
        // так знает). Если auto-generated — plaintext возвращается чтобы
        // admin мог показать пользователю.
        std::string plaintext_password;
    };

    struct UpdateUserRequest {
        std::string email;
        Role        role{Role::Viewer};
        Source      source{Source::Local};
    };

    std::vector<User>                                 adminListUsers();
    std::optional<User>                               adminGetUser(std::int64_t id);
    std::variant<CreatedUser, AdminError>             adminCreateUser(const CreateUserRequest& req);
    std::variant<User, AdminError>                    adminUpdateUser(std::int64_t id,
                                                                      const UpdateUserRequest& req);
    bool                                              adminSetDisabled(std::int64_t id, bool disabled);

    // Hard-delete (commit fix38-followup). Атомарно сносит users-row +
    // sessions/channel_permissions/password_resets + NULL'ит self-FK
    // в users.created_by, ldap_config.updated_by, smtp_config.updated_by.
    // auth_audit'у юзер не нужен (он хранит username snapshot'ом) — purge
    // совместим с retention-политикой. actor_id блокирует self-purge;
    // last-enabled-admin проверка не даёт залочить control-plane.
    // Caller обязан проверить RBAC=Admin до вызова.
    struct PurgedUser {
        std::int64_t id{0};
        std::string  username;        // snapshot для audit-payload
        std::string  email;
        Role         role{Role::Viewer};
    };
    std::variant<PurgedUser, PurgeError>
        adminPurgeUser(std::int64_t id, std::int64_t actor_id);

    // Generate fresh random password, set must_change_password=true,
    // revoke all active sessions (выкидывает юзера со всех устройств).
    // Returns plaintext, чтобы admin показал пользователю.
    std::optional<std::string>                        adminResetPassword(std::int64_t id);

    // ── Per-channel ACL for local users (commit 22/24) ─────────────────
    //
    // Тонкие фасады поверх AuthDb. Логика валидации (юзер существует,
    // не LDAP) живёт здесь — REST не должен лазить в БД напрямую.
    // LDAP-юзерам permissions вычисляются из group_role_map / channel_acl
    // и в этой таблице не лежат — REST ставит UserSourceMismatch.
    enum class ChannelAclError {
        UserNotFound,        // 404
        UserSourceMismatch,  // 400 — попытка задать local-ACL для LDAP-юзера
        InternalError,       // 500
    };

    using ChannelPermissionRow = AuthDb::ChannelPermissionRow;

    std::variant<std::vector<ChannelPermissionRow>, ChannelAclError>
        adminListChannelPermissionsForUser(std::int64_t user_id);

    std::variant<std::vector<ChannelPermissionRow>, ChannelAclError>
        adminListChannelPermissionsForChannel(std::int64_t channel_id);

    // Возвращает true на успех. ChannelAclError::UserSourceMismatch если
    // юзер LDAP — для них grants приходят из directory.
    std::variant<bool, ChannelAclError>
        adminSetChannelPermission(std::int64_t user_id,
                                  std::int64_t channel_id,
                                  ChannelPermission permission,
                                  std::int64_t granted_by);

    // true если row реально удалён, false если был только не-LDAP юзер
    // без такой записи (idempotent path не должен 404'ить — пусть REST
    // сам решает на основе removed=false).
    std::variant<bool, ChannelAclError>
        adminRemoveChannelPermission(std::int64_t user_id,
                                     std::int64_t channel_id);

    // ── Brute-force lockout (commit 13/24) ─────────────────────────────
    //
    // Политика — экспоненциальная задержка: после `threshold` подряд
    // неудачных логинов учётка блокируется на base_delay_sec; каждая
    // следующая неудача удваивает блокировку до max_lock_sec. Успешный
    // логин обнуляет всё. LDAP-учётки этим не покрываются (там свой flow).
    struct LockoutPolicy {
        int threshold      = 5;       // первая блокировка после стольких неудач
        int base_delay_sec = 30;      // длительность первой блокировки
        int max_lock_sec   = 3600;    // потолок (1 час) — чтобы оператор не
                                      //   ждал DOS, а админ-unlock работал.
    };

    LockoutPolicy lockoutPolicy() const noexcept { return policy_; }
    void setLockoutPolicy(LockoutPolicy p) noexcept { policy_ = p; }

    // Wire process-wide EventBus. Optional: when set, emitAudit() fans out
    // an AuthAudit event to SSE subscribers so /observability/events can
    // render admin actions in real time without polling the auth_audit
    // table. Nullable; unset (or explicitly nullptr) keeps the DB-only
    // behaviour used by unit tests that don't want a live bus.
    void setEventBus(liveqx::events::EventBus* bus) noexcept { event_bus_ = bus; }

    // Wire the enterprise AuditLogger. When set, every emitAudit() call
    // also mirrors the event into state/audit.db under Category::Auth so
    // login/logout/refresh/password lifecycle appears alongside all
    // other server mutations on one timeline. Nullable — unit tests and
    // legacy setups without an audit stack keep working unchanged.
    void setAuditLogger(liveqx::audit::AuditLogger* al) noexcept {
        audit_logger_ = al;
    }

    // Снимает блокировку и обнуляет счётчик. Возвращает true если юзер
    // существует. Идемпотентно: повторный unlock «чистого» юзера тоже true.
    bool adminUnlockUser(std::int64_t id);

    // ── Self-change password (commit 8/24) ─────────────────────────────
    //
    // POST /api/auth/me/password — пользователь сам меняет свой пароль.
    // Защита: требуем (а) валидный access_token (= знаем jwt_id, user_id),
    // (б) current_password (= защита от XSS со взломанным токеном — без
    // знания старого пароля атакующий не может lock'нуть юзера).
    // Side-effects: clear must_change_password, revoke other sessions
    // (текущая сессия остаётся живой — UX, не нужно re-login на этом же
    // девайсе).
    enum class SelfPasswordError {
        InvalidSession,        // 401 — bearer invalid/expired/revoked
        UserNotFound,          // 401 — токен ссылается на удалённого юзера
        UserDisabled,          // 403
        CurrentPasswordWrong,  // 403
        NewPasswordWeak,       // 400 — empty / < 8 chars / совпадает с текущим
        InternalError,         // 500
    };

    // Returns std::nullopt on success, error code otherwise.
    std::optional<SelfPasswordError> changeOwnPassword(
        std::string_view access_token,
        std::string_view current_password,
        std::string_view new_password);

    // ── Bootstrap initial admin (commit 9/24) ──────────────────────────
    //
    // Idempotent: ничего не делает, если уже есть хотя бы один не-disabled
    // admin. Иначе создаёт пользователя с заданным username, ролью Admin,
    // source=Local, must_change_password=true и случайным паролем.
    //
    // Возвращается plaintext пароль ТОЛЬКО при создании нового. Если
    // admin уже есть — std::nullopt (это сигнал "ничего не сделано").
    // Caller (main.cpp в commit 24) логирует пароль на stdout/WARN ОДИН
    // раз — единственный момент, когда он материализуется.
    struct BootstrapResult {
        bool        created{false};
        std::string username;
        std::string plaintext_password;  // populated only when created==true
        std::int64_t user_id{0};
        // fix35 (A3.16) — populated when setInitialAdminPasswordFile()
        // was wired and the write succeeded. main.cpp checks this to
        // decide whether to also LOG_WARN the password (file write
        // failed → log fallback) or stay quiet (file write succeeded →
        // operator reads the file, no plaintext in logs).
        std::filesystem::path password_file_path;
    };

    BootstrapResult bootstrapInitialAdmin(std::string_view username = "admin");

    // Hash a refresh token for storage. Public для тестов и
    // редких CLI-утилит; внутренний код этим не пользуется.
    static std::string hashRefreshToken(std::string_view plaintext);

    // ── Audit log (commit 12/24) ───────────────────────────────────────
    //
    // Тонкий fasade над AuthDb::insertAuditEvent — нужен чтобы (а) REST
    // мог логировать admin-действия с actor'ом (jti того, кто дёрнул
    // эндпоинт), (б) AuthService мог логировать self-events внутренне.
    // details_json пишется в "сырое" поле БД — caller сам сериализует
    // в JSON, или передаёт пустую строку.
    void emitAudit(std::string_view event,
                   std::optional<std::int64_t> user_id,
                   std::string_view username,
                   std::string_view ip,
                   std::string_view details_json);

    // Retention sweep — удаляет auth_audit-записи старше cutoff_ts.
    // Возвращает число удалённых записей. Идемпотентен.
    int purgeAuditOlderThan(std::int64_t cutoff_ts);

    // Convenience wrapper — same as purgeAuditOlderThan(now - days*86400).
    int purgeAuditOlderThanDays(int days);

    std::vector<AuditEvent> listAuditEvents(const AuditFilter& f);

private:
    AuthDb&             db_;
    JwtIssuer&          jwt_;
    GrantsResolver      grants_for_user_;
    LdapAuthenticator   ldap_authenticator_;
    LdapGrantsResolver  ldap_grants_resolver_;
    LockoutPolicy       policy_{};
    LdapCachePolicy     ldap_cache_policy_{};

    // fix35 A3.16 — see setInitialAdminPasswordFile(). Empty when not
    // wired (CLI-only contexts, certain unit tests).
    std::filesystem::path initial_admin_password_file_;

    // Set via setEventBus(). nullptr in unit tests — emitAudit skips the
    // publish step then; DB insert happens regardless.
    liveqx::events::EventBus* event_bus_{nullptr};

    // Set via setAuditLogger(). Optional — when non-null every emitAudit
    // is mirrored into the enterprise audit trail (state/audit.db) under
    // Category::Auth via the sync broken-glass path so a full async
    // backlog can never lose a login record.
    liveqx::audit::AuditLogger* audit_logger_{nullptr};

    // Issue + persist session row. Used by both login() and refresh().
    // grants_override: если указан, используется вместо grants_for_user_
    // (так LDAP-логин в commit 19/24 передаёт pre-computed channel_grants
    // напрямую, а refresh/local-login по-прежнему идут через resolver).
    std::optional<JwtIssuer::TokenPair>
        issueAndPersist(const User& u,
                        std::string_view ip,
                        std::string_view ua,
                        std::optional<std::vector<ChannelGrant>>
                            grants_override = std::nullopt);

    // c18/24 — LDAP-ветка login()'а. Приватная: вызывается только из
    // public login() после маршрутизации. existing_user — то, что лежит
    // в БД (если что-то лежит); может быть nullopt для свежего юзера.
    LoginResult loginLdap_(std::string_view username,
                           std::string_view password,
                           std::string_view ip,
                           std::string_view ua,
                           const std::optional<User>& existing_user);
};

const char* loginErrorName(AuthService::LoginError e) noexcept;
const char* refreshErrorName(AuthService::RefreshError e) noexcept;
const char* adminErrorName(AuthService::AdminError e) noexcept;
const char* selfPasswordErrorName(AuthService::SelfPasswordError e) noexcept;

}  // namespace liveqx::auth
