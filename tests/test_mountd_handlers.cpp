// fix41 commit 3 — MountdHandlers: проверяем оркестрацию через
// mock-объекты для ISystemctl / ICredsHelper / IMountTester.
// fix42 — basename выводится из target (systemd-escape), Status принимает
// {id,target} от клиента, Remove — (id, target).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "mounts/MountdHandlers.h"

using namespace liveqx::mounts;
namespace fs = std::filesystem;

namespace {

fs::path uniqueDir(const std::string& tag) {
    auto p = fs::temp_directory_path() /
        ("sc-handlers-" + tag + "-" + std::to_string(::getpid())
         + "-" + std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

struct MockSystemctl : ISystemctl {
    std::vector<std::string> calls;        // "daemon-reload", "enable:foo", etc.
    std::string              return_state = "active";
    std::unordered_map<std::string, UnitState> unit_states;
    bool                     fail_reload  = false;
    bool                     fail_enable  = false;
    bool                     fail_disable = false;
    bool                     fail_start   = false;
    bool                     fail_reset   = false;
    std::string              start_err    = "start-fail";

    bool daemonReload(std::string& err) override {
        calls.emplace_back("daemon-reload");
        if (fail_reload) { err = "reload-fail"; return false; }
        return true;
    }
    bool enableNow(std::string_view unit, std::string& err) override {
        calls.emplace_back("enable:" + std::string(unit));
        if (fail_enable) { err = "enable-fail"; return false; }
        return true;
    }
    bool disableNow(std::string_view unit, std::string& err) override {
        calls.emplace_back("disable:" + std::string(unit));
        if (fail_disable) { err = "disable-fail"; return false; }
        return true;
    }
    bool start(std::string_view unit, std::string& err) override {
        calls.emplace_back("start:" + std::string(unit));
        if (fail_start) { err = start_err; return false; }
        return true;
    }
    bool resetFailed(std::string_view unit, std::string& err) override {
        calls.emplace_back("reset-failed:" + std::string(unit));
        if (fail_reset) { err = "reset-fail"; return false; }
        return true;
    }
    std::string activeState(std::string_view) override {
        return return_state;
    }
    UnitState unitState(std::string_view unit) override {
        auto it = unit_states.find(std::string(unit));
        if (it == unit_states.end()) return UnitState{};
        return it->second;
    }
};

struct MockCreds : ICredsHelper {
    // (payload, out_path) for each writeFile call.
    std::vector<std::pair<std::string, fs::path>> writes;
    std::vector<fs::path> removes;
    bool fail_write = false;

    bool writeFile(const std::string& payload,
                   const fs::path& out,
                   std::string& err) override {
        writes.emplace_back(payload, out);
        if (fail_write) { err = "write-fail"; return false; }
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream(out.string()) << "cred-blob";
        return true;
    }
    bool removeFile(const fs::path& p, std::string&) override {
        removes.push_back(p);
        std::error_code ec;
        fs::remove(p, ec);
        return true;
    }
};

struct MockTester : IMountTester {
    bool ok = true;
    std::string err = "";
    std::int64_t files = 42;
    int duration_ms = 123;
    int calls = 0;

    TestMountResult test(const MountSpec&) override {
        ++calls;
        TestMountResult r;
        r.ok = ok;
        r.error = err;
        r.files = files;
        r.duration_ms = duration_ms;
        return r;
    }
};

MountSpec cifsSpec(std::int64_t id, const fs::path& target_root) {
    MountSpec s;
    s.id      = id;
    s.fs_type = FsType::Cifs;
    s.source  = "//srv/share";
    s.target  = (target_root / ("mnt-" + std::to_string(id))).string();
    s.options = "vers=3.0";
    s.ro      = true;
    s.cifs    = CifsCreds{"user", "pw", ""};
    return s;
}

}  // namespace

TEST(MountdHandlers, ApplyWritesUnitsAndEnables) {
    auto root = uniqueDir("apply");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(7, root);
    const auto base = unitBasenameByTarget(spec.target);
    auto resp = h.handleApply(spec);
    EXPECT_TRUE(resp.ok) << resp.error;
    EXPECT_EQ(resp.status, "active");
    EXPECT_EQ(resp.extra.value("active_state", ""), "active");

    EXPECT_TRUE(fs::exists(unit_dir / (base + ".mount")));
    EXPECT_TRUE(fs::exists(unit_dir / (base + ".automount")));
    // Cred — id-based на cred_dir.
    EXPECT_TRUE(fs::exists(cred_dir / "liveqx-mnt-7.cred"));

    // creds сначала, daemon-reload вторым, reset-failed для .mount и
    // .automount (fix44: чистим память systemd о прошлом сбое до start'а),
    // enable --now четвёртым, eager-start последним — чтобы юнит, который
    // запишет daemon-reload, уже ссылался на готовый cred-файл, чтобы
    // прошлый failed-state не блокировал старт, и чтобы .mount поднялся
    // сразу (а не только при первом доступе через .automount).
    ASSERT_GE(sys->calls.size(), 5u);
    EXPECT_EQ(sys->calls[0], "daemon-reload");
    EXPECT_EQ(sys->calls[1], "reset-failed:" + base + ".mount");
    EXPECT_EQ(sys->calls[2], "reset-failed:" + base + ".automount");
    EXPECT_EQ(sys->calls[3], "enable:" + base + ".automount");
    EXPECT_EQ(sys->calls[4], "start:"  + base + ".mount");
    ASSERT_EQ(creds->writes.size(), 1u);
    EXPECT_EQ(creds->writes[0].second,
              cred_dir / "liveqx-mnt-7.cred");
    EXPECT_NE(creds->writes[0].first.find("password=pw"), std::string::npos);
}

TEST(MountdHandlers, ApplyEagerStartFailurePropagatesMountError) {
    auto root = uniqueDir("apply-eager-fail");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";

    auto sys   = std::make_shared<MockSystemctl>();
    sys->fail_start = true;
    sys->start_err  = "exit=32: mount error(13): Permission denied";
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleApply(cifsSpec(7, root));
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "mount_failed");
    // Полный stderr mount'а попадает клиенту — иначе UI не покажет
    // оператору, почему монтирование сорвалось.
    EXPECT_NE(resp.error.find("Permission denied"), std::string::npos);
}

TEST(MountdHandlers, ApplyResetsFailedStateBeforeEnable) {
    // fix44: после неудачного apply в памяти systemd остаётся "failed".
    // Повторный apply обязан сбросить failed-state ДО enable --now,
    // иначе systemd откажется поднимать юнит с прилипшим failed-флагом,
    // а оператор не сможет переподнять монтирование через UI без
    // рестарта liveqx-mountd.
    auto root = uniqueDir("apply-reset");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(13, root);
    const auto base = unitBasenameByTarget(spec.target);

    auto resp = h.handleApply(spec);
    EXPECT_TRUE(resp.ok) << resp.error;

    // Оба юнита (.mount и .automount) должны быть сброшены, причём ДО
    // enable. Иначе сценарий «второй раз после провала» не лечится.
    auto idx = [&](const std::string& token) -> int {
        for (size_t i = 0; i < sys->calls.size(); ++i) {
            if (sys->calls[i] == token) return static_cast<int>(i);
        }
        return -1;
    };
    const int reset_mount = idx("reset-failed:" + base + ".mount");
    const int reset_auto  = idx("reset-failed:" + base + ".automount");
    const int enable_auto = idx("enable:"       + base + ".automount");
    ASSERT_GE(reset_mount, 0);
    ASSERT_GE(reset_auto,  0);
    ASSERT_GE(enable_auto, 0);
    EXPECT_LT(reset_mount, enable_auto);
    EXPECT_LT(reset_auto,  enable_auto);
}

TEST(MountdHandlers, ApplySucceedsEvenIfResetFailedErrors) {
    // fix44: reset-failed best-effort — если он по какой-то причине упал
    // (новая версия systemd, странный exit-code), apply не должен из-за
    // этого валиться: рабочее enable --now сразу даст оператору
    // настоящую mount-ошибку, а warn в логе хватит для диагностики.
    auto root = uniqueDir("apply-reset-soft");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";

    auto sys   = std::make_shared<MockSystemctl>();
    sys->fail_reset = true;
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleApply(cifsSpec(17, root));
    EXPECT_TRUE(resp.ok) << resp.error;
}

TEST(MountdHandlers, ApplyWithoutAutomountSkipsExtraStart) {
    // generate_automount=false: enable --now <basename>.mount уже
    // запускает .mount сам; второй start не нужен.
    auto root = uniqueDir("apply-no-auto");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir       = unit_dir;
    cfg.unit_ctx.cred_dir               = cred_dir;
    cfg.unit_ctx.generate_automount     = false;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(7, root);
    auto resp = h.handleApply(spec);
    EXPECT_TRUE(resp.ok) << resp.error;
    const auto base = unitBasenameByTarget(spec.target);
    EXPECT_EQ(resp.extra.value("automount", true), false);
    // Не должно быть отдельного "start:<base>.mount" — enable --now уже
    // стартанул .mount.
    for (auto& c : sys->calls) {
        EXPECT_NE(c, "start:" + base + ".mount");
    }
}

TEST(MountdHandlers, ApplyNfsSkipsCreds) {
    auto root = uniqueDir("apply-nfs");
    MountSpec s;
    s.id = 11;
    s.fs_type = FsType::Nfs;
    s.source = "nas:/export";
    s.target = (root / "mnt-11").string();
    s.options = "vers=4";
    s.ro = true;

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleApply(s);
    EXPECT_TRUE(resp.ok) << resp.error;
    // NFS не пишет реальный creds-blob, только опциональный removeFile cleanup.
    EXPECT_TRUE(creds->writes.empty());
}

TEST(MountdHandlers, ApplyRefusesIdZero) {
    auto root = uniqueDir("apply-id0");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(0, root);
    auto resp = h.handleApply(spec);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "invalid");
}

