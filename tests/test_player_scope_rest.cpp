// REST: access to players by object and group (B13), with real accounts.
//
// ControlApi runs with authentication on (AuthService + RbacMiddleware) over a
// real PlayersService, so these tests see what a signed-in person really gets.
//
// What these tests protect:
//   * the list, the details and the tree show a non-administrator only what an
//     administrator has granted, and a player outside that is "not found", not
//     "forbidden" (its existence is not revealed);
//   * rights take effect and vanish at once: no re-login, no waiting for a token;
//   * only an administrator manages rights, and a bad list of rights is refused
//     whole with a reason;
//   * moving a screen needs the move right on both groups and stays inside one
//     object unless an administrator does it;
//   * removing a group or object sweeps the rights that pointed at it.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "api/ChannelManager.h"
#include "api/ControlApi.h"
#include "audit/AuditDb.h"
#include "audit/AuditLogger.h"
#include "auth/AuthDb.h"
#include "auth/AuthService.h"
#include "auth/JwtIssuer.h"
#include "auth/MasterKey.h"
#include "auth/RbacMiddleware.h"
#include "players/PlayersDb.h"
#include "players/PlayersService.h"
#include "utils/Tls.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using namespace liveqx::players;
namespace sa = liveqx::auth;

constexpr const char* kSecret = "0123456789abcdef0123456789abcdef";
constexpr int kBasePort = 19500;

httplib::Headers bearer(const std::string& token) { return {{"Authorization", "Bearer " + token}}; }

struct ScopeFixture {
    fs::path tmp;
    std::unique_ptr<sa::AuthDb>         authdb;
    std::unique_ptr<sa::JwtIssuer>      jwt;
    std::unique_ptr<sa::AuthService>    svc;
    std::unique_ptr<sa::RbacMiddleware> rbac;
    std::unique_ptr<sa::MasterKey>      mk;
    std::unique_ptr<liveqx::audit::AuditDb>     audit_db;
    std::unique_ptr<liveqx::audit::AuditLogger> audit_logger;
    std::unique_ptr<PlayersDb>          pdb;
    std::unique_ptr<PlayersService>     players;
    ChannelManager                      manager;
    std::unique_ptr<ControlApi>         api;
    int port;

    // O1 = {G11, G12}, O2 = {G21}; one player in each group.
    std::int64_t O1{}, O2{}, G11{}, G12{}, G21{};
    std::string  P11, P12, P21;

