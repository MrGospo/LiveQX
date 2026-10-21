// REST: commands to a single screen (A7, A12, B13), with real accounts.
//
// What these tests protect:
//   * only a signed-in person with the command right on that screen sends a
//     command; a screen outside their rights is "not found", not "forbidden";
//     a viewer never commands, whatever they were granted;
//   * a second identical command to a screen that has not confirmed the first is
//     refused with the id of the one that holds the block;
//   * the log of a screen's commands is visible to whoever sees the screen, and
//     only an administrator lifts a block by hand;
//   * a broker that cannot take the command is a 503 and leaves no block;
//   * the answers are the agreed ones for bad input.

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
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
#include "players/CommandService.h"
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
constexpr int kBasePort = 19620;

httplib::Headers bearer(const std::string& token) { return {{"Authorization", "Bearer " + token}}; }

struct CommandFixture {
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
    std::unique_ptr<CommandService>     commands;
    ChannelManager                      manager;
    std::unique_ptr<ControlApi>         api;
    int port;

    std::mutex sent_mu;
    std::vector<liveqx::mqtt::Message> sent;
    std::atomic<bool> broker_up{true};

    std::int64_t O1{}, G11{}, G12{};
    std::string  P11, P12;

    explicit CommandFixture(int p) : manager(nullptr, fs::path{}), port(p) {
        static std::atomic<int> seq{0};
        tmp = fs::temp_directory_path() /
              ("lqx-cmdrest-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
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

        CommandServiceOptions co;
        co.publish = [this](const liveqx::mqtt::Message& m) {
            if (!broker_up) return liveqx::mqtt::PublishResult::NotConnected;
            std::lock_guard<std::mutex> lk(sent_mu);
            sent.push_back(m);
            return liveqx::mqtt::PublishResult::Ok;
        };
        commands = std::make_unique<CommandService>(*pdb, *players, std::move(co));

        O1 = pdb->createObject("O1")->id;
        G11 = pdb->createGroup(O1, "G11")->id;
        G12 = pdb->createGroup(O1, "G12")->id;
        P11 = activePlayer("screen-11", G11);
        P12 = activePlayer("screen-12", G12);

        api = std::make_unique<ControlApi>(
            port, manager, nullptr, LivezOptions{}, nullptr, svc.get(), nullptr, nullptr, rbac.get(),
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, TlsBindings{}, nullptr, nullptr, nullptr,
            audit_logger.get(), nullptr, players.get(), nullptr, commands.get());
        api->start();
        httplib::Client cli("127.0.0.1", port);
        for (int i = 0; i < 100; ++i) {
            auto r = cli.Get("/healthz");
            if (r && r->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "ControlApi never started on port " << port;
    }
    ~CommandFixture() {
        if (api) api->stop();
        api.reset();
        if (audit_logger) audit_logger->stop();
        audit_logger.reset();
        audit_db.reset();
        commands.reset();
        players.reset();
        pdb.reset();
        rbac.reset();
        svc.reset();
        authdb.reset();
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    std::string activePlayer(const std::string& name, std::int64_t group) {
        static std::atomic<int> n{0};
        const std::int64_t t = static_cast<std::int64_t>(std::time(nullptr));
        auto c = pdb->createPlayer(name, 1, "strict", 86400, t, group);
        auto claim = pdb->claimToken(c->player.id, c->token, t + 1);
        CertRow cert;
        cert.player_id = c->player.id; cert.serial_hex = "CC" + std::to_string(++n); cert.generation = 1;
        cert.issued_at = t; cert.not_before = t - 100; cert.not_after = t + 365 * 86400;
        EXPECT_TRUE(pdb->finishEnrollment(claim.token_id, cert, t + 2));
        return c->player.id;
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
    void give(const Person& admin, std::int64_t user_id, const json& grants) {
        auto cli = client();
        auto r = cli.Put(("/api/auth/users/" + std::to_string(user_id) + "/player-grants").c_str(),
                         bearer(admin.token), json{{"grants", grants}}.dump(), "application/json");
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200) << r->body;
    }
    static json groupAccess(std::int64_t id) {
        return {{"scope", "group"}, {"id", id}, {"can_command_group", false}, {"can_command_object", false},
                {"can_move", false}, {"can_view_audit", false}};
    }
    httplib::Result send(const Person& who, const std::string& player, const json& body) {
        auto cli = client();
        return cli.Post(("/api/players/" + player + "/command").c_str(), bearer(who.token),
                        body.dump(), "application/json");
    }
    void online(const std::string& id) {
        StatusRow st;
        st.player_id = id; st.updated_at = static_cast<std::int64_t>(std::time(nullptr)); st.online = true;
        st.version = "0.3.5";
        pdb->applyStatuses({st});
    }
    static json objectAccess(std::int64_t id, bool cg, bool co) {
        return {{"scope", "object"}, {"id", id}, {"can_command_group", cg}, {"can_command_object", co},
                {"can_move", false}, {"can_view_audit", false}};
    }
    static json groupMark(std::int64_t id, bool cg) {
        return {{"scope", "group"}, {"id", id}, {"can_command_group", cg}, {"can_command_object", false},
                {"can_move", false}, {"can_view_audit", false}};
    }
    httplib::Result post(const Person& who, const std::string& path, const json& body) {
        auto cli = client();
        return cli.Post(path.c_str(), bearer(who.token), body.dump(), "application/json");
    }
    std::size_t sentCount() { std::lock_guard<std::mutex> lk(sent_mu); return sent.size(); }
};

// ctest runs every test in its own process, in parallel: the port depends on the process.
std::atomic<int> g_port{kBasePort + static_cast<int>(::getpid() % 4000)};

}  // namespace

TEST(PlayerCommandRest, AnOperatorCommandsAScreenOfItsOwnGroupAndTheMessageGoesOut) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto op = f.person("op1", sa::Role::Operator);
    f.give(admin, op.id, json::array({CommandFixture::groupAccess(f.G11)}));

    auto r = f.send(op, f.P11, {{"type", "restart_player"}});
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 202) << r->body;
    const json j = json::parse(r->body);
    EXPECT_EQ(j["state"], "pending");
    EXPECT_EQ(j["type"], "restart_player");
    EXPECT_EQ(j["player_id"], f.P11);
    EXPECT_EQ(j["issued_by"], op.id);
    EXPECT_EQ(j["ttl_seconds"], 300);
    EXPECT_TRUE(j.contains("deadline_at"));
    ASSERT_EQ(f.sentCount(), 1u);
    EXPECT_EQ(f.sent[0].topic, "liveqx/v1/players/" + f.P11 + "/commands");
}

TEST(PlayerCommandRest, AScreenOutsideTheRightsIsNotFoundNotForbidden) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto op = f.person("op1", sa::Role::Operator);
    f.give(admin, op.id, json::array({CommandFixture::groupAccess(f.G11)}));
    auto r = f.send(op, f.P12, {{"type", "restart_player"}});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(f.sentCount(), 0u);

