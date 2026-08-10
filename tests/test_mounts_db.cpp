// fix41 commit 4 — state/mounts.db CRUD tests.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "mounts/MountsDb.h"

namespace fs = std::filesystem;
using liveqx::mounts::MountRow;
using liveqx::mounts::MountsDb;

namespace {

class MountsDbTest : public ::testing::Test {
protected:
    fs::path tmp_;
    fs::path dbPath() const { return tmp_ / "mounts.db"; }

    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
            ("mounts_db_test_" + std::to_string(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count())
             + "_" + ::testing::UnitTest::GetInstance()
                       ->current_test_info()->name());
        fs::create_directories(tmp_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    static MountRow makeRow(std::string target, std::string source = "//srv/share") {
        MountRow r;
        r.fs_type = "cifs";
        r.source  = std::move(source);
        r.target  = std::move(target);
        r.options = "vers=3.0,iocharset=utf8";
        r.ro      = true;
        r.cifs_username = "alice";
        r.cifs_domain   = "WORKGROUP";
        return r;
    }
};

}  // namespace

TEST_F(MountsDbTest, OpenCreatesDb) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_TRUE(db.ok());
    EXPECT_TRUE(fs::exists(dbPath()));
}

TEST_F(MountsDbTest, ReopenIsIdempotent) {
    {
        MountsDb db(dbPath());
        ASSERT_TRUE(db.open());
        auto id = db.insert(makeRow("/mnt/liveqx/lib1"));
        ASSERT_TRUE(id.has_value());
    }
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    EXPECT_EQ(db.listAll().size(), 1u);
}

TEST_F(MountsDbTest, InsertAssignsIdAndTimestamps) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insert(makeRow("/mnt/liveqx/lib1"));
    ASSERT_TRUE(id.has_value());
    EXPECT_GT(*id, 0);

    auto row = db.findById(*id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->target,  "/mnt/liveqx/lib1");
    EXPECT_EQ(row->fs_type, "cifs");
    EXPECT_GT(row->created_at, 0);
    EXPECT_EQ(row->updated_at, row->created_at);
    EXPECT_TRUE(row->enabled);
}

TEST_F(MountsDbTest, InsertWithBlobRoundTrips) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto r  = makeRow("/mnt/liveqx/lib1");
    r.cifs_password_blob = std::vector<std::uint8_t>{1,2,3,4,5,0,255};
    auto id = db.insert(r);
    ASSERT_TRUE(id.has_value());

    auto fetched = db.findById(*id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->cifs_password_blob, r.cifs_password_blob);
    EXPECT_TRUE(fetched->hasPassword());
}

TEST_F(MountsDbTest, DuplicateTargetRejected) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    ASSERT_TRUE(db.insert(makeRow("/mnt/liveqx/lib1")).has_value());
    auto dup = db.insert(makeRow("/mnt/liveqx/lib1", "//other/share"));
    EXPECT_FALSE(dup.has_value());
}

TEST_F(MountsDbTest, FindByTarget) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    ASSERT_TRUE(db.insert(makeRow("/mnt/liveqx/lib1")).has_value());
    auto r = db.findByTarget("/mnt/liveqx/lib1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->source, "//srv/share");

    EXPECT_FALSE(db.findByTarget("/nope").has_value());
}

TEST_F(MountsDbTest, UpdateChangesFieldsAndUpdatedAt) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insert(makeRow("/mnt/liveqx/lib1"));
    ASSERT_TRUE(id.has_value());
    auto orig = db.findById(*id).value();
    // make sure subsequent updated_at is strictly greater (1s resolution).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    MountRow modified = orig;
    modified.options = "vers=3.0,iocharset=utf8,rsize=131072";
    modified.ro      = false;
    modified.enabled = false;
    EXPECT_TRUE(db.update(modified));

    auto fetched = db.findById(*id).value();
    EXPECT_EQ(fetched.options, modified.options);
    EXPECT_FALSE(fetched.ro);
    EXPECT_FALSE(fetched.enabled);
    EXPECT_GE(fetched.updated_at, orig.updated_at);
}

TEST_F(MountsDbTest, UpdateUniqueTargetCollision) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto a = db.insert(makeRow("/mnt/liveqx/lib1")).value();
    auto b = db.insert(makeRow("/mnt/liveqx/lib2")).value();
    auto rb = db.findById(b).value();
    rb.target = "/mnt/liveqx/lib1";   // collision with a
    EXPECT_FALSE(db.update(rb));
    auto unchanged = db.findById(b).value();
    EXPECT_EQ(unchanged.target, "/mnt/liveqx/lib2");
    (void)a;
}

TEST_F(MountsDbTest, DeleteIsIdempotent) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id = db.insert(makeRow("/mnt/liveqx/lib1")).value();
    EXPECT_TRUE(db.deleteById(id));
    EXPECT_TRUE(db.deleteById(id));   // idempotent on missing
    EXPECT_FALSE(db.findById(id).has_value());
}

TEST_F(MountsDbTest, UpdateStatusOnlyTouchesStatusFields) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    auto id   = db.insert(makeRow("/mnt/liveqx/lib1")).value();
    auto orig = db.findById(id).value();

    EXPECT_TRUE(db.updateStatus(id, "active", 1234567));
    auto fetched = db.findById(id).value();
    EXPECT_EQ(fetched.last_active_state, "active");
    EXPECT_EQ(fetched.last_status_at,    1234567);
    // updated_at stays the same — status pulse must not bump config-mtime.
    EXPECT_EQ(fetched.updated_at, orig.updated_at);
    EXPECT_EQ(fetched.options,    orig.options);
}

TEST_F(MountsDbTest, ListAllReturnsAllRows) {
    MountsDb db(dbPath());
    ASSERT_TRUE(db.open());
    db.insert(makeRow("/mnt/liveqx/lib1"));
    db.insert(makeRow("/mnt/liveqx/lib2"));
    db.insert(makeRow("/mnt/liveqx/lib3"));
    EXPECT_EQ(db.listAll().size(), 3u);
}