    explicit ScopeFixture(int p) : manager(nullptr, fs::path{}), port(p) {
        static std::atomic<int> seq{0};
        tmp = fs::temp_directory_path() /
              ("lqx-scope-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
        fs::create_directories(tmp / "tls");
        EXPECT_TRUE(liveqx::tls::ensureCa(tmp / "tls").ok());

        authdb = std::make_unique<sa::AuthDb>(tmp / "auth.db");
        EXPECT_TRUE(authdb->open());
        jwt = std::make_unique<sa::JwtIssuer>(std::string(kSecret));
        svc = std::make_unique<sa::AuthService>(*authdb, *jwt,
            [](std::int64_t) { return std::vector<sa::ChannelGrant>{}; });
        rbac = std::make_unique<sa::RbacMiddleware>(*authdb, *jwt);
        mk = std::make_unique<sa::MasterKey>((tmp / "master.key").string());
        EXPECT_TRUE(mk->load());
        audit_db = std::make_unique<liveqx::audit::AuditDb>(tmp / "audit.db", mk.get());
        EXPECT_TRUE(audit_db->open());
        audit_logger = std::make_unique<liveqx::audit::AuditLogger>(audit_db.get(), tmp / "audit-emergency.jsonl");
        audit_logger->start();

        pdb = std::make_unique<PlayersDb>(tmp / "players.db");
        EXPECT_TRUE(pdb->open());
        PlayersServiceOptions o;
        o.ca_cert_path = tmp / "tls" / "ca.crt";
        players = std::make_unique<PlayersService>(*pdb, std::move(o));

        O1 = pdb->createObject("O1")->id;
        O2 = pdb->createObject("O2")->id;
        G11 = pdb->createGroup(O1, "G11")->id;
        G12 = pdb->createGroup(O1, "G12")->id;
        G21 = pdb->createGroup(O2, "G21")->id;
        P11 = pdb->createPlayer("screen-11", 1, "strict", 86400, 1'800'000'000, G11)->player.id;
        P12 = pdb->createPlayer("screen-12", 1, "strict", 86400, 1'800'000'000, G12)->player.id;
        P21 = pdb->createPlayer("screen-21", 1, "strict", 86400, 1'800'000'000, G21)->player.id;

        api = std::make_unique<ControlApi>(
            port, manager, nullptr, LivezOptions{}, nullptr, svc.get(), nullptr, nullptr, rbac.get(),
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, TlsBindings{}, nullptr, nullptr, nullptr,
            audit_logger.get(), nullptr, players.get());
        api->start();
        httplib::Client cli("127.0.0.1", port);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started on port " << port;
    }
    ~ScopeFixture() {
        if (api) api->stop();
        api.reset();
        if (audit_logger) audit_logger->stop();
        audit_logger.reset();
        audit_db.reset();
        players.reset();
        pdb.reset();
        rbac.reset();
        svc.reset();
        authdb.reset();
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    httplib::Client client() const {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(10, 0);
        return c;
    }

    struct Person { std::int64_t id; std::string token; };
    Person person(const std::string& name, sa::Role role) {
        sa::AuthService::CreateUserRequest req;
        req.username = name;
        req.password = "Pass1234!";
        req.role = role;
        req.must_change_password = false;
        auto cr = svc->adminCreateUser(req);
        EXPECT_TRUE(std::holds_alternative<sa::AuthService::CreatedUser>(cr));
        auto id = std::get<sa::AuthService::CreatedUser>(cr).user.id;
        auto r = svc->login(name, "Pass1234!", "127.0.0.1", "test");
        EXPECT_TRUE(std::holds_alternative<sa::JwtIssuer::TokenPair>(r.outcome));
        return {id, std::get<sa::JwtIssuer::TokenPair>(r.outcome).access_token};
    }

    static json grant(const char* scope, std::int64_t id, bool cg = false, bool co = false, bool mv = false,
                      bool au = false) {
        return {{"scope", scope}, {"id", id}, {"can_command_group", cg}, {"can_command_object", co},
                {"can_move", mv}, {"can_view_audit", au}};
    }
    // The administrator's way of giving rights.
    httplib::Result putGrants(const Person& admin, std::int64_t user_id, const json& grants) {
        auto cli = client();
        return cli.Put(("/api/auth/users/" + std::to_string(user_id) + "/player-grants").c_str(),
                       bearer(admin.token), json{{"grants", grants}}.dump(), "application/json");
    }
    std::set<std::string> listedNames(const Person& who) {
        auto cli = client();
        auto r = cli.Get("/api/players", bearer(who.token));
        std::set<std::string> out;
        EXPECT_TRUE(r);
        if (!r) return out;
        EXPECT_EQ(r->status, 200) << r->body;
        if (r->status == 200) {
            const json body = json::parse(r->body);     // a named object: the loop must not walk a temporary
            for (const auto& p : body["players"]) out.insert(p["name"].get<std::string>());
        }
        return out;
    }
};

using Names = std::set<std::string>;

}  // namespace

TEST(PlayerScopeRest, AdministratorsSeeEveryoneOthersSeeNothingUntilGranted) {
    ScopeFixture f(kBasePort + 0);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);

    EXPECT_EQ(f.listedNames(admin), (Names{"screen-11", "screen-12", "screen-21"}));
    EXPECT_TRUE(f.listedNames(op).empty()) << "fail closed";
    EXPECT_TRUE(f.listedNames(view).empty());
}

TEST(PlayerScopeRest, AGroupRightShowsThatGroupAnObjectRightShowsAllItsGroups) {
    ScopeFixture f(kBasePort + 1);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", f.G11)}))->status, 200);
    ASSERT_EQ(f.putGrants(admin, view.id, json::array({ScopeFixture::grant("object", f.O1)}))->status, 200);

    EXPECT_EQ(f.listedNames(op), (Names{"screen-11"}));
    EXPECT_EQ(f.listedNames(view), (Names{"screen-11", "screen-12"}));
}

TEST(PlayerScopeRest, ARightIsGoneTheMomentItIsTakenAway) {
    ScopeFixture f(kBasePort + 2);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", f.G11)}))->status, 200);
    ASSERT_EQ(f.listedNames(op).size(), 1u);

    ASSERT_EQ(f.putGrants(admin, op.id, json::array())->status, 200);
    EXPECT_TRUE(f.listedNames(op).empty()) << "same token, no re-login";
}

TEST(PlayerScopeRest, APlayerOutsideTheRightsIsNotFoundNotForbidden) {
    ScopeFixture f(kBasePort + 3);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", f.G11)}))->status, 200);
    auto cli = f.client();
    EXPECT_EQ(cli.Get(("/api/players/" + f.P11).c_str(), bearer(op.token))->status, 200);
    auto hidden = cli.Get(("/api/players/" + f.P12).c_str(), bearer(op.token));
    ASSERT_TRUE(hidden);
    EXPECT_EQ(hidden->status, 404) << "existence is not revealed";
    EXPECT_EQ(json::parse(hidden->body)["error"], "not_found");
    EXPECT_EQ(cli.Get(("/api/players/" + f.P21).c_str(), bearer(op.token))->status, 404);
}

TEST(PlayerScopeRest, TheTreeShowsTheGrantedObjectAndGroupsAndNothingElse) {
    ScopeFixture f(kBasePort + 4);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);
    auto none  = f.person("none", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", f.G11)}))->status, 200);
    ASSERT_EQ(f.putGrants(admin, view.id, json::array({ScopeFixture::grant("object", f.O1)}))->status, 200);

    auto tree = [&](const ScopeFixture::Person& who) {
        auto cli = f.client();
        auto r = cli.Get("/api/player-objects", bearer(who.token));
        EXPECT_TRUE(r);
        EXPECT_EQ(r ? r->status : 0, 200);
        return r ? json::parse(r->body)["objects"] : json::array();
    };
    EXPECT_EQ(tree(admin).size(), 2u);

    auto t = tree(op);
    ASSERT_EQ(t.size(), 1u) << "the building holding the granted group is named";
    EXPECT_EQ(t[0]["name"], "O1");
    ASSERT_EQ(t[0]["groups"].size(), 1u) << "its other groups stay hidden";
    EXPECT_EQ(t[0]["groups"][0]["name"], "G11");

    t = tree(view);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0]["groups"].size(), 2u) << "an object right shows all its groups";

    EXPECT_TRUE(tree(none).empty());
}

TEST(PlayerScopeRest, OnlyAnAdministratorManagesRights) {
    ScopeFixture f(kBasePort + 5);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);
    auto cli = f.client();
    for (const auto& who : {op, view}) {
        EXPECT_EQ(f.putGrants(who, op.id, json::array())->status, 403);
        EXPECT_EQ(cli.Get(("/api/auth/users/" + std::to_string(op.id) + "/player-grants").c_str(), bearer(who.token))->status, 403);
    }
    EXPECT_EQ(cli.Get(("/api/auth/users/" + std::to_string(op.id) + "/player-grants").c_str(), bearer(admin.token))->status, 200);
}

