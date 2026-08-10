#include <gtest/gtest.h>

#include "utils/UrlSanitize.h"

using liveqx::util::rtmpUrlForLogs;
using liveqx::util::rtspUrlForLogs;

namespace {

TEST(UrlSanitize, StripsPathFromRtmp) {
    EXPECT_EQ(rtmpUrlForLogs("rtmp://ingest.example.com/live/SECRET-KEY"),
              "rtmp://ingest.example.com");
}

TEST(UrlSanitize, StripsPathFromRtmps) {
    EXPECT_EQ(rtmpUrlForLogs("rtmps://secure.example.com/app/AnotherSecret"),
              "rtmps://secure.example.com");
}

TEST(UrlSanitize, KeepsExplicitPort) {
    EXPECT_EQ(rtmpUrlForLogs("rtmp://10.0.0.1:1935/live/key"),
              "rtmp://10.0.0.1:1935");
}

TEST(UrlSanitize, NoPathReturnsVerbatim) {
    EXPECT_EQ(rtmpUrlForLogs("rtmp://host.example"),
              "rtmp://host.example");
}

TEST(UrlSanitize, MissingSchemeReturnsPlaceholder) {
    EXPECT_EQ(rtmpUrlForLogs("just-a-string"), "rtmp://?");
}

TEST(UrlSanitize, EmptyInputReturnsPlaceholder) {
    EXPECT_EQ(rtmpUrlForLogs(""), "rtmp://?");
}

// Stream keys with weird characters must still be fully stripped.
TEST(UrlSanitize, StripsKeysWithSpecialChars) {
    EXPECT_EQ(rtmpUrlForLogs("rtmp://h.example/live/abc?token=xyz&u=1"),
              "rtmp://h.example");
}

// ── RTSP sanitization ────────────────────────────────────────────────

TEST(UrlSanitizeRtsp, KeepsPathButMasksUserinfo) {
    // Path identifies the camera channel and is operationally useful;
    // only userinfo password gets masked.
    EXPECT_EQ(
        rtspUrlForLogs("rtsp://admin:secret@192.168.1.10:554/h264/main"),
        "rtsp://admin:***@192.168.1.10:554/h264/main");
}

TEST(UrlSanitizeRtsp, NoUserinfoReturnsVerbatim) {
    EXPECT_EQ(rtspUrlForLogs("rtsp://192.168.1.10:554/Streaming/Channels/101"),
              "rtsp://192.168.1.10:554/Streaming/Channels/101");
}

TEST(UrlSanitizeRtsp, RtspsKeepsScheme) {
    EXPECT_EQ(rtspUrlForLogs("rtsps://secure.cam:8554/main"),
              "rtsps://secure.cam:8554/main");
    EXPECT_EQ(rtspUrlForLogs("rtsps://op:pw@secure.cam:8554/main"),
              "rtsps://op:***@secure.cam:8554/main");
}

TEST(UrlSanitizeRtsp, UserOnlyAuthIsAlsoMasked) {
    // `user@host` (no password) is uncommon but still a fingerprint we
    // don't want logged.
    EXPECT_EQ(rtspUrlForLogs("rtsp://onlyuser@host/path"),
              "rtsp://***@host/path");
}

TEST(UrlSanitizeRtsp, AtSignOnlyInPathIsNotTreatedAsUserinfo) {
    // Path may legitimately contain '@' (e.g. some cameras encode email
    // identifiers); only an '@' inside the authority counts as userinfo.
    EXPECT_EQ(rtspUrlForLogs("rtsp://host/profile@1/main"),
              "rtsp://host/profile@1/main");
}

TEST(UrlSanitizeRtsp, MissingSchemeReturnsPlaceholder) {
    EXPECT_EQ(rtspUrlForLogs("not-a-url"), "rtsp://?");
}

TEST(UrlSanitizeRtsp, EmptyInputReturnsPlaceholder) {
    EXPECT_EQ(rtspUrlForLogs(""), "rtsp://?");
}

} // namespace
