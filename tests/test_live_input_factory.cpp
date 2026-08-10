#include <gtest/gtest.h>

#include "clips/LiveInputFactory.h"

using liveqx::live_input::Kind;
using liveqx::live_input::UnsupportedTypeError;
using liveqx::live_input::kindToString;
using liveqx::live_input::parseKind;

namespace {

// fix13 c2 — discriminator unit tests.
//
// LiveInputFactory::build() lives in the FFmpeg-bound .cpp and is exercised
// by integration tests; here we pin the cheap header-only surface that REST
// validators and ChannelInstance use to answer "is this live-input type
// even known?" without paying for libav* link.

TEST(LiveInputFactoryKind, MulticastKindRoundTrips) {
    auto j = nlohmann::json{{"type", "multicast"}};
    EXPECT_EQ(parseKind(j), Kind::Multicast);
    EXPECT_EQ(kindToString(Kind::Multicast), "multicast");
}

TEST(LiveInputFactoryKind, RtmpKindRoundTrips) {
    auto j = nlohmann::json{{"type", "rtmp"}};
    EXPECT_EQ(parseKind(j), Kind::Rtmp);
    EXPECT_EQ(kindToString(Kind::Rtmp), "rtmp");
}

TEST(LiveInputFactoryKind, RtspKindRoundTrips) {
    auto j = nlohmann::json{{"type", "rtsp"}};
    EXPECT_EQ(parseKind(j), Kind::Rtsp);
    EXPECT_EQ(kindToString(Kind::Rtsp), "rtsp");
}

TEST(LiveInputFactoryKind, NdiKindRoundTrips) {
    auto j = nlohmann::json{{"type", "ndi"}};
    EXPECT_EQ(parseKind(j), Kind::Ndi);
    EXPECT_EQ(kindToString(Kind::Ndi), "ndi");
}

TEST(LiveInputFactoryKind, NonObjectRejected) {
    EXPECT_THROW(parseKind(nlohmann::json::array()), std::invalid_argument);
    EXPECT_THROW(parseKind(nlohmann::json("rtmp")),  std::invalid_argument);
    EXPECT_THROW(parseKind(nlohmann::json{}),        std::invalid_argument);
}

TEST(LiveInputFactoryKind, MissingTypeRejected) {
    auto j = nlohmann::json{{"url", "rtmp://x/y"}};
    EXPECT_THROW(parseKind(j), std::invalid_argument);
}

TEST(LiveInputFactoryKind, NonStringTypeRejected) {
    auto j = nlohmann::json{{"type", 42}};
    EXPECT_THROW(parseKind(j), std::invalid_argument);
}

TEST(LiveInputFactoryKind, UnknownTypeRaisesUnsupportedType) {
    // Future transports (SDI/NDI, ONVIF discovery, etc.) must be rejected
    // with a *distinct* exception so REST handlers can map it to 400 with
    // the right reason ("unsupported type") rather than "malformed cfg".
    auto j = nlohmann::json{{"type", "sdi"}};
    EXPECT_THROW(parseKind(j), UnsupportedTypeError);

    j = nlohmann::json{{"type", "wat"}};
    EXPECT_THROW(parseKind(j), UnsupportedTypeError);
}

TEST(LiveInputFactoryKind, UnsupportedTypeIsAlsoInvalidArgument) {
    // UnsupportedTypeError extends invalid_argument so generic catch-blocks
    // upstream still see the cfg as "rejected" without needing to know the
    // refinement.
    auto j = nlohmann::json{{"type", "sdi"}};
    try {
        parseKind(j);
        FAIL() << "parseKind should have thrown";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("sdi"), std::string::npos);
    } catch (...) {
        FAIL() << "wrong exception type";
    }
}

} // namespace
