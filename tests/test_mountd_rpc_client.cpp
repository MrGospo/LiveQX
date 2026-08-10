// fix41 commit 4 — MountdRpcClient (production client) round-trip tests.
//
// Spins up an RpcServer on an ephemeral socket, connects via the
// production client, and verifies dispatch + framing + error mapping.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "mounts/MountSpec.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/RpcProtocol.h"
#include "mounts/RpcServer.h"

namespace fs = std::filesystem;
using namespace liveqx::mounts;

namespace {

fs::path uniqueSocketPath() {
    return fs::temp_directory_path() /
        ("sc-mountd-client-test-"
         + std::to_string(::getpid()) + "-"
         + std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch().count())
         + ".sock");
}

struct ServerHarness {
    RpcServer::Config         cfg;
    std::unique_ptr<RpcServer> server;
    std::thread               thr;

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

MountSpec makeSpec(std::string target) {
    MountSpec s;
    s.id      = 7;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = std::move(target);
    s.options = "vers=3.0,iocharset=utf8";
    s.ro      = true;
    CifsCreds c; c.username = "alice"; c.password = "s3cret";
    s.cifs    = c;
    return s;
}

}  // namespace

TEST(MountdRpcClient, ApplyMountDispatchesAndCarriesSpec) {
    std::atomic<int> seen{0};
    nlohmann::json   captured;
    ServerHarness h([&](RpcOp op, const nlohmann::json& body) -> RpcResponse {
        ++seen;
        EXPECT_EQ(op, RpcOp::ApplyMount);
        captured = body;
        return RpcResponse::okWith("active",
            nlohmann::json{{"active_state", "active"}});
    });

    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    auto resp = client.applyMount(makeSpec("/mnt/liveqx/lib1"));
    EXPECT_TRUE(resp.ok);
    EXPECT_EQ(resp.status, "active");
    EXPECT_EQ(seen.load(), 1);
    ASSERT_TRUE(captured.contains("spec"));
    EXPECT_EQ(captured["spec"]["target"], "/mnt/liveqx/lib1");
    // Password must be wired through to mountd (it goes into systemd-creds).
    EXPECT_EQ(captured["spec"]["cifs"]["password"], "s3cret");
}

TEST(MountdRpcClient, RemoveMountSendsIdAndTarget) {
    nlohmann::json   captured;
    ServerHarness h([&](RpcOp op, const nlohmann::json& body) -> RpcResponse {
        EXPECT_EQ(op, RpcOp::RemoveMount);
        captured = body;
        return RpcResponse::okWith("removed");
    });
    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    auto resp = client.removeMount(42, "/mnt/liveqx/lib1");
    EXPECT_TRUE(resp.ok);
    EXPECT_EQ(captured["id"].get<std::int64_t>(), 42);
    EXPECT_EQ(captured["target"].get<std::string>(),
              "/mnt/liveqx/lib1");
}

TEST(MountdRpcClient, TestMountForwardsSpec) {
    nlohmann::json   captured;
    ServerHarness h([&](RpcOp op, const nlohmann::json& body) -> RpcResponse {
        EXPECT_EQ(op, RpcOp::TestMount);
        captured = body;
        return RpcResponse::okWith("ok");
    });
    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    auto resp = client.testMount(makeSpec("/mnt/liveqx/probe"));
    EXPECT_TRUE(resp.ok);
    EXPECT_EQ(captured["spec"]["target"], "/mnt/liveqx/probe");
}

TEST(MountdRpcClient, StatusForwardsItemsAndReturnsUnitsArray) {
    nlohmann::json captured;
    ServerHarness h([&](RpcOp op, const nlohmann::json& body) -> RpcResponse {
        EXPECT_EQ(op, RpcOp::Status);
        captured = body;
        nlohmann::json units = nlohmann::json::array();
        units.push_back({{"id", 1}, {"active_state", "active"}});
        units.push_back({{"id", 2}, {"active_state", "inactive"}});
        return RpcResponse::okWith("ok",
            nlohmann::json{{"units", units}});
    });
    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    std::vector<StatusQueryItem> items{
        {1, "/mnt/liveqx/lib1"},
        {2, "/mnt/liveqx/lib2"},
    };
    auto resp = client.status(items);
    EXPECT_TRUE(resp.ok);
    ASSERT_TRUE(resp.extra.contains("units"));
    EXPECT_EQ(resp.extra["units"].size(), 2u);

    ASSERT_TRUE(captured.contains("items"));
    EXPECT_EQ(captured["items"].size(), 2u);
    EXPECT_EQ(captured["items"][0]["id"].get<std::int64_t>(), 1);
    EXPECT_EQ(captured["items"][0]["target"].get<std::string>(),
              "/mnt/liveqx/lib1");
}

TEST(MountdRpcClient, MissingSocketYieldsConnectFailure) {
    MountdRpcClient::Config cfg;
    cfg.socket_path = "/tmp/sc-mountd-client-no-such-socket.sock";
    cfg.rpc_timeout = std::chrono::milliseconds(500);
    MountdRpcClient client(cfg);

    auto resp = client.status({});
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "rpc_connect_failed");
    EXPECT_NE(resp.error.find("rpc connect"), std::string::npos);
}

TEST(MountdRpcClient, HandlerErrorPropagates) {
    ServerHarness h([&](RpcOp, const nlohmann::json&) -> RpcResponse {
        return RpcResponse::fail("boom from handler", "rejected");
    });
    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    auto resp = client.status({});
    EXPECT_FALSE(resp.ok);
    EXPECT_NE(resp.error.find("boom from handler"), std::string::npos);
}

TEST(MountdRpcClient, MultipleSequentialCallsReuseConfig) {
    std::atomic<int> calls{0};
    ServerHarness h([&](RpcOp, const nlohmann::json&) -> RpcResponse {
        ++calls;
        return RpcResponse::okWith("ok");
    });
    MountdRpcClient::Config cfg;
    cfg.socket_path = h.cfg.socket_path;
    MountdRpcClient client(cfg);

    for (int i = 0; i < 5; ++i) {
        auto r = client.status({});
        EXPECT_TRUE(r.ok) << "iter " << i;
    }
    EXPECT_EQ(calls.load(), 5);
}
