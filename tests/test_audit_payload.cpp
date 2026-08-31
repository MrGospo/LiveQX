// AuditPayload — request-body sanitisation for audit rows.
//
// The middleware calls buildAuditDetailsFromBody on every mutation. These
// tests pin the exact behaviour the audit trail relies on:
//   * secrets are redacted regardless of nesting depth or key casing;
//   * non-JSON / oversized / malformed bodies degrade to a forensic note
//     rather than blowing the row up or dropping context entirely;
//   * details always fits under the size cap the DB expects.

#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "audit/AuditPayload.h"

using liveqx::audit::buildAuditDetailsFromBody;
using liveqx::audit::redactSensitiveKeys;
using nlohmann::json;

namespace {
json parse(const std::string& s) { return json::parse(s); }
}

TEST(AuditPayload, EmptyBodyReturnsEmptyObject) {
    EXPECT_EQ(buildAuditDetailsFromBody("", "application/json"), "{}");
}

TEST(AuditPayload, NonJsonContentTypeIsMarkedNote) {
    auto j = parse(buildAuditDetailsFromBody("<xml/>", "text/xml"));
    EXPECT_EQ(j["body_note"], "non_json");
    EXPECT_EQ(j["body_size"], 6);
    EXPECT_FALSE(j.contains("body"));
}

TEST(AuditPayload, MalformedJsonIsMarkedNote) {
    auto j = parse(buildAuditDetailsFromBody("{not valid", "application/json"));
    EXPECT_EQ(j["body_note"], "parse_error");
    EXPECT_FALSE(j.contains("body"));
}

TEST(AuditPayload, ValidJsonCarriedThrough) {
    const std::string body = R"({"name":"studio-a","enabled":true})";
    auto j = parse(buildAuditDetailsFromBody(body, "application/json; charset=utf-8"));
    EXPECT_EQ(j["body"]["name"], "studio-a");
    EXPECT_EQ(j["body"]["enabled"], true);
    EXPECT_EQ(j["body_size"], static_cast<int>(body.size()));
}

TEST(AuditPayload, RedactsPasswordAndTokenAtTopLevel) {
    const std::string body = R"({"user":"alice","password":"hunter2","token":"abc.def.ghi"})";
    auto j = parse(buildAuditDetailsFromBody(body, "application/json"));
    EXPECT_EQ(j["body"]["user"], "alice");
    EXPECT_EQ(j["body"]["password"], "[REDACTED]");
    EXPECT_EQ(j["body"]["token"], "[REDACTED]");
}

TEST(AuditPayload, RedactionIsCaseInsensitive) {
    const std::string body = R"({"Password":"x","API_KEY":"y","SessionID":"z"})";
    auto j = parse(buildAuditDetailsFromBody(body, "application/json"));
    EXPECT_EQ(j["body"]["Password"], "[REDACTED]");
    EXPECT_EQ(j["body"]["API_KEY"], "[REDACTED]");
    EXPECT_EQ(j["body"]["SessionID"], "[REDACTED]");
}

TEST(AuditPayload, RedactsNestedSecrets) {
    const std::string body =
        R"({"srt":{"host":"1.2.3.4","passphrase":"p"},"tls":{"private_key":"k"}})";
    auto j = parse(buildAuditDetailsFromBody(body, "application/json"));
    EXPECT_EQ(j["body"]["srt"]["host"], "1.2.3.4");
    // "passphrase" isn't in the list, so it must NOT be redacted — pins
    // the *actual* key list so a future rename doesn't silently leak.
    EXPECT_EQ(j["body"]["srt"]["passphrase"], "p");
    EXPECT_EQ(j["body"]["tls"]["private_key"], "[REDACTED]");
}

TEST(AuditPayload, RedactsSecretsInsideArrays) {
    const std::string body =
        R"({"users":[{"name":"a","password":"1"},{"name":"b","password":"2"}]})";
    auto j = parse(buildAuditDetailsFromBody(body, "application/json"));
    EXPECT_EQ(j["body"]["users"][0]["name"], "a");
    EXPECT_EQ(j["body"]["users"][0]["password"], "[REDACTED]");
    EXPECT_EQ(j["body"]["users"][1]["password"], "[REDACTED]");
}

TEST(AuditPayload, OversizedBodyCollapsesWithNote) {
    // 70 KB > 64 KB parse cap
    std::string big(70 * 1024, 'x');
    auto j = parse(buildAuditDetailsFromBody(big, "application/json"));
    EXPECT_EQ(j["body_note"], "too_large");
    EXPECT_EQ(j["body_size"], static_cast<int>(big.size()));
    EXPECT_FALSE(j.contains("body"));
}

TEST(AuditPayload, HugeParsedJsonExceedingDetailsCapCollapses) {
    // Parseable JSON whose serialized size crosses the 4KB details cap.
    json fat = json::object();
    for (int i = 0; i < 200; ++i)
        fat["key_" + std::to_string(i)] = std::string(50, 'a');
    const std::string body = fat.dump();
    ASSERT_LT(body.size(), 64u * 1024);   // under parse cap
    auto j = parse(buildAuditDetailsFromBody(body, "application/json"));
    EXPECT_EQ(j["body_note"], "truncated");
    EXPECT_EQ(j["body_size"], static_cast<int>(body.size()));
    EXPECT_FALSE(j.contains("body"));
}

TEST(AuditPayload, RedactInPlaceLeavesNonSecretUnchanged) {
    json v = {{"user", "root"}, {"secret", "s"}, {"nested", {{"token", "t"}}}};
    redactSensitiveKeys(v);
    EXPECT_EQ(v["user"], "root");
    EXPECT_EQ(v["secret"], "[REDACTED]");
    EXPECT_EQ(v["nested"]["token"], "[REDACTED]");
}
