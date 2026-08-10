#include "mounts/CredsHelper.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace liveqx::mounts {

bool PlaintextCredsHelper::writeFile(const std::string& payload,
                                     const fs::path&    out_path,
                                     std::string&       err) {
    if (out_path.empty()) {
        err = "out_path must not be empty";
        return false;
    }

    if (out_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            err = "mkdir " + out_path.parent_path().string() + ": " + ec.message();
            return false;
        }
        // Parent directory: 0700 root:root. Cred files contain a CIFS
        // password — only root should be able to list /run/liveqx/creds.
        fs::permissions(out_path.parent_path(),
                        fs::perms::owner_all,
                        fs::perm_options::replace, ec);
        // Permission failure here is not fatal: parent may be a system-managed
        // RuntimeDirectory whose mode is owned by systemd.
    }

    // Atomic write: open .tmp 0600, write, fsync, rename.
    const auto tmp = out_path.string() + ".tmp";
    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0600);
    if (fd < 0) {
        err = "open " + tmp + ": " + std::strerror(errno);
        return false;
    }

    const char* p = payload.data();
    std::size_t left = payload.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = "write " + tmp + ": " + std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p    += n;
        left -= static_cast<std::size_t>(n);
    }

    if (::fsync(fd) < 0) {
        // fsync may be unsupported on tmpfs; not fatal.
    }
    ::close(fd);

    if (::rename(tmp.c_str(), out_path.c_str()) != 0) {
        err = "rename " + tmp + " -> " + out_path.string() + ": "
              + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }

    // Belt-and-suspenders: enforce 0600 even if umask intervened between
    // open() and rename().
    std::error_code ec;
    fs::permissions(out_path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return true;
}

bool PlaintextCredsHelper::removeFile(const fs::path& path, std::string& err) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    fs::remove(path, ec);
    if (ec) {
        err = "unlink " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace liveqx::mounts
