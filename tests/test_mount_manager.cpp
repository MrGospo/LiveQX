// fix41 commit 4 — MountManager orchestration tests.
//
// Uses a real MountsDb + MasterKey on a temp dir, and an in-process
// IMountdRpcClient mock that records every call and lets the test
// configure responses.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "auth/MasterKey.h"
#include "mounts/MountManager.h"
#include "mounts/MountSpec.h"
#include "mounts/MountdRpcClient.h"
#include "mounts/MountsDb.h"

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
using namespace liveqx::mounts;

namespace {

struct MockRpcClient : public IMountdRpcClient {
    struct ApplyCall  { MountSpec spec; };
    struct RemoveCall { std::int64_t id = 0; std::string target; };
    struct TestCall   { MountSpec spec; };
    struct StatusCall { std::vector<StatusQueryItem> items; };

    std::vector<ApplyCall>  apply_calls;
    std::vector<RemoveCall> remove_calls;
    std::vector<TestCall>   test_calls;
    std::vector<StatusCall> status_calls;

    RpcResponse apply_response =
        RpcResponse::okWith("active",
                            nlohmann::json{{"active_state", "active"}});
    RpcResponse remove_response = RpcResponse::okWith("removed");
    RpcResponse test_response   = RpcResponse::okWith("ok");
    RpcResponse status_response =
        RpcResponse::okWith("ok", nlohmann::json{{"units", nlohmann::json::array()}});

    RpcResponse applyMount(const MountSpec& s) override {
        apply_calls.push_back({s});
        return apply_response;
    }
    RpcResponse removeMount(std::int64_t id, std::string_view target) override {
        remove_calls.push_back({id, std::string(target)});
        return remove_response;
    }
    RpcResponse testMount(const MountSpec& s) override {
        test_calls.push_back({s});
        return test_response;
    }
    RpcResponse status(const std::vector<StatusQueryItem>& items) override {
        status_calls.push_back({items});
        return status_response;
    }
};

class MountManagerTest : public ::testing::Test {
protected:
    fs::path tmp_;
    std::unique_ptr<MountsDb> db_;
    std::unique_ptr<sa::MasterKey> mk_;
    std::shared_ptr<MockRpcClient> rpc_;
    std::unique_ptr<MountManager> mgr_;

    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
            ("mount_manager_test_" + std::to_string(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count())
             + "_" + ::testing::UnitTest::GetInstance()
                       ->current_test_info()->name());
        fs::create_directories(tmp_);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");

        db_ = std::make_unique<MountsDb>(tmp_ / "mounts.db");
        ASSERT_TRUE(db_->open());

        mk_ = std::make_unique<sa::MasterKey>((tmp_ / "master.key").string());
        ASSERT_TRUE(mk_->load());

        rpc_ = std::make_shared<MockRpcClient>();
        mgr_ = std::make_unique<MountManager>(*db_, *mk_, rpc_);
    }

    void TearDown() override {
        mgr_.reset();
        db_.reset();
        mk_.reset();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
    }

    static MountSpec makeCifs(std::string target,
                              std::string password = "s3cret",
                              std::string username = "alice") {
        MountSpec s;
        s.fs_type = FsType::Cifs;
        s.source  = "//srv/share";
        s.target  = std::move(target);
        s.options = "vers=3.0,iocharset=utf8";
        s.ro      = true;
        CifsCreds c;
        c.username = std::move(username);
        c.password = std::move(password);
        s.cifs = c;
        return s;
    }

    static MountSpec makeNfs(std::string target) {
        MountSpec s;
        s.fs_type = FsType::Nfs;
        s.source  = "srv:/export/lib";
        s.target  = std::move(target);
        s.options = "vers=4.1";
        s.ro      = true;
        return s;
    }
};

}  // namespace

TEST_F(MountManagerTest, AddMountInsertsAndCallsHelper) {
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GT(r.id, 0);
    EXPECT_EQ(rpc_->apply_calls.size(), 1u);
    EXPECT_EQ(rpc_->apply_calls.front().spec.id, r.id);
    EXPECT_EQ(rpc_->apply_calls.front().spec.cifs->password, "s3cret");

    auto stored = db_->findById(r.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_TRUE(stored->hasPassword());
    EXPECT_EQ(stored->cifs_username, "alice");
    EXPECT_EQ(stored->last_active_state, "active");
}

TEST_F(MountManagerTest, AddMountInvalidSpecRejected) {
    MountSpec bad;   // empty source/target — fails validate()
    auto r = mgr_->addMount(bad);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "invalid");
    EXPECT_TRUE(rpc_->apply_calls.empty());
}

