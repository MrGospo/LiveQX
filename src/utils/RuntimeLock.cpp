// fix35 (ROADMAP_1 A3.12) — see RuntimeLock.h for the why.

#include "utils/RuntimeLock.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace liveqx::util {

namespace fs = std::filesystem;

RuntimeLock::RuntimeLock() = default;

RuntimeLock::RuntimeLock(RuntimeLock&& o) noexcept
    : fd_(o.fd_), path_(std::move(o.path_)), error_(std::move(o.error_)) {
    o.fd_ = -1;
}

RuntimeLock& RuntimeLock::operator=(RuntimeLock&& o) noexcept {
    if (this != &o) {
        release();
        fd_     = o.fd_;
        path_   = std::move(o.path_);
        error_  = std::move(o.error_);
        o.fd_   = -1;
    }
    return *this;
}

RuntimeLock::~RuntimeLock() { release(); }

void RuntimeLock::release() noexcept {
    if (fd_ >= 0) {
        // Closing the fd releases the flock automatically; explicit
        // LOCK_UN is redundant but cheap and self-documenting.
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

RuntimeLock RuntimeLock::tryAcquire(const fs::path& path) {
    RuntimeLock lock;
    lock.path_ = path;

    // Make sure the parent dir exists — on a fresh install state/ may
    // not have been created yet by the time we grab the lock.
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        lock.error_ = std::string{"open("} + path.string() + "): "
                      + std::strerror(errno);
        return lock;
    }

    // Best-effort permissions tightening for the case where the file
    // already existed with a wider umask. flock semantics don't care
    // about mode bits, but we want to keep state/ tidy.
    ::fchmod(fd, 0600);

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        lock.error_ = (e == EWOULDBLOCK)
            ? std::string{"another instance is running"}
            : std::string{"flock("} + path.string() + "): " + std::strerror(e);
        ::close(fd);
        return lock;
    }

    lock.fd_ = fd;
    return lock;
}

}  // namespace liveqx::util