TEST(MountdHandlers, ApplyPropagatesCredsFailure) {
    auto root = uniqueDir("apply-creds-fail");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    creds->fail_write = true;
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleApply(cifsSpec(5, root));
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "creds_failed");
    EXPECT_TRUE(sys->calls.empty()) << "must not reload after creds failure";
}

TEST(MountdHandlers, ApplyMigratesLegacyIdBasedUnit) {
    auto root = uniqueDir("apply-migrate");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";
    fs::create_directories(unit_dir);
    fs::create_directories(cred_dir);

    // Имитируем pre-fix42 файлы для id=42.
    const auto legacy_base = legacyUnitBasename(42);
    std::ofstream(unit_dir / (legacy_base + ".mount"))     << "stale";
    std::ofstream(unit_dir / (legacy_base + ".automount")) << "stale";
    std::ofstream(cred_dir / (legacy_base + ".cred"))      << "stale";

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(42, root);
    auto resp = h.handleApply(spec);
    EXPECT_TRUE(resp.ok) << resp.error;

    // Legacy файлы исчезли.
    EXPECT_FALSE(fs::exists(unit_dir / (legacy_base + ".mount")));
    EXPECT_FALSE(fs::exists(unit_dir / (legacy_base + ".automount")));
    // Cred-файл id-based — после миграции мы переписали его новым blob'ом.
    // (handleApply unlink'нет legacy_cred, потом encryptToFile запишет
    // тот же id-based путь — это OK, итоговое содержимое корректное.)
    EXPECT_TRUE(fs::exists(cred_dir / (credFilename(42) + ".cred")));

    // Новые (target-derived) файлы появились.
    const auto new_base = unitBasenameByTarget(spec.target);
    EXPECT_NE(new_base, legacy_base);
    EXPECT_TRUE(fs::exists(unit_dir / (new_base + ".mount")));
    EXPECT_TRUE(fs::exists(unit_dir / (new_base + ".automount")));

    // disable legacy юнитов был вызван.
    bool had_legacy_disable = false;
    for (auto& c : sys->calls) {
        if (c == "disable:" + legacy_base + ".mount") {
            had_legacy_disable = true;
            break;
        }
    }
    EXPECT_TRUE(had_legacy_disable);
}

