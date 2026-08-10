// fix41 commit 2 — Subprocess: fork+execve+pipes+timeout helper.
//
// Тестируем через стандартные посикс-утилиты, которые гарантированно
// есть на любом Linux'е CI (cat / true / sh-нет, мы execv абсолютным
// путём). Если /bin/cat нет — тест пропустится через GTEST_SKIP.

#include <gtest/gtest.h>

#include <filesystem>

#include "mounts/Subprocess.h"

using namespace liveqx::mounts;
namespace fs = std::filesystem;

namespace {
bool exists(const std::string& p) { return fs::exists(p); }
}

TEST(Subprocess, RejectsRelativeArgv0) {
    Subprocess sp;
    sp.argv = {"true"};
    auto r  = sp.run();
    EXPECT_NE(r.error.find("absolute"), std::string::npos);
}

TEST(Subprocess, TrueExits0) {
    if (!exists("/bin/true")) GTEST_SKIP() << "/bin/true missing";
    Subprocess sp;
    sp.argv = {"/bin/true"};
    auto r  = sp.run();
    EXPECT_TRUE(r.error.empty());
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
}

TEST(Subprocess, FalseExits1) {
    const std::string falseBin =
        exists("/bin/false") ? "/bin/false" : "/usr/bin/false";
    if (!exists(falseBin)) GTEST_SKIP() << "false binary missing";
    Subprocess sp;
    sp.argv = {falseBin};
    auto r  = sp.run();
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.exit_code, 1);
}

TEST(Subprocess, CatRoundTripsStdin) {
    if (!exists("/bin/cat")) GTEST_SKIP() << "/bin/cat missing";
    Subprocess sp;
    sp.argv       = {"/bin/cat"};
    sp.stdin_data = "hello\nworld\n";
    auto r        = sp.run();
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "hello\nworld\n");
    EXPECT_TRUE(r.stderr_data.empty());
}

TEST(Subprocess, NonexistentBinaryExits127) {
    Subprocess sp;
    sp.argv = {"/no/such/binary-liveqx"};
    auto r  = sp.run();
    // execv в child failed → child делает _exit(127).
    EXPECT_EQ(r.exit_code, 127);
}

TEST(Subprocess, TimeoutKillsHungChild) {
    if (!exists("/bin/sleep")) GTEST_SKIP() << "/bin/sleep missing";
    Subprocess sp;
    sp.argv       = {"/bin/sleep", "30"};
    sp.timeout_ms = std::chrono::milliseconds(200);
    const auto t0 = std::chrono::steady_clock::now();
    auto r        = sp.run();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(r.timed_out);
    EXPECT_LT(dt, 5000) << "killAndReap should not block past ~1s grace";
}

TEST(Subprocess, StdoutCapped) {
    if (!exists("/bin/yes") || !exists("/usr/bin/head")) {
        GTEST_SKIP() << "yes/head missing";
    }
    // /bin/yes печатает бесконечно — без cap'а Subprocess съел бы всю
    // память. Здесь cap = 32 байта; ожидаем, что мы получим именно 32.
    Subprocess sp;
    sp.argv             = {"/bin/yes"};
    sp.timeout_ms       = std::chrono::milliseconds(300);
    sp.max_stdout_bytes = 32;
    auto r              = sp.run();
    // yes без stdout-pipe-close в child'е не завершится сам — наш
    // timeout это и должен поймать.
    EXPECT_TRUE(r.timed_out);
    EXPECT_LE(r.stdout_data.size(), 32u);
}