    auto nobody = f.person("op2", sa::Role::Operator);
    EXPECT_EQ(f.send(nobody, f.P11, {{"type", "restart_player"}})->status, 404) << "no rights, nothing visible";
}

TEST(PlayerCommandRest, AViewerNeverCommandsAndAnonymousCannotEither) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto viewer = f.person("view1", sa::Role::Viewer);
    f.give(admin, viewer.id, json::array({CommandFixture::groupAccess(f.G11)}));
    auto r = f.send(viewer, f.P11, {{"type", "restart_player"}});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);

    auto cli = f.client();
    auto anon = cli.Post(("/api/players/" + f.P11 + "/command").c_str(),
                         json{{"type", "restart_player"}}.dump(), "application/json");
    ASSERT_TRUE(anon);
    EXPECT_EQ(anon->status, 401);
    EXPECT_EQ(f.sentCount(), 0u);
}

TEST(PlayerCommandRest, ASecondIdenticalCommandIsRefusedWithTheIdThatHoldsTheBlock) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto first = f.send(admin, f.P11, {{"type", "reboot_screen"}});
    ASSERT_EQ(first->status, 202);
    auto again = f.send(admin, f.P11, {{"type", "reboot_screen"}});
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 409);
    const json e = json::parse(again->body);
    EXPECT_EQ(e["error"], "command_in_progress");
    EXPECT_EQ(e["command_id"], json::parse(first->body)["id"]);
    EXPECT_EQ(f.sentCount(), 1u);
    EXPECT_EQ(f.send(admin, f.P11, {{"type", "reload_playlist"}})->status, 202) << "another type goes through";
}

