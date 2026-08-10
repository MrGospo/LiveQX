// fix41 commit 1 — unit tests для MountSpec и RpcProtocol.
//
// MountSpec покрывает: валидацию запрещённых символов, формат source
// для cifs/nfs, обязательность полей, JSON round-trip с/без password.
// RpcProtocol покрывает: encodeFrame длину/headers, fromJson сбор
// extra-полей, RpcOp <-> string.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "mounts/MountSpec.h"
#include "mounts/RpcProtocol.h"

using namespace liveqx::mounts;

// ─── MountSpec ──────────────────────────────────────────────────────────────

TEST(MountSpec, CifsHappyPath) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//host/share";
    s.target  = "/mnt/liveqx/lib1";
    s.options = "vers=3.0,iocharset=utf8";
    s.ro      = true;
    std::string err;
    EXPECT_TRUE(s.validate(err)) << err;
}

TEST(MountSpec, NfsHappyPath) {
    MountSpec s;
    s.fs_type = FsType::Nfs;
    s.source  = "nas.local:/export/video";
    s.target  = "/mnt/liveqx/nfs1";
    std::string err;
    EXPECT_TRUE(s.validate(err)) << err;
}

TEST(MountSpec, NewlineInOptionsRejected) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//h/s";
    s.target  = "/mnt/liveqx/x";
    s.options = "vers=3\n[Service]\nExecStart=/bin/sh";
    std::string err;
    EXPECT_FALSE(s.validate(err));
    EXPECT_NE(err.find("forbidden"), std::string::npos);
}

TEST(MountSpec, DotDotInTargetRejected) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//h/s";
    s.target  = "/mnt/liveqx/../etc";
    std::string err;
    EXPECT_FALSE(s.validate(err));
}

TEST(MountSpec, RelativeTargetRejected) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//h/s";
    s.target  = "mnt/liveqx/x";
    std::string err;
    EXPECT_FALSE(s.validate(err));
}

TEST(MountSpec, CifsBadSourceRejected) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "host/share";  // нет ведущих //
    s.target  = "/mnt/liveqx/x";
    std::string err;
    EXPECT_FALSE(s.validate(err));
}

TEST(MountSpec, NfsBadSourceRejected) {
    MountSpec s;
    s.fs_type = FsType::Nfs;
    s.source  = "nashost/path";  // нет :/
    s.target  = "/mnt/liveqx/x";
    std::string err;
    EXPECT_FALSE(s.validate(err));
}

TEST(MountSpec, NfsCannotCarryCifsCreds) {
    MountSpec s;
    s.fs_type = FsType::Nfs;
    s.source  = "nas:/p";
    s.target  = "/mnt/liveqx/x";
    s.cifs    = CifsCreds{"u", "p", ""};
    std::string err;
    EXPECT_FALSE(s.validate(err));
}