TEST(MountdHandlers, RemoveDisablesAndUnlinks) {
    auto root = uniqueDir("remove");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";
    fs::create_directories(unit_dir);
    fs::create_directories(cred_dir);

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(7, root);
    const auto base = unitBasenameByTarget(spec.target);

    // Готовим существующие файлы под target-derived basename.
    std::ofstream(unit_dir / (base + ".mount"))     << "x";
    std::ofstream(unit_dir / (base + ".automount")) << "y";
    std::ofstream(cred_dir / "liveqx-mnt-7.cred") << "z";

    auto resp = h.handleRemove(7, spec.target);
    EXPECT_TRUE(resp.ok) << resp.error;

    EXPECT_FALSE(fs::exists(unit_dir / (base + ".mount")));
    EXPECT_FALSE(fs::exists(unit_dir / (base + ".automount")));
    EXPECT_FALSE(fs::exists(cred_dir / "liveqx-mnt-7.cred"));

    bool had_disable_automount = false;
    bool had_disable_mount     = false;
    bool had_reload            = false;
    for (auto& c : sys->calls) {
        if (c == "disable:" + base + ".automount") had_disable_automount = true;
        if (c == "disable:" + base + ".mount")     had_disable_mount     = true;
        if (c == "daemon-reload")                   had_reload            = true;
    }
    EXPECT_TRUE(had_disable_automount);
    EXPECT_TRUE(had_disable_mount);
    EXPECT_TRUE(had_reload);
}

