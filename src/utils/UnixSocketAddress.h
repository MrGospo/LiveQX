#pragma once
//
// Address of a Unix-domain socket for a filesystem path of any length.
//
// sockaddr_un.sun_path holds about 107 bytes. A state directory deep in a
// home directory or a container path can exceed that. The standard way round
// it on Linux is to open the parent directory and address the socket relative
// to the descriptor: "/proc/self/fd/<dirfd>/<name>". The descriptor must stay
// open until bind()/connect() has returned.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace liveqx::util {

class UnixSocketAddress {
public:
    UnixSocketAddress() = default;
    UnixSocketAddress(const UnixSocketAddress&) = delete;
    UnixSocketAddress& operator=(const UnixSocketAddress&) = delete;
    ~UnixSocketAddress() { if (dir_fd_ >= 0) ::close(dir_fd_); }

    // False (with `err`) if the path cannot be expressed: its directory cannot
    // be opened, or even the file name alone is too long.
    bool set(const std::filesystem::path& path, std::string& err) {
        std::memset(&addr_, 0, sizeof addr_);
        addr_.sun_family = AF_UNIX;
        const std::string full = path.string();
        if (full.size() < sizeof(addr_.sun_path)) {
            std::memcpy(addr_.sun_path, full.c_str(), full.size() + 1);
            return true;
        }
        const auto parent = path.parent_path();
        dir_fd_ = ::open(parent.empty() ? "." : parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd_ < 0) {
            err = "cannot open " + parent.string() + ": " + std::strerror(errno);
            return false;
        }
        const std::string rel =
            "/proc/self/fd/" + std::to_string(dir_fd_) + "/" + path.filename().string();
        if (rel.size() >= sizeof(addr_.sun_path)) {
            err = "the socket file name is too long: " + path.filename().string();
            return false;
        }
        std::memcpy(addr_.sun_path, rel.c_str(), rel.size() + 1);
        return true;
    }

    const sockaddr* address() const { return reinterpret_cast<const sockaddr*>(&addr_); }
    socklen_t length() const { return sizeof addr_; }

private:
    sockaddr_un addr_{};
    int         dir_fd_{-1};
};

}  // namespace liveqx::util
