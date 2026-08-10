// fix22 commit 16/24 — LDAP client unit-тесты.
//
// Тесты делятся на две категории:
//   - pure-функции (escape, validate, buildUri, renderUserFilter) — идут
//     без сетевого IO, проверяют корректность экранирования и подстановки;
//   - authenticate/ping против заведомо недоступного хоста — проверяют,
//     что мы быстро падаем с понятной ошибкой и без зависаний.
//
// Полноценный e2e против реального DC выходит за рамки unit-уровня
// (нужен docker-compose с slapd) и появится в commit 21/24 как admin
// /api/auth/ldap/test.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "auth/LdapClient.h"

namespace sa = liveqx::auth;

namespace {

sa::LdapConfig makeConfig() {
    sa::LdapConfig c;
    c.enabled            = true;
    // 127.0.0.1 + случайный непривязанный port — гарантированный
    // ECONNREFUSED без DNS. Использовать .invalid нельзя: getaddrinfo
    // там зависает с retries на 20+ секунд, и LDAP_OPT_NETWORK_TIMEOUT
    // на это не влияет (он про socket connect, не про разрешение имени).
    c.server             = "127.0.0.1:39999";
    c.tls_mode           = sa::LdapTlsMode::Plain;
    c.base_dn            = "dc=corp,dc=example,dc=com";
    c.bind_dn            = "cn=svc,dc=corp,dc=example,dc=com";
    c.bind_password      = "svc-password";
    c.user_filter        = "(uid=%s)";
    c.network_timeout_sec = 1;  // не мариноваться при unreachable
    return c;
}

}  // namespace

TEST(LdapClientEscape, FilterEscapesAllSpecials) {
    EXPECT_EQ(sa::LdapClient::escapeFilter("alice"), "alice");
    EXPECT_EQ(sa::LdapClient::escapeFilter("a*b"),    "a\\2ab");
    EXPECT_EQ(sa::LdapClient::escapeFilter("(or)"),   "\\28or\\29");
    EXPECT_EQ(sa::LdapClient::escapeFilter("\\bs"),   "\\5cbs");
    // NUL не должен приехать в C-string'е — escape должен его убить.
    std::string nul_input;
    nul_input.push_back('a');
    nul_input.push_back('\0');
    nul_input.push_back('b');
    EXPECT_EQ(sa::LdapClient::escapeFilter(nul_input), "a\\00b");
}

TEST(LdapClientEscape, FilterEscapesNonAscii) {
    // Кириллица: 'я' (U+044F) → utf-8 0xD1 0x8F. Оба байта вне ASCII —
    // оба должны быть hex-экранированы.
    auto out = sa::LdapClient::escapeFilter("я");
    EXPECT_EQ(out, "\\d1\\8f");
}

TEST(LdapClientEscape, DnEscapesSpecialChars) {
    EXPECT_EQ(sa::LdapClient::escapeDn("alice"), "alice");
    EXPECT_EQ(sa::LdapClient::escapeDn("Doe, John"), "Doe\\, John");
    EXPECT_EQ(sa::LdapClient::escapeDn("a\\b"), "a\\\\b");
    // Leading space + leading hash — экранируются.
    EXPECT_EQ(sa::LdapClient::escapeDn(" lead"),  "\\ lead");
    EXPECT_EQ(sa::LdapClient::escapeDn("#hash"),  "\\#hash");
    // Trailing space — экранируется.
    EXPECT_EQ(sa::LdapClient::escapeDn("trail "), "trail\\ ");
    // '+', ';', '<', '>', '"' — экранируются всегда.
    EXPECT_EQ(sa::LdapClient::escapeDn("a+b;c<d>e\"f"),
              "a\\+b\\;c\\<d\\>e\\\"f");
}

TEST(LdapClientUri, AddsDefaultPortByTlsMode) {
    auto cfg = makeConfig();
    cfg.server = "dc.example.com";  // hostname без порта

    cfg.tls_mode = sa::LdapTlsMode::Plain;
    EXPECT_EQ(sa::LdapClient::buildUri(cfg),
              "ldap://dc.example.com:389");

    cfg.tls_mode = sa::LdapTlsMode::StartTls;
    EXPECT_EQ(sa::LdapClient::buildUri(cfg),
              "ldap://dc.example.com:389");

    cfg.tls_mode = sa::LdapTlsMode::Ldaps;
    EXPECT_EQ(sa::LdapClient::buildUri(cfg),
              "ldaps://dc.example.com:636");
}