TEST(PlayerCommandRest, TheLogShowsWhoSeesTheScreenAndOnlyAnAdministratorReleases) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto op = f.person("op1", sa::Role::Operator);
    f.give(admin, op.id, json::array({CommandFixture::groupAccess(f.G11)}));
    auto sent = f.send(op, f.P11, {{"type", "restart_player"}});
    const std::string cid = json::parse(sent->body)["id"];

    auto cli = f.client();
    auto log = cli.Get(("/api/players/" + f.P11 + "/commands").c_str(), bearer(op.token));
    ASSERT_TRUE(log);
    ASSERT_EQ(log->status, 200) << log->body;
    const json arr = json::parse(log->body)["commands"];
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["id"], cid);
    EXPECT_EQ(arr[0]["state"], "pending");

    EXPECT_EQ(cli.Get(("/api/players/" + f.P12 + "/commands").c_str(), bearer(op.token))->status, 404)
        << "another group's log is not visible";

    auto by_op = cli.Post(("/api/players/" + f.P11 + "/commands/" + cid + "/release").c_str(),
                          bearer(op.token), "", "application/json");
    EXPECT_EQ(by_op->status, 403);
    auto by_admin = cli.Post(("/api/players/" + f.P11 + "/commands/" + cid + "/release").c_str(),
                             bearer(admin.token), "", "application/json");
    ASSERT_TRUE(by_admin);
    EXPECT_EQ(by_admin->status, 200) << by_admin->body;
    EXPECT_EQ(json::parse(by_admin->body)["state"], "released");
    auto twice = cli.Post(("/api/players/" + f.P11 + "/commands/" + cid + "/release").c_str(),
                          bearer(admin.token), "", "application/json");
    EXPECT_EQ(twice->status, 409);
    auto wrong = cli.Post(("/api/players/" + f.P12 + "/commands/" + cid + "/release").c_str(),
                          bearer(admin.token), "", "application/json");
    EXPECT_EQ(wrong->status, 404) << "the command is not that screen's";
    EXPECT_EQ(f.send(op, f.P11, {{"type", "restart_player"}})->status, 202) << "the block is gone";
}

TEST(PlayerCommandRest, TheScreensReportSettlesTheCommandInTheLog) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    const std::string cid = json::parse(f.send(admin, f.P11, {{"type", "restart_player"}})->body)["id"];
    f.commands->onResult(f.P11, cid, "executed", std::nullopt);
    auto cli = f.client();
    auto log = cli.Get(("/api/players/" + f.P11 + "/commands").c_str(), bearer(admin.token));
    const json arr = json::parse(log->body)["commands"];
    EXPECT_EQ(arr[0]["state"], "executed");
    EXPECT_TRUE(arr[0].contains("finished_at"));
}

TEST(PlayerCommandRest, ABrokerThatCannotTakeTheCommandIsA503AndLeavesNoBlock) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    f.broker_up = false;
    auto r = f.send(admin, f.P11, {{"type", "restart_player"}});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(json::parse(r->body)["error"], "broker_unavailable");
    f.broker_up = true;
    EXPECT_EQ(f.send(admin, f.P11, {{"type", "restart_player"}})->status, 202);
}

TEST(PlayerCommandRest, BadInputGetsTheAgreedAnswers) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    EXPECT_EQ(f.send(admin, f.P11, json::object())->status, 400);
    EXPECT_EQ(f.send(admin, f.P11, {{"type", 5}})->status, 400);
    auto unknown = f.send(admin, f.P11, {{"type", "format_disk"}});
    EXPECT_EQ(unknown->status, 400);
    EXPECT_EQ(json::parse(unknown->body)["error"], "unsupported_type");
    EXPECT_EQ(f.send(admin, "no-such-screen", {{"type", "restart_player"}})->status, 404);

    auto cli = f.client();
    auto bad = cli.Post(("/api/players/" + f.P11 + "/command").c_str(), bearer(admin.token),
                        "{not json", "application/json");
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(f.sentCount(), 0u);
}

