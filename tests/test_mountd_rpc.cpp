// fix41 commit 1 — integration-тест для RpcServer.
//
// Поднимает сервер на временном сокете (--skip-chown, чтобы юзер не
// дёргал группы), коннектится клиентом из того же процесса, шлёт
// запросы и проверяет диспатч + frame round-trip + dispatch для
// stub Status.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "mounts/RpcProtocol.h"
#include "mounts/RpcServer.h"

using namespace liveqx::mounts;
namespace fs = std::filesystem;

namespace {

fs::path uniqueSocketPath() {
    auto p = fs::temp_directory_path() /
        ("sc-mountd-test-"
         + std::to_string(::getpid()) + "-"
         + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
         + ".sock");
    return p;
}

int connectClient(const fs::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto s = path.string();
    std::memcpy(addr.sun_path, s.data(), s.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct ServerHarness {
    RpcServer::Config cfg;
    std::unique_ptr<RpcServer> server;
    std::thread thr;
    std::atomic<int> calls{0};

    explicit ServerHarness(RpcHandler h) {
        cfg.socket_path = uniqueSocketPath();
        cfg.skip_chown  = true;
        server = std::make_unique<RpcServer>(cfg, std::move(h));
        std::string err;
        if (!server->start(err)) {
            ADD_FAILURE() << "start: " << err;
        }
        thr = std::thread([this] { server->runForever(); });
    }
    ~ServerHarness() {
        server->stop();
        if (thr.joinable()) thr.join();
    }
};

}  // namespace

TEST(MountdRpc, StatusDispatchesAndReturnsOk) {
    std::atomic<int> seen{0};
    ServerHarness h([&](RpcOp op, const nlohmann::json&) -> RpcResponse {
        ++seen;
        EXPECT_EQ(op, RpcOp::Status);
        return RpcResponse::okWith("ok",
            nlohmann::json{{"units", nlohmann::json::array()}});
    });

    int fd = connectClient(h.cfg.socket_path);
    ASSERT_GE(fd, 0);

    nlohmann::json req = {{"op", "Status"}};
    std::string err;
    ASSERT_TRUE(writeFrame(fd, req, err)) << err;
    nlohmann::json resp;
    ASSERT_TRUE(readFrame(fd, resp, err)) << err;
    ::close(fd);

    EXPECT_EQ(seen.load(), 1);
    EXPECT_EQ(resp["ok"], true);
    EXPECT_EQ(resp["status"], "ok");
    EXPECT_TRUE(resp["units"].is_array());
}

TEST(MountdRpc, UnknownOpRejected) {
    ServerHarness h([&](RpcOp, const nlohmann::json&) -> RpcResponse {
        ADD_FAILURE() << "handler should not run for unknown op";
        return RpcResponse::fail("nope");
    });

    int fd = connectClient(h.cfg.socket_path);
    ASSERT_GE(fd, 0);

    nlohmann::json req = {{"op", "Bogus"}};
    std::string err;
    ASSERT_TRUE(writeFrame(fd, req, err));
    nlohmann::json resp;
    ASSERT_TRUE(readFrame(fd, resp, err));
    ::close(fd);

    EXPECT_EQ(resp["ok"],     false);
    EXPECT_EQ(resp["status"], "rejected");
    EXPECT_NE(resp["error"].get<std::string>().find("unknown op"),
              std::string::npos);
}

TEST(MountdRpc, HandlerExceptionBecomesFailureResponse) {
    ServerHarness h([&](RpcOp, const nlohmann::json&) -> RpcResponse {
        throw std::runtime_error("boom");
    });

    int fd = connectClient(h.cfg.socket_path);
    ASSERT_GE(fd, 0);

    nlohmann::json req = {{"op", "Status"}};
    std::string err;
    ASSERT_TRUE(writeFrame(fd, req, err));
    nlohmann::json resp;
    ASSERT_TRUE(readFrame(fd, resp, err));
    ::close(fd);

    EXPECT_EQ(resp["ok"], false);
    EXPECT_NE(resp["error"].get<std::string>().find("boom"),
              std::string::npos);
}

TEST(MountdRpc, MultipleSequentialConnections) {
    std::atomic<int> calls{0};
    ServerHarness h([&](RpcOp, const nlohmann::json&) -> RpcResponse {
        ++calls;
        return RpcResponse::okWith("ok");
    });

    for (int i = 0; i < 5; ++i) {
        int fd = connectClient(h.cfg.socket_path);
        ASSERT_GE(fd, 0) << "iter " << i;
        nlohmann::json req = {{"op", "Status"}};
        std::string err;
        ASSERT_TRUE(writeFrame(fd, req, err));
        nlohmann::json resp;
        ASSERT_TRUE(readFrame(fd, resp, err));
        ::close(fd);
    }
    EXPECT_EQ(calls.load(), 5);
}

TEST(MountdRpc, SocketRemovedOnDestroy) {
    fs::path p;
    {
        ServerHarness h([](RpcOp, const nlohmann::json&) {
            return RpcResponse::okWith("ok");
        });
        p = h.cfg.socket_path;
        EXPECT_TRUE(fs::exists(p));
    }
    EXPECT_FALSE(fs::exists(p))
        << "socket file should be cleaned up by RpcServer dtor";
}
