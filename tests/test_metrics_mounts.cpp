// fix41 commit 6 — Prometheus rendering smoke tests for the mounts subsystem.
//
// Constructs a MetricsCollector with a real MountManager backed by a tmp
// MountsDb + MasterKey + in-process MockRpcClient, inserts a couple of rows
// (one cifs / one nfs), then verifies:
//
//   • all four aggregate families appear when mounts!=nullptr
//   • the per-mount labelled family carries id/target/fs_type
//   • families are *omitted* when the manager pointer is nullptr
//   • totals reflect inserted rows; active count tracks an injected
//     status RPC payload merged via syncStatusFromHelper().
//
// No REST layer required — talks straight to MetricsCollector::renderPrometheus().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/MetricsCollector.h"
#include "auth/MasterKey.h"
#include "metrics/ProcessMetrics.h"
#include "mounts/MountManager.h"
#include "mounts/MountSpec.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/MountsDb.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace liveqx::mounts;
namespace sa = liveqx::auth;

namespace {

fs::path makeTmp(const std::string& tag) {
    static std::atomic<uint64_t> seq{0};
    auto base = fs::temp_directory_path() / "liveqx_metrics_mounts";
    fs::create_directories(base);
    auto p = base / (tag + "_" + std::to_string(::getpid()) + "_" +
                     std::to_string(seq++));
    fs::create_directories(p);
    return p;
}

struct MockRpc : public IMountdRpcClient {
    RpcResponse apply_resp =
        RpcResponse::okWith("active", json{{"active_state", "active"}});
    RpcResponse remove_resp = RpcResponse::okWith("removed");
    RpcResponse test_resp   = RpcResponse::okWith("ok");
    RpcResponse status_resp =
        RpcResponse::okWith("ok", json{{"units", json::array()}});

    RpcResponse applyMount(const MountSpec&)  override { return apply_resp; }
    RpcResponse removeMount(std::int64_t, std::string_view) override { return remove_resp; }
    RpcResponse testMount(const MountSpec&)   override { return test_resp; }
    RpcResponse status(const std::vector<StatusQueryItem>&) override { return status_resp; }
};

struct Fixture {
    fs::path                       tmp;
    std::unique_ptr<MountsDb>      db;
    std::unique_ptr<sa::MasterKey> mk;
    std::shared_ptr<MockRpc>       rpc;
    std::unique_ptr<MountManager>  mounts;

    explicit Fixture(const std::string& tag) : tmp(makeTmp(tag)) {
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
        db = std::make_unique<MountsDb>(tmp / "mounts.db");
        EXPECT_TRUE(db->open());
        mk = std::make_unique<sa::MasterKey>((tmp / "master.key").string());
        EXPECT_TRUE(mk->load());
        rpc = std::make_shared<MockRpc>();
        mounts = std::make_unique<MountManager>(*db, *mk, rpc);
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
};

MountSpec cifsSpec(const std::string& target) {
    MountSpec s;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = target;
    s.options = "vers=3.0";
    s.ro      = true;
    CifsCreds c;
    c.username = "alice";
    c.password = "s3cret";
    s.cifs = c;
    return s;
}

MountSpec nfsSpec(const std::string& target) {
    MountSpec s;
    s.fs_type = FsType::Nfs;
    s.source  = "srv:/export";
    s.target  = target;
    s.options = "vers=4.1";
    s.ro      = true;
    return s;
}

}  // namespace

TEST(MountsMetricsRender, FamiliesOmittedWhenManagerNull) {
    ChannelManager mgr(nullptr);
    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-10T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b,
                        /*gateways=*/nullptr,
                        /*stress=*/nullptr,
                        /*plugins=*/nullptr,
                        /*mounts=*/nullptr);

    const auto text = mc.renderPrometheus();
    EXPECT_EQ(text.find("liveqx_mount_active"),  std::string::npos);
    EXPECT_EQ(text.find("liveqx_mounts_total"),  std::string::npos);
    EXPECT_EQ(text.find("liveqx_mounts_active"), std::string::npos);
    EXPECT_EQ(text.find("liveqx_mounts_failed"), std::string::npos);
}

TEST(MountsMetricsRender, EmptyManagerEmitsZeroAggregates) {
    Fixture f("empty");
    ChannelManager mgr(nullptr);
    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-10T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b,
                        /*gateways=*/nullptr,
                        /*stress=*/nullptr,
                        /*plugins=*/nullptr,
                        f.mounts.get());

    const auto text = mc.renderPrometheus();
    EXPECT_NE(text.find("liveqx_mounts_total 0"),   std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_enabled 0"), std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_active 0"),  std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_failed 0"),  std::string::npos);
}

TEST(MountsMetricsRender, AggregatesReflectInsertedRows) {
    Fixture f("rows");

    auto r1 = f.mounts->addMount(cifsSpec("/mnt/liveqx/lib1"));
    ASSERT_TRUE(r1.ok) << r1.error;
    auto r2 = f.mounts->addMount(nfsSpec("/mnt/liveqx/lib2"));
    ASSERT_TRUE(r2.ok) << r2.error;

    // Inject a status payload: mark r1 active, r2 failed.
    f.rpc->status_resp = RpcResponse::okWith(
        "ok",
        json{{"units", json::array({
            json{{"id", r1.id}, {"active_state", "active"}},
            json{{"id", r2.id}, {"active_state", "failed"}},
        })}});
    f.mounts->syncStatusFromHelper();

    ChannelManager mgr(nullptr);
    ProcessMetrics pm(/*start=*/1234567890);
    BuildInfo b{"1.0.0", "abc", "2026-05-10T10:00:00Z", "Release"};
    MetricsCollector mc(mgr, pm, b,
                        /*gateways=*/nullptr,
                        /*stress=*/nullptr,
                        /*plugins=*/nullptr,
                        f.mounts.get());

    const auto text = mc.renderPrometheus();

    // Aggregate families.
    EXPECT_NE(text.find("liveqx_mounts_total 2"),   std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_enabled 2"), std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_active 1"),  std::string::npos);
    EXPECT_NE(text.find("liveqx_mounts_failed 1"),  std::string::npos);

    // Per-mount labelled family present, with both targets and fs_types.
    EXPECT_NE(text.find("liveqx_mount_active{"),       std::string::npos);
    EXPECT_NE(text.find("target=\"/mnt/liveqx/lib1\""), std::string::npos);
    EXPECT_NE(text.find("target=\"/mnt/liveqx/lib2\""), std::string::npos);
    EXPECT_NE(text.find("fs_type=\"cifs\""),                    std::string::npos);
    EXPECT_NE(text.find("fs_type=\"nfs\""),                     std::string::npos);
}