TEST(MountdHandlers, RemoveCleansLegacyToo) {
    // Если на диске лежит pre-fix42 .mount под id-based basename'ом —
    // remove должен снести и его тоже.
    auto root = uniqueDir("remove-legacy");
    auto unit_dir = root / "systemd";
    auto cred_dir = root / "creds";
    fs::create_directories(unit_dir);
    fs::create_directories(cred_dir);

    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = unit_dir;
    cfg.unit_ctx.cred_dir         = cred_dir;
    MountdHandlers h(cfg, sys, creds, t);

    auto spec = cifsSpec(7, root);
    const auto legacy = legacyUnitBasename(7);
    std::ofstream(unit_dir / (legacy + ".mount")) << "legacy";

    auto resp = h.handleRemove(7, spec.target);
    EXPECT_TRUE(resp.ok) << resp.error;
    EXPECT_FALSE(fs::exists(unit_dir / (legacy + ".mount")));
}

TEST(MountdHandlers, RemoveRefusesIdZero) {
    auto root = uniqueDir("rm-id0");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();
    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);
    auto resp = h.handleRemove(0, "/mnt/foo");
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "invalid");
}

TEST(MountdHandlers, RemoveRefusesEmptyTarget) {
    auto root = uniqueDir("rm-empty");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();
    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);
    auto resp = h.handleRemove(7, "");
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.status, "invalid");
}

TEST(MountdHandlers, TestDelegatesToTester) {
    auto root = uniqueDir("test");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();
    t->ok    = true;
    t->files = 17;
    t->duration_ms = 250;

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleTest(cifsSpec(0, root));
    EXPECT_TRUE(resp.ok);
    EXPECT_EQ(t->calls, 1);
    EXPECT_EQ(resp.extra.value("files", 0),       17);
    EXPECT_EQ(resp.extra.value("duration_ms", 0), 250);
    // Test НЕ должен трогать systemd / writeUnitFile.
    EXPECT_TRUE(sys->calls.empty());
    EXPECT_TRUE(creds->writes.empty());
}

TEST(MountdHandlers, TestPropagatesFailure) {
    auto root = uniqueDir("test-fail");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();
    t->ok = false;
    t->err = "no route to host";

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto resp = h.handleTest(cifsSpec(0, root));
    EXPECT_FALSE(resp.ok);
    EXPECT_NE(resp.error.find("no route"), std::string::npos);
}