TEST(PlayerScopeRest, ABadListOfRightsIsRefusedWholeAndSaysWhy) {
    ScopeFixture f(kBasePort + 6);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", f.G11)}))->status, 200);

    auto err = [&](std::int64_t uid, const json& g) {
        auto r = f.putGrants(admin, uid, g);
        return std::make_pair(r ? r->status : 0, r ? json::parse(r->body, nullptr, false) : json());
    };

    EXPECT_EQ(err(op.id, json::array({ScopeFixture::grant("group", 424242)})).first, 400);
    EXPECT_EQ(err(op.id, json::array({ScopeFixture::grant("group", 424242)})).second["error"], "unknown_scope");
    EXPECT_EQ(err(op.id, json::array({ScopeFixture::grant("object", 424242)})).second["error"], "unknown_scope");
    EXPECT_EQ(err(op.id, json::array({json{{"scope", "building"}, {"id", 1}}})).second["error"], "invalid_scope");
    EXPECT_EQ(err(op.id, json::array({ScopeFixture::grant("group", f.G11), ScopeFixture::grant("group", f.G11)})).second["error"],
              "duplicate_grant");
    EXPECT_EQ(err(op.id, json::array({ScopeFixture::grant("group", f.G11, false, /*co=*/true)})).second["error"],
              "invalid_flag") << "'command the object' makes no sense on a group";
    EXPECT_EQ(err(view.id, json::array({ScopeFixture::grant("object", f.O1, /*cg=*/true)})).second["error"],
              "flag_not_allowed_for_role") << "a viewer never commands";
    EXPECT_EQ(err(424242, json::array()).first, 404);
    auto notArray = f.putGrants(admin, op.id, json::object());
    ASSERT_TRUE(notArray);
    EXPECT_EQ(notArray->status, 400);

    EXPECT_EQ(f.listedNames(op), (Names{"screen-11"})) << "every refusal left the old rights alone";
}