TEST(LdapClientUri, KeepsExplicitPort) {
    auto cfg = makeConfig();
    cfg.server   = "dc.example.com:1636";
    cfg.tls_mode = sa::LdapTlsMode::Ldaps;
    EXPECT_EQ(sa::LdapClient::buildUri(cfg),
              "ldaps://dc.example.com:1636");
}

TEST(LdapClientUri, IPv6) {
    auto cfg = makeConfig();
    cfg.server   = "[2001:db8::1]:389";
    cfg.tls_mode = sa::LdapTlsMode::Plain;
    EXPECT_EQ(sa::LdapClient::buildUri(cfg),
              "ldap://[2001:db8::1]:389");
}

TEST(LdapClientFilter, RendersTemplateAndEscapes) {
    EXPECT_EQ(sa::LdapClient::renderUserFilter("(uid=%s)", "alice"),
              "(uid=alice)");
    EXPECT_EQ(sa::LdapClient::renderUserFilter("(uid=%s)", "a*b"),
              "(uid=a\\2ab)");
    // Multi-substitution (например AD'шный двойной filter):
    EXPECT_EQ(sa::LdapClient::renderUserFilter("(|(uid=%s)(mail=%s))", "x"),
              "(|(uid=x)(mail=x))");
}

TEST(LdapClientValidate, RejectsObviouslyBadConfigs) {
    sa::LdapConfig c;
    EXPECT_FALSE(sa::LdapClient::validate(c).empty());  // disabled

    c.enabled = true;
    EXPECT_NE(sa::LdapClient::validate(c).find("server"), std::string::npos);

    c.server  = "ldap.host";
    EXPECT_NE(sa::LdapClient::validate(c).find("base_dn"), std::string::npos);

    c.base_dn = "dc=x";
    c.user_filter = "noplaceholder";
    EXPECT_NE(sa::LdapClient::validate(c).find("%s"), std::string::npos);

    c.user_filter = "(uid=%s)";
    EXPECT_TRUE(sa::LdapClient::validate(c).empty());
}

TEST(LdapClientAuthenticate, FailsFastOnUnreachableHost) {
    auto cfg = makeConfig();
    sa::LdapClient cli(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    auto r = cli.authenticate("alice", "secret");
    const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(r.ok);
    // network_timeout_sec=1 → должны уложиться в ~3s даже с retries.
    EXPECT_LT(dt_ms, 5000) << "took " << dt_ms << "ms";
    EXPECT_TRUE(r.reason == sa::LdapClient::AuthResult::Reason::ConnectionFailed
             || r.reason == sa::LdapClient::AuthResult::Reason::BindServiceFailed)
        << "reason=" << static_cast<int>(r.reason) << " err=" << r.error;
}

TEST(LdapClientAuthenticate, RejectsEmptyPasswordWithoutNetwork) {
    auto cfg = makeConfig();
    sa::LdapClient cli(cfg);

    auto r = cli.authenticate("alice", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, sa::LdapClient::AuthResult::Reason::InvalidCredentials);
    EXPECT_NE(r.error.find("empty password"), std::string::npos);
}

TEST(LdapClientAuthenticate, RejectsBadConfigBeforeNetwork) {
    sa::LdapConfig bad;
    bad.enabled = false;  // → ConfigError
    sa::LdapClient cli(bad);

    auto r = cli.authenticate("alice", "secret");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, sa::LdapClient::AuthResult::Reason::ConfigError);
}

TEST(LdapClientPing, FailsFastOnUnreachableHost) {
    auto cfg = makeConfig();
    sa::LdapClient cli(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    auto p = cli.ping();
    const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(p.ok);
    EXPECT_LT(dt_ms, 5000) << "took " << dt_ms << "ms";
    EXPECT_FALSE(p.error.empty());
}