TEST(MountdHandlers, StatusQueriesByTarget) {
    auto root = uniqueDir("status");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    // Готовим ответы по target-derived именам.
    auto s1 = cifsSpec(1, root);
    auto s2 = cifsSpec(2, root);
    const auto b1 = unitBasenameByTarget(s1.target);
    const auto b2 = unitBasenameByTarget(s2.target);
    sys->unit_states[b1 + ".mount"] = {0, "loaded", "active",   "mounted"};
    sys->unit_states[b2 + ".mount"] = {0, "loaded", "inactive", "dead"};

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    std::vector<MountdHandlers::StatusItem> items{
        {1, s1.target},
        {2, s2.target},
    };
    auto resp = h.handleStatus(items);
    EXPECT_TRUE(resp.ok);
    ASSERT_TRUE(resp.extra.contains("units"));
    EXPECT_EQ(resp.extra["units"].size(), 2u);
    EXPECT_EQ(resp.extra["units"][0]["id"], 1);
    EXPECT_EQ(resp.extra["units"][0]["active_state"], "active");
    EXPECT_EQ(resp.extra["units"][1]["id"], 2);
    EXPECT_EQ(resp.extra["units"][1]["active_state"], "inactive");
}

TEST(MountdHandlers, AsRpcHandlerDispatchesAllOps) {
    auto root = uniqueDir("dispatch");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();
    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);
    auto handler = h.asRpcHandler();

    {
        // Status — items может быть пустым.
        auto r = handler(RpcOp::Status,
                         nlohmann::json{{"items", nlohmann::json::array()}});
        EXPECT_TRUE(r.ok);
    }
    auto spec = cifsSpec(11, root);
    {
        auto r = handler(RpcOp::ApplyMount,
                         nlohmann::json{{"spec", spec.toJson(true)}});
        EXPECT_TRUE(r.ok) << r.error;
    }
    {
        // RemoveMount теперь требует target.
        auto r = handler(RpcOp::RemoveMount,
                         nlohmann::json{{"id", 11}, {"target", spec.target}});
        EXPECT_TRUE(r.ok);
    }
    {
        auto sp = cifsSpec(0, root);
        auto r = handler(RpcOp::TestMount,
                         nlohmann::json{{"spec", sp.toJson(true)}});
        EXPECT_TRUE(r.ok);
    }
    {
        // RemoveMount без target → ошибка validation.
        auto r = handler(RpcOp::RemoveMount, nlohmann::json{{"id", 11}});
        EXPECT_FALSE(r.ok);
    }
}

TEST(MountdHandlers, ApplyAtomicityAcrossRewriteOfSameId) {
    // Apply дважды с одним id: вторая запись должна перезаписать первую,
    // не оставив .tmp хвоста.
    auto root = uniqueDir("rewrite");
    auto sys   = std::make_shared<MockSystemctl>();
    auto creds = std::make_shared<MockCreds>();
    auto t     = std::make_shared<MockTester>();

    MountdHandlersConfig cfg;
    cfg.unit_ctx.systemd_unit_dir = root / "systemd";
    cfg.unit_ctx.cred_dir         = root / "creds";
    MountdHandlers h(cfg, sys, creds, t);

    auto a = cifsSpec(42, root);
    a.options = "vers=3.0";
    EXPECT_TRUE(h.handleApply(a).ok);

    auto b = cifsSpec(42, root);
    b.options = "vers=3.1.1";
    EXPECT_TRUE(h.handleApply(b).ok);

    const auto base = unitBasenameByTarget(a.target);
    std::string contents;
    {
        std::ifstream f((root / "systemd" / (base + ".mount")).string());
        std::string s((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        contents = s;
    }
    EXPECT_NE(contents.find("vers=3.1.1"), std::string::npos);
    EXPECT_FALSE(fs::exists(
        root / "systemd" / (base + ".mount.tmp")));
}