TEST(PlayerScopeRest, TheListOfRightsCanBeReadBack) {
    ScopeFixture f(kBasePort + 7);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("object", f.O1, true, false, true, false),
                                                     ScopeFixture::grant("group", f.G21, false, false, false, true)}))->status, 200);
    auto cli = f.client();
    auto r = cli.Get(("/api/auth/users/" + std::to_string(op.id) + "/player-grants").c_str(), bearer(admin.token));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    auto gs = json::parse(r->body)["grants"];
    ASSERT_EQ(gs.size(), 2u);
    for (auto& g : gs) {
        if (g["scope"] == "object") {
            EXPECT_EQ(g["id"], f.O1);
            EXPECT_EQ(g["name"], "O1");
            EXPECT_TRUE(g["can_command_group"].get<bool>());
            EXPECT_TRUE(g["can_move"].get<bool>());
            EXPECT_FALSE(g["can_view_audit"].get<bool>());
        } else {
            EXPECT_EQ(g["name"], "G21");
            EXPECT_EQ(g["object_name"], "O2");
            EXPECT_TRUE(g["can_view_audit"].get<bool>());
        }
    }
}

TEST(PlayerScopeRest, MovingNeedsTheMoveRightOnBothGroupsAndStaysInsideAnObject) {
    ScopeFixture f(kBasePort + 8);
    auto admin = f.person("root", sa::Role::Admin);
    auto mover = f.person("mover", sa::Role::Operator);
    auto plain = f.person("plain", sa::Role::Operator);
    auto view  = f.person("view", sa::Role::Viewer);
    ASSERT_EQ(f.putGrants(admin, mover.id, json::array({ScopeFixture::grant("group", f.G11, false, false, true),
                                                        ScopeFixture::grant("group", f.G12, false, false, true),
                                                        ScopeFixture::grant("group", f.G21, false, false, true)}))->status, 200);
    ASSERT_EQ(f.putGrants(admin, plain.id, json::array({ScopeFixture::grant("group", f.G11),
                                                        ScopeFixture::grant("group", f.G12)}))->status, 200);
    auto cli = f.client();
    auto move = [&](const ScopeFixture::Person& who, const std::string& pid, std::int64_t gid) {
        return cli.Put(("/api/players/" + pid + "/group").c_str(), bearer(who.token),
                       json{{"group_id", gid}}.dump(), "application/json");
    };

    EXPECT_EQ(move(view, f.P11, f.G12)->status, 403) << "a viewer never moves";
    EXPECT_EQ(move(plain, f.P11, f.G12)->status, 403) << "no move mark";
    EXPECT_EQ(move(mover, f.P11, f.G21)->status, 403) << "between objects: administrators only, even with every mark";
    EXPECT_EQ(move(mover, f.P21, f.G11)->status, 403);
    EXPECT_EQ(move(mover, f.P11, 424242)->status, 403) << "a group that does not exist looks the same as one he may not use";

    auto ok = move(mover, f.P11, f.G12);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200) << ok->body;
    EXPECT_EQ(json::parse(ok->body)["group_name"], "G12");

    EXPECT_EQ(move(admin, f.P11, f.G21)->status, 200) << "an administrator may cross objects";
    EXPECT_EQ(move(admin, f.P11, 424242)->status, 404) << "an administrator is told the truth";
}

