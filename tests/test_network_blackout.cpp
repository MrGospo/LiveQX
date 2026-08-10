// fix26 c9 — unit tests for NetworkBlackout scenario.
//
// The scenario shells out to `tc` and requires root. To keep tests fully
// hermetic and runnable on CI / dev laptops, the production class accepts
// injectable TcRunner + PrivCheck hooks. Tests use a fake runner that
// records every command and a flag-driven priv check.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "stress/NetworkBlackout.h"
#include "stress/Scenario.h"

using nlohmann::json;
using liveqx::stress::makeScenario;
using liveqx::stress::NetworkBlackout;
using liveqx::stress::NetworkBlackoutIntervals;
using liveqx::stress::ScenarioContext;
using liveqx::stress::ScenarioEvent;

namespace {

ScenarioContext makeCtx(std::int64_t started_ms, std::uint64_t seed) {
    ScenarioContext ctx;
    ctx.mgr            = nullptr;       // network_blackout doesn't touch it
    ctx.run_started_ms = started_ms;
    ctx.rng.seed(seed);
    return ctx;
}

}  // namespace

TEST(NetworkBlackout, NotPrivilegedEmitsSkipEventAndDoesNotShellOut) {
    std::vector<std::string> cmds;
    auto runner = [&](const std::string& cmd) -> int {
        cmds.push_back(cmd);
        return 0;
    };
    auto priv = []() { return false; };

    NetworkBlackoutIntervals iv{1, 1, 100, 100};
    NetworkBlackout sc({"eth0"}, iv, runner, priv);
    auto ctx = makeCtx(0, 1);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, /*now_ms=*/2, /*elapsed_ms=*/2, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_FALSE(evs[0].ok);
    EXPECT_NE(evs[0].detail.find("CAP_NET_ADMIN"), std::string::npos);
    EXPECT_TRUE(cmds.empty());                       // tc must not be invoked
}

TEST(NetworkBlackout, PrivilegedKillThenRestoreCycleInvokesTc) {
    std::vector<std::string> cmds;
    auto runner = [&](const std::string& cmd) -> int {
        cmds.push_back(cmd);
        return 0;
    };
    auto priv = []() { return true; };

    NetworkBlackoutIntervals iv{1, 1, 5, 5};
    NetworkBlackout sc({"wan0"}, iv, runner, priv);
    auto ctx = makeCtx(0, 2);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, /*now_ms=*/2, /*elapsed_ms=*/2, evs);   // kill
    ASSERT_GE(evs.size(), 1u);
    EXPECT_TRUE(evs.back().ok) << evs.back().detail;
    ASSERT_GE(cmds.size(), 1u);
    EXPECT_NE(cmds[0].find("tc qdisc add dev wan0 root netem loss 100%"),
              std::string::npos);

    sc.tick(ctx, /*now_ms=*/100, /*elapsed_ms=*/100, evs);   // restore + maybe re-kill
    bool saw_del = false;
    for (const auto& c : cmds) {
        if (c.find("tc qdisc del dev wan0 root") != std::string::npos) saw_del = true;
    }
    EXPECT_TRUE(saw_del);
}

TEST(NetworkBlackout, TcAddFailureProducesErrorEventAndNoPending) {
    auto runner = [](const std::string& /*cmd*/) -> int { return 1; };
    auto priv   = []() { return true; };

    NetworkBlackoutIntervals iv{1, 1, 5, 5};
    NetworkBlackout sc({"eth0"}, iv, runner, priv);
    auto ctx = makeCtx(0, 3);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_FALSE(evs[0].ok);
    EXPECT_NE(evs[0].detail.find("tc add failed"), std::string::npos);

    // No pending state -> onFinish must not try to restore.
    sc.onFinish(ctx, evs);
    EXPECT_EQ(evs.size(), 1u);  // unchanged
}

TEST(NetworkBlackout, OnFinishRestoresPendingBlackout) {
    std::vector<std::string> cmds;
    auto runner = [&](const std::string& cmd) -> int {
        cmds.push_back(cmd);
        return 0;
    };
    auto priv = []() { return true; };

    NetworkBlackoutIntervals iv{1, 1, /*blackout far away*/100000, 100000};
    NetworkBlackout sc({"eth1"}, iv, runner, priv);
    auto ctx = makeCtx(0, 4);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 2, 2, evs);   // kill — restore_at far in future
    ASSERT_FALSE(cmds.empty());
    const auto cmds_after_kill = cmds.size();

    sc.onFinish(ctx, evs);
    ASSERT_GT(cmds.size(), cmds_after_kill);
    EXPECT_NE(cmds.back().find("tc qdisc del dev eth1 root"),
              std::string::npos);
}

TEST(NetworkBlackout, EmptyInterfacesNoOp) {
    std::vector<std::string> cmds;
    auto runner = [&](const std::string& cmd) -> int {
        cmds.push_back(cmd); return 0;
    };
    auto priv = []() { return true; };

    NetworkBlackoutIntervals iv{1, 1, 5, 5};
    NetworkBlackout sc({}, iv, runner, priv);
    auto ctx = makeCtx(0, 5);
    sc.onStart(ctx);

    std::vector<ScenarioEvent> evs;
    sc.tick(ctx, 100, 100, evs);
    EXPECT_TRUE(evs.empty());
    EXPECT_TRUE(cmds.empty());

    sc.onFinish(ctx, evs);
    EXPECT_TRUE(evs.empty());
}

TEST(ScenarioFactory, NetworkBlackoutRequiresInterfaces) {
    auto null1 = makeScenario("network_blackout", {});
    EXPECT_EQ(null1, nullptr);

    auto null2 = makeScenario("network_blackout",
                              json{{"interfaces", json::array()}});
    EXPECT_EQ(null2, nullptr);

    auto good = makeScenario("network_blackout",
                             json{{"interfaces", {"eth0"}}});
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->name(), "network_blackout");
}
