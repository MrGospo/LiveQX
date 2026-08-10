#include "gateway/net/UdpSocket.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace liveqx::gateway::net {

namespace {

class FdGuard {
public:
    FdGuard() = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() { if (fd_ >= 0) ::close(fd_); }

    int  get()     const noexcept { return fd_; }
    bool valid()   const noexcept { return fd_ >= 0; }
    int  release() noexcept { return std::exchange(fd_, -1); }
private:
    int fd_ = -1;
};

bool isMulticast(const in_addr& a) noexcept {
    return (ntohl(a.s_addr) & 0xF0000000u) == 0xE0000000u;
}

void setErr(std::string* out, const std::string& msg) noexcept {
    if (out) *out = msg;
}

} // namespace

bool nicNameToIPv4(const std::string& name, in_addr& out) noexcept {
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (name != p->ifa_name) continue;
        out = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr;
        found = true; break;
    }
    ::freeifaddrs(ifa);
    return found;
}

int openRxSocket(const UdpRxOptions& opts, std::string* err_out) noexcept {
    in_addr group{};
    if (::inet_pton(AF_INET, opts.address.c_str(), &group) != 1) {
        setErr(err_out, "invalid address '" + opts.address + "'");
        return -1;
    }
    FdGuard fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        setErr(err_out, std::string("socket(): ") + std::strerror(errno));
        return -1;
    }

    const int reuse = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (opts.recv_buffer_kb > 0) {
        const int rcvbuf = opts.recv_buffer_kb * 1024;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    if (opts.rcv_timeout_ms > 0) {
        timeval tv{};
        tv.tv_sec  = opts.rcv_timeout_ms / 1000;
        tv.tv_usec = (opts.rcv_timeout_ms % 1000) * 1000;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons(static_cast<std::uint16_t>(opts.port));
    bind_addr.sin_addr.s_addr =
        isMulticast(group) ? htonl(INADDR_ANY) : group.s_addr;
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        setErr(err_out, "bind(" + opts.address + ":" + std::to_string(opts.port) +
                        ") failed: " + std::strerror(errno));
        return -1;
    }

    if (!opts.interface_name.empty()) {
        ::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE,
                     opts.interface_name.c_str(),
                     static_cast<socklen_t>(opts.interface_name.size()));
    }

    if (isMulticast(group)) {
        ip_mreq mreq{};
        mreq.imr_multiaddr        = group;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!opts.interface_addr.empty()) {
            ::inet_pton(AF_INET, opts.interface_addr.c_str(), &mreq.imr_interface);
        } else if (!opts.interface_name.empty()) {
            in_addr nic{};
            if (nicNameToIPv4(opts.interface_name, nic)) mreq.imr_interface = nic;
        }
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &mreq, sizeof(mreq)) < 0) {
            setErr(err_out, "IP_ADD_MEMBERSHIP(" + opts.address +
                            ") failed: " + std::strerror(errno));
            return -1;
        }
    }

    return fd.release();
}

} // namespace liveqx::gateway::net
