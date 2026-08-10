#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "events/EventBus.h"
#include "gateway/Gateway.h"
#include "gateway/IGateway.h"
#include "gateway/GatewayCfg.h"
#include "gateway/GatewayManager.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace liveqx::gateway;

namespace {

// Per-test temp root under TMPDIR so parallel ctest runs don't clash.
fs::path makeTmpRoot(const std::string& tag) {
    auto base = fs::temp_directory_path() / "liveqx_gw_test";
    fs::create_directories(base);
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    auto root = base / (tag + "_" + stamp + "_" + std::to_string(seq++));
    fs::create_directories(root);
    return root;
}

json simpleCfg() {
    return json{
        {"name",    "alpha"},
        {"input",   {{"address", "127.0.0.1"}, {"port", 49000}}},
        {"outputs", json::array({
            json{{"address", "127.0.0.1"}, {"port", 49001}},
            json{{"address", "127.0.0.1"}, {"port", 49002}},
        })},
    };
}

// ─── CRUD ────────────────────────────────────────────────────────────────────

TEST(GatewayManager, CreatePersistsConfigAndAssignsSubIds) {
    auto root = makeTmpRoot("create");
    GatewayManager mgr(root);

    int id = -1;
    EXPECT_EQ(mgr.create(simpleCfg(), &id), GatewayManager::Result::Ok);
    EXPECT_EQ(id, 1);

    const auto cfg_path = root / "gw1-alpha" / "config.json";
    ASSERT_TRUE(fs::exists(cfg_path));

    std::ifstream f(cfg_path);
    json on_disk;
    f >> on_disk;
    EXPECT_EQ(on_disk["id"],   1);
    EXPECT_EQ(on_disk["name"], "alpha");
    ASSERT_EQ(on_disk["outputs"].size(), 2u);
    EXPECT_EQ(on_disk["outputs"][0]["id"], "out0");
    EXPECT_EQ(on_disk["outputs"][1]["id"], "out1");
}

TEST(GatewayManager, CreateDuplicateIdRejected) {
    auto root = makeTmpRoot("dupe");
    GatewayManager mgr(root);

    auto cfg1 = simpleCfg();
    cfg1["id"] = 7;
    EXPECT_EQ(mgr.create(cfg1), GatewayManager::Result::Ok);

    auto cfg2 = simpleCfg();
    cfg2["id"] = 7;
    EXPECT_EQ(mgr.create(cfg2), GatewayManager::Result::AlreadyExists);
}

TEST(GatewayManager, CreateBadJsonReturnsBadJson) {
    GatewayManager mgr(makeTmpRoot("badjson"));
    EXPECT_EQ(mgr.create(json{{"name","x"}}), GatewayManager::Result::BadJson);
}

TEST(GatewayManager, CreateUserSuppliedOutputIdConflict) {
    GatewayManager mgr(makeTmpRoot("conflict"));
    auto cfg = json{
        {"name",  "conflict"},
        {"input", {{"address","127.0.0.1"},{"port",49100}}},
        {"outputs", json::array({
            json{{"id","mine"},{"address","127.0.0.1"},{"port",49101}},
            json{{"id","mine"},{"address","127.0.0.1"},{"port",49102}},
        })},
    };
    EXPECT_EQ(mgr.create(cfg), GatewayManager::Result::OutputIdConflict);
}

TEST(GatewayManager, RemoveDeletesDirAndStopsGateway) {
    auto root = makeTmpRoot("remove");
    GatewayManager mgr(root);
    int id = -1;
    EXPECT_EQ(mgr.create(simpleCfg(), &id), GatewayManager::Result::Ok);
    const auto dir = root / "gw1-alpha";
    ASSERT_TRUE(fs::exists(dir));

    EXPECT_EQ(mgr.remove(id), GatewayManager::Result::Ok);
    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_FALSE(fs::exists(dir));

    EXPECT_EQ(mgr.remove(id), GatewayManager::Result::NotFound);
}

TEST(GatewayManager, PatchOutputsReplacesAndPersists) {
    auto root = makeTmpRoot("patch");
    GatewayManager mgr(root);
    int id = -1;
    EXPECT_EQ(mgr.create(simpleCfg(), &id), GatewayManager::Result::Ok);

    const auto patch = json{{"outputs", json::array({
        json{{"address","127.0.0.1"},{"port",49500}},
    })}};
    EXPECT_EQ(mgr.patch(id, patch), GatewayManager::Result::Ok);

    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    ASSERT_EQ(status["outputs"].size(), 1u);
    EXPECT_EQ(status["outputs"][0]["port"], 49500);

    std::ifstream f(root / "gw1-alpha" / "config.json");
    json on_disk; f >> on_disk;
    ASSERT_EQ(on_disk["outputs"].size(), 1u);
    EXPECT_EQ(on_disk["outputs"][0]["port"], 49500);
}

TEST(GatewayManager, PatchInputRejected) {
    GatewayManager mgr(makeTmpRoot("patch-in"));
    int id = -1; mgr.create(simpleCfg(), &id);
    EXPECT_EQ(mgr.patch(id, json{{"input",{{"address","127.0.0.1"},{"port",60000}}}}),
              GatewayManager::Result::BadPatch);
}

TEST(GatewayManager, PatchEmptyOutputsRejected) {
    GatewayManager mgr(makeTmpRoot("patch-empty"));
    int id = -1; mgr.create(simpleCfg(), &id);
    EXPECT_EQ(mgr.patch(id, json{{"outputs", json::array()}}),
              GatewayManager::Result::BadPatch);
}

TEST(GatewayManager, PatchRenameMovesDirectory) {
    auto root = makeTmpRoot("rename");
    GatewayManager mgr(root);
    int id = -1; mgr.create(simpleCfg(), &id);

    EXPECT_EQ(mgr.patch(id, json{{"name","beta"}}),
              GatewayManager::Result::Ok);
    EXPECT_FALSE(fs::exists(root / "gw1-alpha"));
    EXPECT_TRUE(fs::exists(root / "gw1-beta" / "config.json"));
}

// Helper: minimal Demux cfg with one output. Used by FEC tests because
// passthrough gateway doesn't yet wire FEC (FEC follow-up).
namespace {
json demuxCfg(int output_port = 49001) {
    return json{
        {"name",    "alpha"},
        {"mode",    "demux"},
        {"input",   {{"address", "127.0.0.1"}, {"port", 49000}}},
        {"outputs", json::array({
            json{{"id", "out0"}, {"address", "127.0.0.1"}, {"port", output_port}},
        })},
        {"demux",   {{"routes", json::array({
            json{{"service_id", 1}, {"output_id", "out0"}},
        })}}},
    };
}
}

// fix40 A7 — PATCH /api/gateways/{id} with a "fec" field hot-swaps the FEC
// configuration; new cfg is persisted and reflected on the runtime gateway.
TEST(GatewayManager, PatchFecUpdatesAndPersists) {
    auto root = makeTmpRoot("patch-fec");
    GatewayManager mgr(root);
    int id = -1; mgr.create(demuxCfg(), &id);

    const auto patch_body = json{{"fec", json{
        {"enabled", true},
        {"mode",    "2d"},
        {"L",       4},
        {"D",       4},
    }}};
    EXPECT_EQ(mgr.patch(id, patch_body), GatewayManager::Result::Ok);

    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    ASSERT_TRUE(status.contains("fec"));
    EXPECT_EQ(status["fec"]["enabled"], true);
    EXPECT_EQ(status["fec"]["mode"],    "2d");
    EXPECT_EQ(status["fec"]["L"],       4);
    EXPECT_EQ(status["fec"]["D"],       4);

    std::ifstream f(root / "gw1-alpha" / "config.json");
    json on_disk; f >> on_disk;
    ASSERT_TRUE(on_disk.contains("fec"));
    EXPECT_EQ(on_disk["fec"]["enabled"], true);
    EXPECT_EQ(on_disk["fec"]["mode"],    "2d");
}

// FEC port-overflow validation runs again on PATCH (mirrors parseGatewayCfg),
// so a malformed offset is rejected without disturbing the running cfg.
TEST(GatewayManager, PatchFecPortOverflowRejected) {
    auto root = makeTmpRoot("patch-fec-overflow");
    GatewayManager mgr(root);
    // Pin the output to a high port so even the column offset overflows.
    auto cfg = demuxCfg(/*output_port=*/65530);
    int id = -1; mgr.create(cfg, &id);

    const auto patch_body = json{{"fec", json{
        {"enabled",            true},
        {"mode",               "2d"},
        {"column_port_offset", 200},   // 65530 + 200 > 65535
        {"row_port_offset",    8},
    }}};
    EXPECT_EQ(mgr.patch(id, patch_body), GatewayManager::Result::BadJson);

    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    // Original cfg preserved — fec stays disabled (no fec block emitted when
    // disabled+default, so .contains("fec") may be false).
    if (status.contains("fec")) {
        EXPECT_FALSE(status["fec"].value("enabled", false));
    }
}

// ─── Reload ──────────────────────────────────────────────────────────────────

TEST(GatewayManager, LoadFromRootReconstructsGateways) {
    auto root = makeTmpRoot("reload");

    {
        GatewayManager mgr(root);
        int id = -1;
        ASSERT_EQ(mgr.create(simpleCfg(), &id), GatewayManager::Result::Ok);
    }

    GatewayManager mgr2(root);
    EXPECT_EQ(mgr2.loadFromRoot(), 1u);
    EXPECT_EQ(mgr2.size(), 1u);
    auto status = mgr2.statusJson(1);
    ASSERT_TRUE(status.is_object());
    EXPECT_EQ(status["name"],     "alpha");
    EXPECT_EQ(status["input"]["port"], 49000);
}

TEST(GatewayManager, LoadFromRootSkipsBrokenEntries) {
    auto root = makeTmpRoot("broken");
    fs::create_directories(root / "gw99-broken");
    std::ofstream(root / "gw99-broken" / "config.json") << "{ not json";

    fs::create_directories(root / "gw100-half");
    std::ofstream(root / "gw100-half" / "config.json") << R"({"id":100})";

    GatewayManager mgr(root);
    EXPECT_EQ(mgr.loadFromRoot(), 0u);
    EXPECT_EQ(mgr.size(), 0u);
}

// ─── Event publishing (fix33 D1) ─────────────────────────────────────────────

TEST(GatewayManager, StateChangePublishesEvent) {
    namespace events = liveqx::events;
    auto root = makeTmpRoot("evt");
    GatewayManager mgr(root);

    events::EventBus bus(/*replay=*/64, /*per_sub=*/64);
    auto sub = bus.subscribe();
    mgr.setEventBus(&bus);

    // Use a unique high port to avoid clashes with parallel tests.
    static std::atomic<int> port_seq{55000};
    const int base = port_seq.fetch_add(10);
    auto cfg = json{
        {"name",  "evt"},
        {"input", {{"address","127.0.0.1"},{"port", base}}},
        {"outputs", json::array({
            json{{"address","127.0.0.1"},{"port", base + 1}},
        })},
    };

    int id = -1;
    ASSERT_EQ(mgr.create(cfg, &id), GatewayManager::Result::Ok);

    // create() itself does not publish — only play/stop/patch.
    {
        auto early = sub->drain(std::chrono::milliseconds(50));
        EXPECT_TRUE(early.empty()) << "create() must not publish state events";
    }

    ASSERT_EQ(mgr.play(id), GatewayManager::Result::Ok);
    auto after_play = sub->drain(std::chrono::milliseconds(500));
    ASSERT_EQ(after_play.size(), 1u);
    EXPECT_EQ(after_play[0].type, events::EventType::GatewayStateChange);
    EXPECT_EQ(after_play[0].payload["id"],    id);
    EXPECT_EQ(after_play[0].payload["name"],  "evt");
    EXPECT_EQ(after_play[0].payload["state"], "running");

    ASSERT_EQ(mgr.stop(id), GatewayManager::Result::Ok);
    auto after_stop = sub->drain(std::chrono::milliseconds(500));
    ASSERT_EQ(after_stop.size(), 1u);
    EXPECT_EQ(after_stop[0].type, events::EventType::GatewayStateChange);
    EXPECT_EQ(after_stop[0].payload["state"], "stopped");
}

TEST(GatewayManager, StateChangeWithoutBusIsNoop) {
    GatewayManager mgr(makeTmpRoot("nobus"));
    static std::atomic<int> port_seq{55500};
    const int base = port_seq.fetch_add(10);
    auto cfg = json{
        {"name",  "nobus"},
        {"input", {{"address","127.0.0.1"},{"port", base}}},
        {"outputs", json::array({
            json{{"address","127.0.0.1"},{"port", base + 1}},
        })},
    };
    int id = -1;
    ASSERT_EQ(mgr.create(cfg, &id), GatewayManager::Result::Ok);
    EXPECT_EQ(mgr.play(id), GatewayManager::Result::Ok);
    EXPECT_EQ(mgr.stop(id), GatewayManager::Result::Ok);
}

TEST(GatewayManager, ListAndForEachGateway) {
    auto root = makeTmpRoot("list");
    GatewayManager mgr(root);
    int a = -1, b = -1;
    auto cfg_a = simpleCfg();
    cfg_a["name"] = "aaa";
    auto cfg_b = simpleCfg();
    cfg_b["name"] = "bbb";
    cfg_b["input"]["port"] = 49200;
    cfg_b["outputs"][0]["port"] = 49201;
    cfg_b["outputs"][1]["port"] = 49202;
    mgr.create(cfg_a, &a);
    mgr.create(cfg_b, &b);

    auto arr = mgr.listJson();
    EXPECT_EQ(arr.size(), 2u);

    int seen = 0;
    mgr.forEachGateway([&seen](const IGateway& gw) {
        EXPECT_GT(gw.id(), 0);
        ++seen;
    });
    EXPECT_EQ(seen, 2);
}

// fix40 A2 — factory dispatch on cfg.mode.
//
// Passthrough (default) gives a Gateway; "demux" gives a DemuxGateway.
// We can't see the concrete type through IGateway, but the demux gateway
// surfaces a `programs` field in its statusJson that the passthrough one
// never emits — that's a sufficient signal.

TEST(GatewayManager, ModeDemuxFactoryProducesDemuxGateway) {
    auto root = makeTmpRoot("demux_mode");
    GatewayManager mgr(root);

    json cfg = {
        {"name",  "demux1"},
        {"mode",  "demux"},
        {"input", {{"address", "127.0.0.1"}, {"port", 49500}}},
        {"outputs", json::array({
            json{{"id", "spts1"}, {"address", "127.0.0.1"}, {"port", 49501}},
        })},
        {"demux", {
            {"routes", json::array({
                json{{"service_id", 1}, {"output_id", "spts1"}},
            })},
        }},
    };

    int id = -1;
    ASSERT_EQ(mgr.create(cfg, &id), GatewayManager::Result::Ok);

    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    EXPECT_TRUE(status.contains("programs"))
        << "demux gateway should expose programs in statusJson";
}

TEST(GatewayManager, ModePassthroughDoesNotExposePrograms) {
    auto root = makeTmpRoot("passthrough_mode");
    GatewayManager mgr(root);
    int id = -1;
    ASSERT_EQ(mgr.create(simpleCfg(), &id), GatewayManager::Result::Ok);
    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    EXPECT_FALSE(status.contains("programs"));
}

TEST(GatewayManager, ModeRemuxFactoryProducesRemuxGateway) {
    auto root = makeTmpRoot("remux_ok");
    GatewayManager mgr(root);

    // Remux requires ≥2 inputs and exactly one output — supply a multi-input
    // config matching parseGatewayCfg's wire format.
    json cfg = {
        {"name",    "remux1"},
        {"mode",    "remux"},
        {"inputs",  json::array({
            json{{"address", "127.0.0.1"}, {"port", 49100}},
            json{{"address", "127.0.0.1"}, {"port", 49101}},
        })},
        {"outputs", json::array({
            json{{"address", "127.0.0.1"}, {"port", 49102}},
        })},
        {"remux",   {{"transport_stream_id", 5}, {"original_network_id", 9}}},
    };

    int id = -1;
    ASSERT_EQ(mgr.create(cfg, &id), GatewayManager::Result::Ok);
    auto status = mgr.statusJson(id);
    ASSERT_TRUE(status.is_object());
    EXPECT_EQ(status.value("mode", ""), "remux");
    ASSERT_TRUE(status.contains("inputs"));
    EXPECT_EQ(status["inputs"].size(), 2u);
}

TEST(GatewayManager, ModeRemuxBadCfgRejected) {
    auto root = makeTmpRoot("remux_bad");
    GatewayManager mgr(root);

    // Remux mode but only one input — parseGatewayCfg should throw → BadJson.
    json cfg = simpleCfg();
    cfg["mode"] = "remux";       // single input is invalid for remux
    int id = -1;
    EXPECT_EQ(mgr.create(cfg, &id), GatewayManager::Result::BadJson);
}

} // namespace