// ── group, object, everyone ─────────────────────────────────────────────────
TEST(PlayerCommandRest, ADryRunTellsTheInterfaceThePhraseAndTheCountAndChangesNothing) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    f.online(f.P11);
    auto r = f.post(admin, "/api/player-groups/" + std::to_string(f.G11) + "/command",
                    {{"type", "reboot_screen"}, {"dry_run", true}});
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    const json j = json::parse(r->body);
    EXPECT_EQ(j["required_phrase"], "reboot_screen group O1 / G11");
    EXPECT_EQ(j["expected_count"], 1);
    EXPECT_EQ(j["online"], 1);
    EXPECT_EQ(f.sentCount(), 0u);
}

TEST(PlayerCommandRest, AGroupCommandNeedsThePhraseAndTheCountAndThenReachesTheConnectedScreens) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    f.online(f.P11);
    const std::string path = "/api/player-groups/" + std::to_string(f.G11) + "/command";

    auto no_phrase = f.post(admin, path, {{"type", "restart_player"}, {"expected_count", 1}});
    EXPECT_EQ(no_phrase->status, 400);
    EXPECT_EQ(json::parse(no_phrase->body)["error"], "confirm_phrase_mismatch");
    auto no_count = f.post(admin, path, {{"type", "restart_player"}, {"confirm_phrase", "restart_player group O1 / G11"}});
    EXPECT_EQ(no_count->status, 400);
    EXPECT_EQ(json::parse(no_count->body)["error"], "invalid_expected_count");
    auto stale = f.post(admin, path, {{"type", "restart_player"}, {"confirm_phrase", "restart_player group O1 / G11"},
                                      {"expected_count", 4}});
    EXPECT_EQ(stale->status, 409);
    EXPECT_EQ(json::parse(stale->body)["actual"], 1);
    EXPECT_EQ(f.sentCount(), 0u);

    auto ok = f.post(admin, path, {{"type", "restart_player"}, {"confirm_phrase", "restart_player group O1 / G11"},
                                   {"expected_count", 1}});
    ASSERT_TRUE(ok);
    ASSERT_EQ(ok->status, 200) << ok->body;
    const json j = json::parse(ok->body);
    ASSERT_EQ(j["sent"].size(), 1u);
    EXPECT_EQ(j["sent"][0]["player_id"], f.P11);
    EXPECT_TRUE(j["skipped_offline"].empty());
    EXPECT_EQ(f.sentCount(), 1u);

    auto again = f.post(admin, path, {{"type", "restart_player"}, {"confirm_phrase", "restart_player group O1 / G11"},
                                      {"expected_count", 1}});
    EXPECT_EQ(again->status, 409);
    const json e = json::parse(again->body);
    EXPECT_EQ(e["error"], "screens_busy");
    ASSERT_EQ(e["busy"].size(), 1u);
    EXPECT_EQ(e["busy"][0]["player_id"], f.P11);
}

TEST(PlayerCommandRest, ScreensThatAreNotConnectedAreSkippedAndListed) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto r = f.post(admin, "/api/player-objects/" + std::to_string(f.O1) + "/command",
                    {{"type", "reload_playlist"}, {"confirm_phrase", "reload_playlist object O1"}, {"expected_count", 2}});
    ASSERT_TRUE(r);
    // Nobody is connected: the count matched, nothing was sent, and the report says why.
    ASSERT_EQ(r->status, 200) << r->body;
    const json j = json::parse(r->body);
    EXPECT_TRUE(j["sent"].empty());
    EXPECT_EQ(j["skipped_offline"].size(), 2u);
    EXPECT_EQ(f.sentCount(), 0u);
}

