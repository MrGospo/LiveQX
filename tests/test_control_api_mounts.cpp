// fix41 commit 5a — REST CRUD integration tests for /api/system/mounts.
//
// Spins up ControlApi on an ephemeral port with a real MountManager backed
// by a real MountsDb + MasterKey on tmp dirs and an in-process mock RPC
// client (so no liveqx-mountd daemon required). Drives create /
// list / get / update / delete / test / sync via httplib::Client.
//
// RBAC is wired but kept Public for these tests via a permissive RbacMiddleware
// configuration: ControlApi falls back to its own role check only when
// rbac!=nullptr. Passing nullptr keeps the endpoints reachable without auth,
// which is what we want for shape testing here.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "auth/MasterKey.h"
#include "mounts/MountManager.h"
#include "mounts/MountSpec.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/MountsDb.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using namespace liveqx::mounts;
namespace sa = liveqx::auth;

fs::path makeTmpRoot(const std::string& tag) {
    auto base = fs::temp_directory_path() / "liveqx_mounts_rest";
    fs::create_directories(base);
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    auto root = base / (tag + "_" + stamp + "_" + std::to_string(seq++));
    fs::create_directories(root);
    return root;
}

struct MockRpcClient : public IMountdRpcClient {
    std::atomic<int> apply_calls{0};
    std::atomic<int> remove_calls{0};
    std::atomic<int> test_calls{0};
    std::atomic<int> status_calls{0};

    RpcResponse apply_response =
        RpcResponse::okWith("active", json{{"active_state", "active"}});
    RpcResponse remove_response = RpcResponse::okWith("removed");
    RpcResponse test_response   = RpcResponse::okWith("ok");
    RpcResponse status_response =
        RpcResponse::okWith("ok", json{{"units", json::array()}});

    RpcResponse applyMount(const MountSpec&)  override { ++apply_calls;  return apply_response; }
    RpcResponse removeMount(std::int64_t, std::string_view) override { ++remove_calls; return remove_response; }
    RpcResponse testMount(const MountSpec&)   override { ++test_calls;   return test_response; }
    RpcResponse status(const std::vector<StatusQueryItem>&) override { ++status_calls; return status_response; }
};

struct ApiFixture {
    fs::path                       tmp;
    std::unique_ptr<MountsDb>      db;
    std::unique_ptr<sa::MasterKey> mk;
    std::shared_ptr<MockRpcClient> rpc;
    std::unique_ptr<MountManager>  mounts;
    ChannelManager                 manager;
    std::unique_ptr<ControlApi>    api;
    int                            port;

    ApiFixture(int p, fs::path root)
        : tmp(std::move(root)),
          manager(nullptr, fs::path{}),
          port(p) {
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
        db = std::make_unique<MountsDb>(tmp / "mounts.db");
        EXPECT_TRUE(db->open());
        mk = std::make_unique<sa::MasterKey>((tmp / "master.key").string());
        EXPECT_TRUE(mk->load());
        rpc = std::make_shared<MockRpcClient>();
        mounts = std::make_unique<MountManager>(*db, *mk, rpc);

        api = std::make_unique<ControlApi>(
            port, manager,
            /*metrics=*/nullptr, LivezOptions{},
            /*gateways=*/nullptr,
            /*auth=*/nullptr, /*ldap_repo=*/nullptr, /*smtp_repo=*/nullptr,
            /*rbac=*/nullptr,
            /*events=*/nullptr, /*preview=*/nullptr,
            /*stress=*/nullptr, /*plugins=*/nullptr,
            /*master_key=*/nullptr,
            /*mounts=*/mounts.get(),
            TlsBindings{});
        api->start();
        waitListening(port);
    }