TEST(MountSpec, JsonRoundTripWithoutPassword) {
    MountSpec s;
    s.id      = 7;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = "/mnt/liveqx/lib";
    s.options = "vers=3.0";
    s.ro      = true;
    s.cifs    = CifsCreds{"viewer", "secret-pwd", "WORKGROUP"};

    auto j = s.toJson(/*include_password=*/false);
    EXPECT_FALSE(j["cifs"].contains("password"));
    EXPECT_EQ(j["cifs"]["username"], "viewer");
    EXPECT_EQ(j["cifs"]["domain"],   "WORKGROUP");

    std::string err;
    auto parsed = MountSpec::fromJson(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    EXPECT_EQ(parsed->source, s.source);
    ASSERT_TRUE(parsed->cifs.has_value());
    EXPECT_EQ(parsed->cifs->username, "viewer");
    EXPECT_TRUE(parsed->cifs->password.empty());
}

TEST(MountSpec, JsonRoundTripWithPassword) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = "/mnt/liveqx/lib";
    s.cifs    = CifsCreds{"viewer", "secret-pwd", ""};

    auto j = s.toJson(/*include_password=*/true);
    EXPECT_EQ(j["cifs"]["password"], "secret-pwd");

    std::string err;
    auto parsed = MountSpec::fromJson(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    EXPECT_EQ(parsed->cifs->password, "secret-pwd");
}

TEST(MountSpec, FromJsonUnknownFsTypeRejected) {
    nlohmann::json j = {{"fs_type", "nfs4"}, {"source", "x:/y"},
                        {"target", "/mnt/liveqx/x"}};
    std::string err;
    EXPECT_FALSE(MountSpec::fromJson(j, err).has_value());
    EXPECT_NE(err.find("fs_type"), std::string::npos);
}

// ─── RpcProtocol ─────────────────────────────────────────────────────────────

TEST(RpcProtocol, OpToFromString) {
    EXPECT_STREQ(toString(RpcOp::ApplyMount),  "ApplyMount");
    EXPECT_STREQ(toString(RpcOp::RemoveMount), "RemoveMount");
    EXPECT_STREQ(toString(RpcOp::TestMount),   "TestMount");
    EXPECT_STREQ(toString(RpcOp::Status),      "Status");
    RpcOp op;
    EXPECT_TRUE(rpcOpFromString("Status", op));
    EXPECT_EQ(op, RpcOp::Status);
    EXPECT_FALSE(rpcOpFromString("Bogus", op));
}

TEST(RpcProtocol, EncodeFramePrefixesBigEndianLength) {
    nlohmann::json body = {{"op", "Status"}};
    auto frame = encodeFrame(body);
    ASSERT_GE(frame.size(), 4u);
    const auto len = (std::uint32_t(frame[0]) << 24)
                   | (std::uint32_t(frame[1]) << 16)
                   | (std::uint32_t(frame[2]) << 8)
                   |  std::uint32_t(frame[3]);
    EXPECT_EQ(len, frame.size() - 4);
    EXPECT_EQ(std::string(frame.begin() + 4, frame.end()), body.dump());
}

TEST(RpcProtocol, ReadFrameRoundTrip) {
    int fds[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    nlohmann::json body = {{"op", "ApplyMount"}, {"id", 42}};
    std::string err;
    EXPECT_TRUE(writeFrame(fds[0], body, err)) << err;

    nlohmann::json parsed;
    EXPECT_TRUE(readFrame(fds[1], parsed, err)) << err;
    EXPECT_EQ(parsed["op"], "ApplyMount");
    EXPECT_EQ(parsed["id"], 42);

    ::close(fds[0]); ::close(fds[1]);
}

TEST(RpcProtocol, ReadFrameRejectsHugeLength) {
    int fds[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // Header заявляет размер > kMaxFrameBytes — readFrame должен
    // отказать и не пытаться прочитать гигабайт мусора.
    const std::uint32_t huge = htonl(kMaxFrameBytes + 1);
    ::write(fds[0], &huge, 4);
    ::close(fds[0]);

    nlohmann::json out;
    std::string err;
    EXPECT_FALSE(readFrame(fds[1], out, err));
    EXPECT_NE(err.find("out of bounds"), std::string::npos);
    ::close(fds[1]);
}

TEST(RpcProtocol, ResponseFromJsonGathersExtra) {
    nlohmann::json j = {
        {"ok", true}, {"status", "ok"},
        {"file_count", 17}, {"sample", nlohmann::json::array({"a", "b"})}
    };
    auto r = RpcResponse::fromJson(j);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.status, "ok");
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.extra["file_count"], 17);
    EXPECT_EQ(r.extra["sample"].size(), 2u);
}

TEST(RpcProtocol, ResponseFailRoundTrip) {
    auto r1 = RpcResponse::fail("host unreachable", "failed");
    auto j  = r1.toJson();
    auto r2 = RpcResponse::fromJson(j);
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(r2.status, "failed");
    EXPECT_EQ(r2.error,  "host unreachable");
}
