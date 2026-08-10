#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "clips/LiveInputFactory.h"
#include "clips/PlaylistEntry.h"

using liveqx::live_input::UnsupportedTypeError;
using liveqx::playlist::EntryKind;
using liveqx::playlist::parseEntry;
using liveqx::playlist::parseEntryKind;
using liveqx::playlist::parsePlaylist;

namespace {

// fix13 c4 — playlist entry parser tests.
//
// The parser is the single point where REST handlers (and later c5
// ChannelInstance integration) reject a malformed `type:"live"`
// playlist entry. We pin the field-level error messages here so a
// future refactor doesn't accidentally collapse them into a single
// "bad cfg" payload — REST consumers rely on the precise reason to
// surface it to the operator.

TEST(PlaylistEntryKind, FileEntryWithoutTypeIsLegacyFile) {
    auto j = nlohmann::json{{"path", "A.mp4"}};
    EXPECT_EQ(parseEntryKind(j), EntryKind::File);
}

TEST(PlaylistEntryKind, FileEntryWithExplicitVideoTypeIsFile) {
    auto j = nlohmann::json{{"type", "video"}, {"path", "A.mp4"}};
    EXPECT_EQ(parseEntryKind(j), EntryKind::File);
}

TEST(PlaylistEntryKind, ImageTypeIsFile) {
    EXPECT_EQ(parseEntryKind(nlohmann::json{{"type", "image"}}),
              EntryKind::File);
}

TEST(PlaylistEntryKind, LiveTypeRecognized) {
    EXPECT_EQ(parseEntryKind(nlohmann::json{{"type", "live"}}),
              EntryKind::Live);
}

TEST(PlaylistEntryKind, NonObjectRejected) {
    EXPECT_THROW(parseEntryKind(nlohmann::json::array()),
                 std::invalid_argument);
    EXPECT_THROW(parseEntryKind(nlohmann::json("video")),
                 std::invalid_argument);
}

TEST(PlaylistEntryKind, NonStringTypeRejected) {
    EXPECT_THROW(parseEntryKind(nlohmann::json{{"type", 42}}),
                 std::invalid_argument);
}

TEST(PlaylistEntryKind, UnknownTypeRaisesUnsupportedType) {
    auto j = nlohmann::json{{"type", "audio"}};
    EXPECT_THROW(parseEntryKind(j), UnsupportedTypeError);
}

// ── File entry ─────────────────────────────────────────────────────────

TEST(PlaylistEntryFile, ParsesPathAndDuration) {
    auto j = nlohmann::json{{"type", "video"}, {"path", "A.mp4"}, {"duration", 12.5}};
    auto e = parseEntry(j);
    ASSERT_EQ(e.kind, EntryKind::File);
    EXPECT_EQ(e.file.path, "A.mp4");
    EXPECT_DOUBLE_EQ(e.file.display_duration, 12.5);
}

TEST(PlaylistEntryFile, FillsDefaultPhotoDurationWhenAbsent) {
    auto j = nlohmann::json{{"type", "image"}, {"path", "x.jpg"}};
    auto e = parseEntry(j, /*default_photo_duration=*/8.0);
    EXPECT_DOUBLE_EQ(e.file.display_duration, 8.0);
}

TEST(PlaylistEntryFile, RejectsMissingPath) {
    auto j = nlohmann::json{{"type", "video"}};
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryFile, RejectsNonPositiveDuration) {
    auto j = nlohmann::json{{"path", "A.mp4"}, {"duration", 0.0}};
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
    j["duration"] = -1.0;
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ── Live entry ─────────────────────────────────────────────────────────

nlohmann::json baseLive() {
    return {
        {"type", "live"},
        {"id",   "studio-morning"},
        {"input", {{"type", "rtmp"}, {"url", "rtmp://h/p"}}},
        {"duration_sec", 3600},
    };
}

TEST(PlaylistEntryLive, ParsesAllFieldsWithDefaults) {
    auto e = parseEntry(baseLive());
    ASSERT_EQ(e.kind, EntryKind::Live);
    EXPECT_EQ(e.live.id, "studio-morning");
    EXPECT_EQ(e.live.live.id, "studio-morning");
    EXPECT_EQ(e.live.live.duration_ns, 3'600'000'000'000ULL);
    EXPECT_EQ(e.live.live.warm_up_ns, 5'000'000'000ULL);     // default
    EXPECT_EQ(e.live.live.loss_threshold_ns, 2'000'000'000ULL); // 2s
    EXPECT_EQ(e.live.fallback_on_loss, "fallback_clip");
    EXPECT_TRUE(e.live.input.is_object());
    EXPECT_EQ(e.live.input["type"], "rtmp");
}

TEST(PlaylistEntryLive, ParsesExplicitWarmUpAndLossThreshold) {
    auto j = baseLive();
    j["warm_up_sec"]       = 8.5;
    j["loss_threshold_ms"] = 750;
    j["fallback_on_loss"]  = "freeze";
    auto e = parseEntry(j);
    EXPECT_EQ(e.live.live.warm_up_ns, 8'500'000'000ULL);
    EXPECT_EQ(e.live.live.loss_threshold_ns, 750'000'000ULL);
    EXPECT_EQ(e.live.fallback_on_loss, "freeze");
}

TEST(PlaylistEntryLive, RejectsMissingId) {
    auto j = baseLive();
    j.erase("id");
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
    j["id"] = "";
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsMissingInput) {
    auto j = baseLive();
    j.erase("input");
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsNonObjectInput) {
    auto j = baseLive();
    j["input"] = "rtmp://h/p";
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsUnknownInputType) {
    auto j = baseLive();
    j["input"]["type"] = "sdi";  // SDI/NDI ingest not yet supported
    // UnsupportedTypeError refines invalid_argument so REST can map
    // it to a precise reason; here we pin the more specific exception.
    EXPECT_THROW(parseEntry(j), UnsupportedTypeError);
}

TEST(PlaylistEntryLive, RejectsMissingDuration) {
    auto j = baseLive();
    j.erase("duration_sec");
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsNonPositiveDuration) {
    auto j = baseLive();
    j["duration_sec"] = 0;
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
    j["duration_sec"] = -10;
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsNegativeWarmUp) {
    auto j = baseLive();
    j["warm_up_sec"] = -1.0;
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsNonPositiveLossThreshold) {
    auto j = baseLive();
    j["loss_threshold_ms"] = 0;
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

TEST(PlaylistEntryLive, RejectsUnknownFallbackMode) {
    auto j = baseLive();
    j["fallback_on_loss"] = "panic";
    EXPECT_THROW(parseEntry(j), std::invalid_argument);
}

// ── Whole playlist ─────────────────────────────────────────────────────

TEST(PlaylistEntryList, MixesFileAndLive) {
    auto arr = nlohmann::json::array({
        {{"type", "video"}, {"path", "A.mp4"}, {"duration", 60}},
        baseLive(),
        {{"type", "video"}, {"path", "B.mp4"}, {"duration", 30}},
    });
    auto out = parsePlaylist(arr);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].kind, EntryKind::File);
    EXPECT_EQ(out[1].kind, EntryKind::Live);
    EXPECT_EQ(out[1].live.id, "studio-morning");
    EXPECT_EQ(out[2].kind, EntryKind::File);
}

TEST(PlaylistEntryList, RejectsDuplicateLiveIds) {
    auto a = baseLive();
    auto b = baseLive();   // same id "studio-morning"
    auto arr = nlohmann::json::array({a, b});
    EXPECT_THROW(parsePlaylist(arr), std::invalid_argument);
}

TEST(PlaylistEntryList, AllowsSameIdAcrossDifferentKinds) {
    // File entries don't carry a top-level id in the current schema,
    // so uniqueness applies only to live entries — pin that here so
    // a later refactor that adds id to file entries doesn't tighten
    // this implicitly.
    auto arr = nlohmann::json::array({
        {{"path", "A.mp4"}},
        baseLive(),
    });
    EXPECT_NO_THROW(parsePlaylist(arr));
}

TEST(PlaylistEntryList, ErrorMessagePrefixesIndex) {
    auto bad = baseLive();
    bad.erase("duration_sec");
    auto arr = nlohmann::json::array({{{"path", "A.mp4"}}, bad});
    try {
        parsePlaylist(arr);
        FAIL() << "expected throw";
    } catch (const std::invalid_argument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("playlist[1]"), std::string::npos)
            << "got: " << msg;
    }
}

TEST(PlaylistEntryList, RejectsNonArray) {
    EXPECT_THROW(parsePlaylist(nlohmann::json::object()),
                 std::invalid_argument);
}

} // namespace