    ~ApiFixture() {
        if (api) api->stop();
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    static void waitListening(int port) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(0, 50'000);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        FAIL() << "ControlApi never started listening on port " << port;
    }
};

// Each test uses a unique port to keep parallel ctest runs isolated.
constexpr int kBasePort = 18900;

json cifsBody(const std::string& target,
              const std::string& password = "s3cret",
              const std::string& username = "alice") {
    return json{
        {"fs_type",  "cifs"},
        {"source",   "//srv/share"},
        {"target",   target},
        {"options",  "vers=3.0,iocharset=utf8"},
        {"ro",       true},
        {"cifs", json{
            {"username", username},
            {"password", password},
        }},
    };
}

json nfsBody(const std::string& target) {
    return json{
        {"fs_type",  "nfs"},
        {"source",   "srv:/export/lib"},
        {"target",   target},
        {"options",  "vers=4.1"},
        {"ro",       true},
    };
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(ControlApiMounts, ListIsInitiallyEmpty) {
    ApiFixture f(kBasePort + 0, makeTmpRoot("rest-empty"));
    httplib::Client cli("127.0.0.1", f.port);
    auto r = cli.Get("/api/system/mounts");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("mounts"));
    EXPECT_TRUE(body["mounts"].is_array());
    EXPECT_EQ(body["mounts"].size(), 0u);
}

TEST(ControlApiMounts, CreateReturns201WithPublicShape) {
    ApiFixture f(kBasePort + 1, makeTmpRoot("rest-create"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/lib1");
    auto r = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201) << r->body;
    auto out = json::parse(r->body);
    EXPECT_GT(out["id"].get<std::int64_t>(), 0);
    EXPECT_EQ(out["fs_type"], "cifs");
    EXPECT_EQ(out["target"], "/mnt/liveqx/lib1");
    EXPECT_EQ(out["has_password"], true);
    EXPECT_EQ(out["cifs_username"], "alice");
    EXPECT_FALSE(out.contains("password"));
    EXPECT_FALSE(out.contains("password_blob"));

    EXPECT_EQ(f.rpc->apply_calls.load(), 1);
}

TEST(ControlApiMounts, CreateInvalidSpecReturns400) {
    ApiFixture f(kBasePort + 2, makeTmpRoot("rest-bad"));
    httplib::Client cli("127.0.0.1", f.port);

    json body = json{{"fs_type","cifs"}};   // missing source/target
    auto r = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(ControlApiMounts, CreateBadJsonReturns400) {
    ApiFixture f(kBasePort + 3, makeTmpRoot("rest-bad-json"));
    httplib::Client cli("127.0.0.1", f.port);
    auto r = cli.Post("/api/system/mounts", "{ not json", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST(ControlApiMounts, CreateDuplicateTargetReturns409) {
    ApiFixture f(kBasePort + 4, makeTmpRoot("rest-dup"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/dup");
    auto r1 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_EQ(r1->status, 201);

    auto r2 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 409) << r2->body;
    auto e = json::parse(r2->body);
    EXPECT_EQ(e["error_code"], "duplicate");
}

TEST(ControlApiMounts, GetByIdRoundTrip) {
    ApiFixture f(kBasePort + 5, makeTmpRoot("rest-get"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = nfsBody("/mnt/liveqx/lib2");
    auto r1 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_EQ(r1->status, 201);
    auto id = json::parse(r1->body)["id"].get<std::int64_t>();

    auto r2 = cli.Get(("/api/system/mounts/" + std::to_string(id)).c_str());
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200);
    auto out = json::parse(r2->body);
    EXPECT_EQ(out["id"], id);
    EXPECT_EQ(out["fs_type"], "nfs");
    EXPECT_EQ(out["has_password"], false);
}

TEST(ControlApiMounts, GetByIdUnknownReturns404) {
    ApiFixture f(kBasePort + 6, makeTmpRoot("rest-404"));
    httplib::Client cli("127.0.0.1", f.port);
    auto r = cli.Get("/api/system/mounts/9999");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

TEST(ControlApiMounts, UpdateChangesFields) {
    ApiFixture f(kBasePort + 7, makeTmpRoot("rest-update"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/orig");
    auto r1 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_EQ(r1->status, 201);
    auto id = json::parse(r1->body)["id"].get<std::int64_t>();
    EXPECT_EQ(f.rpc->apply_calls.load(), 1);

    // Change target and username; password omitted → keep existing blob.
    auto patch = body;
    patch["target"] = "/mnt/liveqx/renamed";
    patch["cifs"]["username"] = "bob";
    patch["cifs"].erase("password");

    auto r2 = cli.Put(("/api/system/mounts/" + std::to_string(id)).c_str(),
                      patch.dump(), "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200) << r2->body;
    auto out = json::parse(r2->body);
    EXPECT_EQ(out["target"], "/mnt/liveqx/renamed");
    EXPECT_EQ(out["cifs_username"], "bob");
    EXPECT_EQ(out["has_password"], true);    // kept
    EXPECT_EQ(f.rpc->apply_calls.load(), 2);
}

TEST(ControlApiMounts, UpdateUnknownReturns404) {
    ApiFixture f(kBasePort + 8, makeTmpRoot("rest-update-404"));
    httplib::Client cli("127.0.0.1", f.port);
    auto body = cifsBody("/mnt/liveqx/x");
    auto r = cli.Put("/api/system/mounts/9999", body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404) << r->body;
}

TEST(ControlApiMounts, DeleteRemovesRow) {
    ApiFixture f(kBasePort + 9, makeTmpRoot("rest-delete"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/del");
    auto r1 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_EQ(r1->status, 201);
    auto id = json::parse(r1->body)["id"].get<std::int64_t>();

    auto r2 = cli.Delete(("/api/system/mounts/" + std::to_string(id)).c_str());
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200);
    EXPECT_EQ(f.rpc->remove_calls.load(), 1);

    auto r3 = cli.Get(("/api/system/mounts/" + std::to_string(id)).c_str());
    EXPECT_EQ(r3->status, 404);
}

TEST(ControlApiMounts, DeleteUnknownIsIdempotent) {
    ApiFixture f(kBasePort + 10, makeTmpRoot("rest-delete-404"));
    httplib::Client cli("127.0.0.1", f.port);
    // MountManager::removeMount on missing row returns ok=true (idempotent).
    auto r = cli.Delete("/api/system/mounts/9999");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
}

TEST(ControlApiMounts, TestEndpointReturnsHelperVerdict) {
    ApiFixture f(kBasePort + 11, makeTmpRoot("rest-test-ok"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/probe");
    auto r = cli.Post("/api/system/mounts/test", body.dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200) << r->body;
    auto out = json::parse(r->body);
    EXPECT_EQ(out["ok"], true);
    EXPECT_EQ(f.rpc->test_calls.load(), 1);
    // Catalogue must remain empty — test never persists.
    auto list = cli.Get("/api/system/mounts");
    ASSERT_TRUE(list);
    EXPECT_EQ(json::parse(list->body)["mounts"].size(), 0u);
}

TEST(ControlApiMounts, TestEndpointReportsHelperFailure) {
    ApiFixture f(kBasePort + 12, makeTmpRoot("rest-test-fail"));
    f.rpc->test_response = RpcResponse::fail("can't reach share", "rpc");
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/probe");
    auto r = cli.Post("/api/system/mounts/test", body.dump(),
                      "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 502) << r->body;
    auto out = json::parse(r->body);
    EXPECT_EQ(out["ok"], false);
    EXPECT_NE(out["error"].get<std::string>().find("can't reach share"),
              std::string::npos);
}

TEST(ControlApiMounts, SyncReturnsLatestRow) {
    ApiFixture f(kBasePort + 13, makeTmpRoot("rest-sync"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/sync");
    auto r1 = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_EQ(r1->status, 201);
    auto id = json::parse(r1->body)["id"].get<std::int64_t>();

    f.rpc->status_response = RpcResponse::okWith(
        "ok", json{{"units", json::array({
            json{{"id", id}, {"active_state", "active"}}})}});

    auto r2 = cli.Post(
        ("/api/system/mounts/" + std::to_string(id) + "/sync").c_str(),
        "", "application/json");
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->status, 200) << r2->body;
    auto out = json::parse(r2->body);
    EXPECT_EQ(out["id"], id);
    EXPECT_GE(f.rpc->status_calls.load(), 1);
}

// ── OpenAPI contract — MountPublic shape from CRUD response ─────────────────
//
// fix41 commit 7. Locks the JSON shape returned by GET / POST against
// docs/openapi.yaml#components.schemas.MountPublic — if backend renames
// a field, this fires before UI/openapi regeneration drifts.
TEST(ControlApiMounts, ResponseMatchesMountPublicSchema) {
    ApiFixture f(kBasePort + 15, makeTmpRoot("rest-contract"));
    httplib::Client cli("127.0.0.1", f.port);

    auto body = cifsBody("/mnt/liveqx/contract");
    auto r = cli.Post("/api/system/mounts", body.dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 201) << r->body;
    auto out = json::parse(r->body);

    // openapi.yaml#components.schemas.MountPublic.required
    for (const auto& key : {
            "id", "fs_type", "source", "target", "options", "ro",
            "enabled", "has_password", "created_at", "updated_at"}) {
        EXPECT_TRUE(out.contains(key))
            << "MountPublic missing required field '" << key << "': " << r->body;
    }
    EXPECT_TRUE(out["id"].is_number_integer());
    EXPECT_TRUE(out["fs_type"].is_string());
    EXPECT_TRUE(out["source"].is_string());
    EXPECT_TRUE(out["target"].is_string());
    EXPECT_TRUE(out["options"].is_string());
    EXPECT_TRUE(out["ro"].is_boolean());
    EXPECT_TRUE(out["enabled"].is_boolean());
    EXPECT_TRUE(out["has_password"].is_boolean());
    EXPECT_TRUE(out["created_at"].is_number_integer());
    EXPECT_TRUE(out["updated_at"].is_number_integer());

    // Non-required but documented fields should not contain plaintext password.
    EXPECT_FALSE(out.contains("password"));
    EXPECT_FALSE(out.contains("password_blob"));
    EXPECT_FALSE(out.contains("cifs_password"));
}

TEST(ControlApiMounts, ServiceUnavailableWhenManagerNull) {
    fs::path tmp = makeTmpRoot("rest-503");
    ChannelManager cm(nullptr, fs::path{});
    ControlApi api(kBasePort + 14, cm,
                   /*metrics=*/nullptr, LivezOptions{},
                   /*gateways=*/nullptr,
                   /*auth=*/nullptr, /*ldap_repo=*/nullptr,
                   /*smtp_repo=*/nullptr, /*rbac=*/nullptr,
                   /*events=*/nullptr, /*preview=*/nullptr,
                   /*stress=*/nullptr, /*plugins=*/nullptr,
                   /*master_key=*/nullptr, /*mounts=*/nullptr,
                   TlsBindings{});
    api.start();
    httplib::Client cli("127.0.0.1", kBasePort + 14);
    cli.set_connection_timeout(0, 50'000);
    for (int i = 0; i < 100; ++i) {
        auto h = cli.Get("/healthz");
        if (h && h->status == 200) break;
        std::this_thread::sleep_for(20ms);
    }
    auto r = cli.Get("/api/system/mounts");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    api.stop();
    std::error_code ec; fs::remove_all(tmp, ec);
}

}  // namespace
