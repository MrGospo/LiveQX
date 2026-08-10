#pragma once
//
// fix35 (ROADMAP_1 A3.12) — single-instance lock for liveqx.
//
// Why we need this: the master-key reset CLI must refuse to run while
// the server is alive (the running process holds the old key in memory
// and would silently keep writing fresh ciphertext after the wipe).
// We can't rely on SQLite locks because the server's AuthDb opens a
// connection in the default rollback-journal mode and only takes the
// file lock for the duration of an active statement — not the whole
// uptime.
//
// Instead we use a Unix file-lock (`flock(LOCK_EX|LOCK_NB)`) on a
// dedicated `<state_dir>/liveqx.lock` file. The kernel
// guarantees that only one process at a time can hold the lock, and
// the lock is released automatically when the file descriptor is
// closed (or the process dies for any reason — kernel panic, kill -9).
// No PID-file races, no stale state to clean up.
//
// Usage:
//   auto lock = RuntimeLock::tryAcquire(state_dir / "liveqx.lock");
//   if (!lock.held()) { /* another instance is running */ }
//
// And `MasterKeyReset` does the inverse: tries to acquire and treats
// EWOULDBLOCK as "service is running". On success it releases the lock
// immediately so the running-by-mistake operator can stop the service
// and re-run reset without leftover state.

#include <filesystem>
#include <string>

namespace liveqx::util {

class RuntimeLock {
public:
    // Try to acquire an exclusive non-blocking flock on `path`.
    // The file is created with mode 0600 if it doesn't exist.
    static RuntimeLock tryAcquire(const std::filesystem::path& path);

    RuntimeLock();
    RuntimeLock(const RuntimeLock&)            = delete;
    RuntimeLock& operator=(const RuntimeLock&) = delete;
    RuntimeLock(RuntimeLock&& o) noexcept;
    RuntimeLock& operator=(RuntimeLock&& o) noexcept;
    ~RuntimeLock();

    bool held() const noexcept { return fd_ >= 0; }

    // Empty when held() is false; useful for diagnostics.
    const std::string& error() const noexcept { return error_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // Drop the lock and close the fd. Idempotent. The lockfile is left
    // on disk — `flock` semantics don't care about the file's existence,
    // only about its inode.
    void release() noexcept;

private:
    int                    fd_{-1};
    std::filesystem::path  path_;
    std::string            error_;
};

}  // namespace liveqx::util