TEST_F(MountManagerTest, AddMountDuplicateTargetRejected) {
    ASSERT_TRUE(mgr_->addMount(makeCifs("/mnt/liveqx/lib1")).ok);
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "duplicate");
    EXPECT_EQ(rpc_->apply_calls.size(), 1u);   // only the first one
}

TEST_F(MountManagerTest, AddMountRejectsTargetNestedUnderExisting) {
    // fix43: pre-fix43 history — оператор смог создать /mnt/liveqx
    // как mount, а потом /mnt/liveqx/Sozvezd1 как второй mount.
    // systemd авто-добавил Requires=mnt-liveqx.mount к
    // вложенному, тот пал с Dependency failed и mount ни разу не active.
    ASSERT_TRUE(mgr_->addMount(makeCifs("/mnt/liveqx")).ok);
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx/Sozvezd1"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "overlap");
    EXPECT_NE(r.error.find("nested"), std::string::npos);
    EXPECT_EQ(rpc_->apply_calls.size(), 1u);   // только первый дошёл
}

TEST_F(MountManagerTest, AddMountRejectsExistingNestedUnderNewTarget) {
    ASSERT_TRUE(mgr_->addMount(makeCifs("/mnt/liveqx/Sozvezd1")).ok);
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "overlap");
}

TEST_F(MountManagerTest, AddMountAllowsSiblingTargets) {
    // /mnt/liveqx/lib1 и /mnt/liveqx/lib1bis — не один
    // под другим, не nesting. Должны проходить (отказ был бы false-positive
    // если использовать starts_with без '/' boundary).
    ASSERT_TRUE(mgr_->addMount(makeCifs("/mnt/liveqx/lib1")).ok);
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx/lib1bis"));
    EXPECT_TRUE(r.ok) << r.error;
}

TEST_F(MountManagerTest, UpdateRejectsTargetThatBecomesOverlapping) {
    ASSERT_TRUE(mgr_->addMount(makeCifs("/mnt/liveqx/lib1")).ok);
    auto add2 = mgr_->addMount(makeCifs("/mnt/liveqx/lib2"));
    ASSERT_TRUE(add2.ok);
    // Сдвигаем lib2 → /mnt/liveqx/lib1/sub → конфликт с lib1.
    auto bad = makeCifs("/mnt/liveqx/lib1/sub");
    auto u = mgr_->updateMount(add2.id, bad);
    EXPECT_FALSE(u.ok);
    EXPECT_EQ(u.error_code, "overlap");
}

TEST_F(MountManagerTest, UpdateAllowsKeepingSameTarget) {
    // Self-overlap не должен случиться: update с тем же target проходит.
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    auto sp = makeCifs("/mnt/liveqx/lib1");
    sp.options = "vers=3.0,iocharset=utf8,rsize=131072";
    auto u = mgr_->updateMount(add.id, sp);
    EXPECT_TRUE(u.ok) << u.error;
}

TEST_F(MountManagerTest, UpdateTargetChangeRemovesOldUnitsBeforeApply) {
    // fix43 c4: при смене target прежний basename юнитов (target-derived)
    // больше не соответствует. Если не снять — старые .mount/.automount
    // останутся в /etc/systemd/system и продолжат удерживать старый target.
    // Manager должен вызвать removeMount(id, OLD target) до applyMount.
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    rpc_->remove_calls.clear();
    rpc_->apply_calls.clear();

    auto sp = makeCifs("/mnt/liveqx/lib1-renamed");
    auto u  = mgr_->updateMount(add.id, sp);
    EXPECT_TRUE(u.ok) << u.error;

    // removeMount передан со СТАРЫМ target'ом и тем же id.
    ASSERT_EQ(rpc_->remove_calls.size(), 1u);
    EXPECT_EQ(rpc_->remove_calls.front().id,     add.id);
    EXPECT_EQ(rpc_->remove_calls.front().target, "/mnt/liveqx/lib1");

    // applyMount передан с НОВЫМ target'ом.
    ASSERT_EQ(rpc_->apply_calls.size(), 1u);
    EXPECT_EQ(rpc_->apply_calls.front().spec.target, "/mnt/liveqx/lib1-renamed");
}