TEST(PlayerScopeRest, ARightThatPointsAtARemovedGroupIsSweptAway) {
    ScopeFixture f(kBasePort + 9);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    auto g = f.pdb->createGroup(f.O2, "Temp");
    ASSERT_TRUE(g);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("group", g->id)}))->status, 200);

    auto cli = f.client();
    ASSERT_EQ(cli.Delete(("/api/player-groups/" + std::to_string(g->id)).c_str(), bearer(admin.token))->status, 200);
    auto r = cli.Get(("/api/auth/users/" + std::to_string(op.id) + "/player-grants").c_str(), bearer(admin.token));
    ASSERT_TRUE(r);
    EXPECT_TRUE(json::parse(r->body)["grants"].empty()) << "no dangling right waits for an id to come back";
}

TEST(PlayerScopeRest, ChangingRightsIsAudited) {
    ScopeFixture f(kBasePort + 10);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("object", f.O1, true)}))->status, 200);
    f.audit_logger->flushForTesting();
    liveqx::audit::AuditFilter af;
    af.action = "player.grants_update";
    af.limit = 4;
    auto rows = f.audit_db->list(af);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].category, liveqx::audit::Category::Player);
    EXPECT_EQ(rows[0].target_type, "user");
    EXPECT_EQ(rows[0].target_id, std::to_string(op.id));
    EXPECT_NE(rows[0].details_json.find("O1"), std::string::npos) << "the trail names what was granted";
}

TEST(PlayerScopeRest, PlayerManagementOutsideMovesStaysWithAdministrators) {
    ScopeFixture f(kBasePort + 11);
    auto admin = f.person("root", sa::Role::Admin);
    auto op    = f.person("op", sa::Role::Operator);
    ASSERT_EQ(f.putGrants(admin, op.id, json::array({ScopeFixture::grant("object", f.O1, true, true, true, true)}))->status, 200);
    auto cli = f.client();
    EXPECT_EQ(cli.Post("/api/players", bearer(op.token), json{{"name", "x"}, {"group_id", f.G11}}.dump(), "application/json")->status, 403);
    EXPECT_EQ(cli.Delete(("/api/players/" + f.P11).c_str(), bearer(op.token))->status, 403);
    EXPECT_EQ(cli.Post(("/api/players/" + f.P11 + "/revoke-cert").c_str(), bearer(op.token), "{}", "application/json")->status, 403);
    EXPECT_EQ(cli.Post(("/api/players/" + f.P11 + "/reissue-token").c_str(), bearer(op.token), "", "application/json")->status, 403);
    EXPECT_EQ(cli.Post("/api/player-objects", bearer(op.token), R"({"name":"x"})", "application/json")->status, 403);
    EXPECT_EQ(cli.Get("/api/players/settings", bearer(op.token))->status, 403);
}