TEST(PlayerCommandRest, TheGroupMarkDecidesWhoMayRebootAGroupButReloadNeedsOnlyAccess) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto plain = f.person("op-plain", sa::Role::Operator);
    auto marked = f.person("op-marked", sa::Role::Operator);
    f.give(admin, plain.id, json::array({CommandFixture::groupMark(f.G11, false)}));
    f.give(admin, marked.id, json::array({CommandFixture::groupMark(f.G11, true)}));
    f.online(f.P11);
    const std::string path = "/api/player-groups/" + std::to_string(f.G11) + "/command";

    EXPECT_EQ(f.post(plain, path, {{"type", "reboot_screen"}, {"dry_run", true}})->status, 403);
    EXPECT_EQ(f.post(plain, path, {{"type", "reload_playlist"}, {"dry_run", true}})->status, 200)
        << "reloading the playlist of a group is for anyone who may command its screens";
    EXPECT_EQ(f.post(marked, path, {{"type", "reboot_screen"}, {"dry_run", true}})->status, 200);

    auto done = f.post(marked, path, {{"type", "reboot_screen"},
                                      {"confirm_phrase", "reboot_screen group O1 / G11"}, {"expected_count", 1}});
    EXPECT_EQ(done->status, 200) << done->body;
    EXPECT_EQ(f.sentCount(), 1u);
}

TEST(PlayerCommandRest, AGroupOutsideTheRightsIsNotFound) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto op = f.person("op1", sa::Role::Operator);
    f.give(admin, op.id, json::array({CommandFixture::groupMark(f.G11, true)}));
    EXPECT_EQ(f.post(op, "/api/player-groups/" + std::to_string(f.G12) + "/command",
                     {{"type", "reload_playlist"}, {"dry_run", true}})->status, 404);
    EXPECT_EQ(f.post(op, "/api/player-groups/9999/command", {{"type", "reload_playlist"}, {"dry_run", true}})->status, 404);
}

TEST(PlayerCommandRest, AnObjectCommandNeedsTheObjectMarkAndAGroupRightDoesNotImplyIt) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto group_only = f.person("op-g", sa::Role::Operator);
    auto group_mark = f.person("op-gm", sa::Role::Operator);
    auto whole = f.person("op-o", sa::Role::Operator);
    f.give(admin, group_only.id, json::array({CommandFixture::groupMark(f.G11, false)}));
    f.give(admin, group_mark.id, json::array({CommandFixture::objectAccess(f.O1, true, false)}));
    f.give(admin, whole.id, json::array({CommandFixture::objectAccess(f.O1, false, true)}));
    const std::string path = "/api/player-objects/" + std::to_string(f.O1) + "/command";
    const json dry = {{"type", "restart_player"}, {"dry_run", true}};

    EXPECT_EQ(f.post(group_only, path, dry)->status, 403) << "the building's name is visible, its command is not";
    EXPECT_EQ(f.post(group_mark, path, dry)->status, 403) << "commanding every group one by one is not the object";
    EXPECT_EQ(f.post(whole, path, dry)->status, 200);
}

TEST(PlayerCommandRest, EveryoneIsForAdministratorsOnly) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto op = f.person("op1", sa::Role::Operator);
    f.give(admin, op.id, json::array({CommandFixture::objectAccess(f.O1, true, true)}));
    f.online(f.P11); f.online(f.P12);

    EXPECT_EQ(f.post(op, "/api/players/command-all", {{"type", "restart_player"}, {"dry_run", true}})->status, 403)
        << "even with every mark on the only object";
    auto dry = f.post(admin, "/api/players/command-all", {{"type", "restart_player"}, {"dry_run", true}});
    ASSERT_EQ(dry->status, 200);
    EXPECT_EQ(json::parse(dry->body)["required_phrase"], "restart_player all");
    auto go = f.post(admin, "/api/players/command-all",
                     {{"type", "restart_player"}, {"confirm_phrase", "restart_player all"}, {"expected_count", 2}});
    ASSERT_EQ(go->status, 200) << go->body;
    EXPECT_EQ(json::parse(go->body)["sent"].size(), 2u);
    EXPECT_EQ(f.sentCount(), 2u);
}

TEST(PlayerCommandRest, AViewerCannotCommandAGroupWhateverItHolds) {
    CommandFixture f(g_port++);
    auto admin = f.person("admin1", sa::Role::Admin);
    auto viewer = f.person("view1", sa::Role::Viewer);
    f.give(admin, viewer.id, json::array({CommandFixture::groupAccess(f.G11)}));
    EXPECT_EQ(f.post(viewer, "/api/player-groups/" + std::to_string(f.G11) + "/command",
                     {{"type", "reload_playlist"}, {"dry_run", true}})->status, 403);
}