TEST_F(MountManagerTest, UpdateSameTargetDoesNotCallRemove) {
    // Если target не меняется — removeMount не вызываем (мы только
    // переписываем cred / опции). Лишний disable--now поломал бы и без
    // того уже-активный mount без необходимости.
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    rpc_->remove_calls.clear();

    auto sp = makeCifs("/mnt/liveqx/lib1");
    sp.options = "vers=3.0,iocharset=utf8,rsize=131072";
    auto u = mgr_->updateMount(add.id, sp);
    EXPECT_TRUE(u.ok) << u.error;
    EXPECT_TRUE(rpc_->remove_calls.empty());
}

TEST_F(MountManagerTest, UpdateTargetChangeContinuesIfRemoveFails) {
    // removeMount best-effort: если helper не смог снять старый юнит,
    // applyMount всё равно идёт — новые юниты должны лечь, оператор
    // увидит ok=true. Старый юнит в худшем случае пересоберётся при
    // следующем daemon-reload или останется как orphan, что лечится
    // вручную и не блокирует пользователя.
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    rpc_->remove_response = RpcResponse::fail("mountd: disable timed out",
                                              "systemd_failed");
    rpc_->remove_calls.clear();
    rpc_->apply_calls.clear();

    auto u = mgr_->updateMount(add.id,
                               makeCifs("/mnt/liveqx/lib1-new"));
    EXPECT_TRUE(u.ok) << u.error;
    EXPECT_EQ(rpc_->remove_calls.size(), 1u);
    EXPECT_EQ(rpc_->apply_calls.size(),  1u);
}

TEST_F(MountManagerTest, AddMountHelperFailureMarksRpcButRowPersists) {
    rpc_->apply_response =
        RpcResponse::fail("mountd: unit failed to start", "failed");
    auto r = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "rpc");
    EXPECT_NE(r.error.find("helper error"), std::string::npos);
    // Row is in DB so the operator can retry from the UI.
    EXPECT_EQ(db_->listAll().size(), 1u);
}

TEST_F(MountManagerTest, UpdateKeepsExistingPasswordWhenEmpty) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1", "first"));
    ASSERT_TRUE(add.ok);
    auto orig = db_->findById(add.id).value();
    auto orig_blob = orig.cifs_password_blob;
    ASSERT_FALSE(orig_blob.empty());

    // Edit-flow with empty password = "leave it alone".
    auto upd_spec = makeCifs("/mnt/liveqx/lib1", /*pw=*/"");
    upd_spec.options = "vers=3.0,iocharset=utf8,rsize=131072";
    auto u = mgr_->updateMount(add.id, upd_spec);
    EXPECT_TRUE(u.ok) << u.error;

    auto stored = db_->findById(add.id).value();
    EXPECT_EQ(stored.cifs_password_blob, orig_blob);
    EXPECT_EQ(stored.options, upd_spec.options);

    // Helper should have received a non-empty password — decrypted from
    // the stored blob.
    ASSERT_EQ(rpc_->apply_calls.size(), 2u);   // [0] add, [1] update
    EXPECT_EQ(rpc_->apply_calls.back().spec.cifs->password, "first");
}

TEST_F(MountManagerTest, UpdateReencryptsWhenNewPasswordProvided) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1", "first"));
    auto orig_blob = db_->findById(add.id)->cifs_password_blob;

    auto upd = mgr_->updateMount(add.id,
                                 makeCifs("/mnt/liveqx/lib1", "second"));
    EXPECT_TRUE(upd.ok);

    auto stored = db_->findById(add.id).value();
    EXPECT_NE(stored.cifs_password_blob, orig_blob);
    auto pt = mk_->decrypt(stored.cifs_password_blob);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, "second");
    EXPECT_EQ(rpc_->apply_calls.back().spec.cifs->password, "second");
}

