// fix43 — PlaintextCredsHelper: атомарная запись cred-файла на tmpfs.
// До fix43 хелпер шеллил `systemd-creds encrypt` и тесты подсовывали
// shell-fake вместо binary. После fix43 — никаких subprocess'ов, только
// open/write/fsync/rename с гарантированным mode 0600.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "mounts/CredsHelper.h"

using namespace liveqx::mounts;
namespace fs = std::filesystem;

namespace {

fs::path uniqueDir(const std::string& tag) {
    auto p = fs::temp_directory_path() /
        ("sc-creds-" + tag + "-" + std::to_string(::getpid())
         + "-" + std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p.string());
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    return s;
}

mode_t fileMode(const fs::path& p) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return st.st_mode & 0777;
}

}  // namespace

TEST(PlaintextCredsHelper, WriteFileRoundTrip) {
    auto root = uniqueDir("rt");
    auto out  = root / "creds" / "liveqx-mnt-7.cred";

    PlaintextCredsHelper h;
    std::string payload = "username=foo\npassword=bar\n";
    std::string err;
    ASSERT_TRUE(h.writeFile(payload, out, err)) << err;

    EXPECT_EQ(readFile(out), payload);
    EXPECT_EQ(fileMode(out), 0600u);
}

TEST(PlaintextCredsHelper, WriteCreatesParentDirIfMissing) {
    auto root = uniqueDir("mkdir");
    auto out  = root / "deep" / "nested" / "creds" / "x.cred";
    PlaintextCredsHelper h;
    std::string err;
    ASSERT_TRUE(h.writeFile("x", out, err)) << err;
    EXPECT_TRUE(fs::exists(out));
}

TEST(PlaintextCredsHelper, WriteIsAtomicNoTmpLeftBehind) {
    auto root = uniqueDir("atomic");
    auto out  = root / "x.cred";
    PlaintextCredsHelper h;
    std::string err;
    ASSERT_TRUE(h.writeFile("payload-1", out, err)) << err;
    EXPECT_FALSE(fs::exists(out.string() + ".tmp"));

    // Перезапись существующего: тоже атомарна.
    ASSERT_TRUE(h.writeFile("payload-2", out, err)) << err;
    EXPECT_EQ(readFile(out), "payload-2");
    EXPECT_FALSE(fs::exists(out.string() + ".tmp"));
}

TEST(PlaintextCredsHelper, WriteRefusesEmptyOutPath) {
    PlaintextCredsHelper h;
    std::string err;
    EXPECT_FALSE(h.writeFile("x", fs::path{}, err));
}

TEST(PlaintextCredsHelper, WriteHardensParentDirTo0700) {
    auto root = uniqueDir("perm");
    auto out  = root / "subdir" / "x.cred";
    PlaintextCredsHelper h;
    std::string err;
    ASSERT_TRUE(h.writeFile("x", out, err)) << err;
    // Parent создан hardened: только owner.
    EXPECT_EQ(fileMode(out.parent_path()), 0700u);
}

TEST(PlaintextCredsHelper, RemoveFileMissingIsNoop) {
    PlaintextCredsHelper h;
    std::string err;
    EXPECT_TRUE(h.removeFile("/tmp/_definitely_not_there_xyz.cred", err));
}

TEST(PlaintextCredsHelper, RemoveFileExisting) {
    auto root = uniqueDir("rm");
    auto p    = root / "x.cred";
    {
        std::ofstream s(p.string());
        s << "data";
    }
    ASSERT_TRUE(fs::exists(p));
    PlaintextCredsHelper h;
    std::string err;
    EXPECT_TRUE(h.removeFile(p, err));
    EXPECT_FALSE(fs::exists(p));
}
