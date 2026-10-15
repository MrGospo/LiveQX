// Unit tests for RuntimeLock::probe — "is the service running?" without side effects.
//
// The recovery CLI runs as root. If asking the question created
// state/liveqx.lock, the file would belong to root (mode 0600) and the
// service, running as user liveqx, could no longer open its own lock file.

#include "utils/RuntimeLock.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;
using liveqx::util::RuntimeLock;

namespace {
fs::path tempDir() {
    static std::atomic<int> seq{0};
    auto p = fs::temp_directory_path() /
             ("lqx-rtlock-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
    fs::create_directories(p);
    return p;
}
}  // namespace

TEST(RuntimeLockProbe, AbsentLockFileMeansNotRunningAndIsNotCreated) {
    const auto dir = tempDir();
    std::string err;
    EXPECT_EQ(RuntimeLock::probe(dir / "liveqx.lock", err), RuntimeLock::Probe::NotRunning);
    EXPECT_FALSE(fs::exists(dir / "liveqx.lock"));
    fs::remove_all(dir);
}

TEST(RuntimeLockProbe, HeldLockMeansRunning) {
    const auto dir = tempDir();
    auto lock = RuntimeLock::tryAcquire(dir / "liveqx.lock");
    ASSERT_TRUE(lock.held());
    std::string err;
    EXPECT_EQ(RuntimeLock::probe(dir / "liveqx.lock", err), RuntimeLock::Probe::Running);
    fs::remove_all(dir);
}

TEST(RuntimeLockProbe, ReleasedLockMeansNotRunningAndProbeDoesNotKeepIt) {
    const auto dir = tempDir();
    {
        auto lock = RuntimeLock::tryAcquire(dir / "liveqx.lock");
        ASSERT_TRUE(lock.held());
    }
    std::string err;
    EXPECT_EQ(RuntimeLock::probe(dir / "liveqx.lock", err), RuntimeLock::Probe::NotRunning);
    // The probe must have released what it briefly took.
    auto again = RuntimeLock::tryAcquire(dir / "liveqx.lock");
    EXPECT_TRUE(again.held());
    fs::remove_all(dir);
}

TEST(RuntimeLockProbe, UnreadableLockFileIsAnErrorNotAGuess) {
    const auto dir = tempDir();
    { auto l = RuntimeLock::tryAcquire(dir / "liveqx.lock"); }
    ASSERT_EQ(::chmod((dir / "liveqx.lock").c_str(), 0000), 0);
    std::string err;
    const auto r = RuntimeLock::probe(dir / "liveqx.lock", err);
    if (::geteuid() == 0) {
        EXPECT_EQ(r, RuntimeLock::Probe::NotRunning);   // root opens it anyway
    } else {
        EXPECT_EQ(r, RuntimeLock::Probe::Error);
        EXPECT_FALSE(err.empty());
    }
    fs::remove_all(dir);
}