TEST_F(MountManagerTest, UpdateCifsToNfsWipesCreds) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    auto upd = mgr_->updateMount(add.id, makeNfs("/mnt/liveqx/lib1"));
    EXPECT_TRUE(upd.ok);
    auto stored = db_->findById(add.id).value();
    EXPECT_FALSE(stored.hasPassword());
    EXPECT_TRUE(stored.cifs_username.empty());
    EXPECT_TRUE(stored.cifs_domain.empty());
    EXPECT_EQ(stored.fs_type, "nfs");
}

TEST_F(MountManagerTest, RemoveMountCallsHelperAndDeletes) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    auto r = mgr_->removeMount(add.id);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(rpc_->remove_calls.size(), 1u);
    EXPECT_EQ(rpc_->remove_calls.front().id, add.id);
    EXPECT_FALSE(db_->findById(add.id).has_value());
}

TEST_F(MountManagerTest, RemoveMountIsIdempotentOnMissing) {
    auto r = mgr_->removeMount(999);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(rpc_->remove_calls.empty());
}

TEST_F(MountManagerTest, RemoveMountStillDeletesRowIfHelperFails) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    rpc_->remove_response = RpcResponse::fail("mountd: unit not found");
    auto r = mgr_->removeMount(add.id);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(db_->findById(add.id).has_value());
}

TEST_F(MountManagerTest, TestMountForwardsToHelperWithoutDbWrite) {
    auto r = mgr_->testMount(makeCifs("/mnt/liveqx/probe"));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(rpc_->test_calls.size(), 1u);
    EXPECT_TRUE(db_->listAll().empty());
}

TEST_F(MountManagerTest, ListAllExcludesPasswordBlob) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    ASSERT_TRUE(add.ok);
    auto rows = mgr_->listAll();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().id, add.id);
    EXPECT_TRUE(rows.front().has_password);
    EXPECT_EQ(rows.front().cifs_username, "alice");
}

TEST_F(MountManagerTest, GetDecryptedSpecRoundTrip) {
    auto add = mgr_->addMount(makeCifs("/mnt/liveqx/lib1", "secret"));
    ASSERT_TRUE(add.ok);
    auto spec = mgr_->getDecryptedSpec(add.id);
    ASSERT_TRUE(spec.has_value());
    ASSERT_TRUE(spec->cifs.has_value());
    EXPECT_EQ(spec->cifs->password, "secret");
    EXPECT_EQ(spec->cifs->username, "alice");
}

TEST_F(MountManagerTest, ReconcileOnBootAppliesEnabledRows) {
    auto add1 = mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    auto add2 = mgr_->addMount(makeNfs("/mnt/liveqx/lib2"));
    ASSERT_TRUE(add1.ok) << add1.error;
    ASSERT_TRUE(add2.ok) << add2.error;

    // Disable lib2 so reconcile should skip it.
    auto row2 = db_->findById(add2.id).value();
    row2.enabled = false;
    ASSERT_TRUE(db_->update(row2));

    rpc_->apply_calls.clear();
    mgr_->reconcileOnBoot();
    EXPECT_EQ(rpc_->apply_calls.size(), 1u);
    EXPECT_EQ(rpc_->apply_calls.front().spec.id, add1.id);
}

TEST_F(MountManagerTest, SyncStatusFromHelperMergesIntoDb) {
    auto a = mgr_->addMount(makeCifs("/mnt/liveqx/lib1")).id;
    auto b = mgr_->addMount(makeCifs("/mnt/liveqx/lib2")).id;

    nlohmann::json units = nlohmann::json::array();
    units.push_back({{"id", a}, {"active_state", "active"}});
    units.push_back({{"id", b}, {"active_state", "failed"}});
    rpc_->status_response = RpcResponse::okWith("ok",
        nlohmann::json{{"units", units}});

    mgr_->syncStatusFromHelper();
    EXPECT_EQ(db_->findById(a)->last_active_state, "active");
    EXPECT_EQ(db_->findById(b)->last_active_state, "failed");
}

TEST_F(MountManagerTest, SyncStatusIgnoresUnknownIds) {
    mgr_->addMount(makeCifs("/mnt/liveqx/lib1"));
    nlohmann::json units = nlohmann::json::array();
    units.push_back({{"id", 9999}, {"active_state", "active"}});
    rpc_->status_response = RpcResponse::okWith("ok",
        nlohmann::json{{"units", units}});
    // Should not throw / crash; updateStatus on a missing id is a no-op.
    mgr_->syncStatusFromHelper();
}
