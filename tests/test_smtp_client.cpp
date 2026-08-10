// fix22 commit 23/24 — unit tests для SmtpClient (без сети).
//
// Проверяем pure helpers: validate(), buildUri(), formatFromHeader(),
// isPlausibleEmail(), buildPayload(). Реальная sendMail()-проверка с
// fakesmtpd не делается — это интеграция, отдельный тестовый стенд.

#include <gtest/gtest.h>

#include "auth/SmtpClient.h"

namespace sa = liveqx::auth;

namespace {
sa::SmtpConfig minimalCfg() {
    sa::SmtpConfig c;
    c.server     = "smtp.example.com";
    c.port       = 587;
    c.security   = sa::SmtpSecurity::StartTls;
    c.from_email = "noreply@example.com";
    c.timeout_sec = 15;
    return c;
}
}  // namespace

TEST(SmtpClient, IsPlausibleEmailRejectsBadInput) {
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail(""));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("noatsign"));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("two@@signs"));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("@leading"));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("trailing@"));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("with space@x"));
    EXPECT_FALSE(sa::SmtpClient::isPlausibleEmail("inj\r\nect@x"));
    EXPECT_TRUE(sa::SmtpClient::isPlausibleEmail("ok@example.com"));
    EXPECT_TRUE(sa::SmtpClient::isPlausibleEmail("a.b+c@domain.tld"));
}

TEST(SmtpClient, ValidateMinimalOk) {
    auto c = minimalCfg();
    EXPECT_EQ(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateRejectsEmptyServer) {
    auto c = minimalCfg();
    c.server.clear();
    EXPECT_NE(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateRejectsEmptyFromEmail) {
    auto c = minimalCfg();
    c.from_email.clear();
    EXPECT_NE(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateRejectsMalformedFromEmail) {
    auto c = minimalCfg();
    c.from_email = "noatsign";
    EXPECT_NE(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateRejectsAsymmetricCreds) {
    auto c = minimalCfg();
    c.username = "user";
    // password empty → mismatch
    EXPECT_NE(sa::SmtpClient::validate(c), "");
    c.username.clear();
    c.password = "secret";
    EXPECT_NE(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateAcceptsBothCredsSet) {
    auto c = minimalCfg();
    c.username = "user";
    c.password = "secret";
    EXPECT_EQ(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, ValidateRejectsBadTimeout) {
    auto c = minimalCfg();
    c.timeout_sec = 0;
    EXPECT_NE(sa::SmtpClient::validate(c), "");
    c.timeout_sec = 1000;
    EXPECT_NE(sa::SmtpClient::validate(c), "");
}

TEST(SmtpClient, BuildUriUsesSchemeAndPort) {
    auto c = minimalCfg();
    c.security = sa::SmtpSecurity::StartTls;
    c.port     = 587;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtp://smtp.example.com:587");

    c.security = sa::SmtpSecurity::Tls;
    c.port     = 465;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtps://smtp.example.com:465");

    c.security = sa::SmtpSecurity::None;
    c.port     = 25;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtp://smtp.example.com:25");
}

TEST(SmtpClient, BuildUriDefaultsPortByScheme) {
    auto c = minimalCfg();
    c.port = 0;

    c.security = sa::SmtpSecurity::None;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtp://smtp.example.com:25");
    c.security = sa::SmtpSecurity::StartTls;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtp://smtp.example.com:587");
    c.security = sa::SmtpSecurity::Tls;
    EXPECT_EQ(sa::SmtpClient::buildUri(c), "smtps://smtp.example.com:465");
}

TEST(SmtpClient, FormatFromHeaderPlainEmailWithoutName) {
    auto c = minimalCfg();
    c.from_name.clear();
    EXPECT_EQ(sa::SmtpClient::formatFromHeader(c), "noreply@example.com");
}

TEST(SmtpClient, FormatFromHeaderQuotedNameWithEmail) {
    auto c = minimalCfg();
    c.from_name  = "Streaming Core";
    EXPECT_EQ(sa::SmtpClient::formatFromHeader(c),
              "\"Streaming Core\" <noreply@example.com>");
}

TEST(SmtpClient, FormatFromHeaderEscapesQuotes) {
    auto c = minimalCfg();
    c.from_name  = "S\"C";
    EXPECT_EQ(sa::SmtpClient::formatFromHeader(c),
              "\"S\\\"C\" <noreply@example.com>");
}

TEST(SmtpClient, BuildPayloadHasRequiredHeadersAndBody) {
    auto c = minimalCfg();
    c.from_name = "SC";
    auto p = sa::SmtpClient::buildPayload(c, "user@x", "Subj", "Hello!");
    EXPECT_NE(p.find("To: user@x\r\n"),                std::string::npos);
    EXPECT_NE(p.find("From: \"SC\" <noreply@example.com>\r\n"),
              std::string::npos);
    EXPECT_NE(p.find("Subject: Subj\r\n"),             std::string::npos);
    EXPECT_NE(p.find("MIME-Version: 1.0\r\n"),         std::string::npos);
    EXPECT_NE(p.find("Content-Type: text/plain; charset=UTF-8\r\n"),
              std::string::npos);
    EXPECT_NE(p.find("\r\n\r\nHello!"),                std::string::npos);
}
