#include "mounts/MountdRpcClient.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace liveqx::mounts {

namespace {

void setRcvSndTimeouts(int fd, std::chrono::milliseconds total) {
    timeval tv{};
    tv.tv_sec  = static_cast<long>(total.count() / 1000);
    tv.tv_usec = static_cast<long>((total.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int connectTo(const std::filesystem::path& path,
              std::chrono::milliseconds timeout,
              std::string& err) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    setRcvSndTimeouts(fd, timeout);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto s = path.string();
    if (s.size() >= sizeof(addr.sun_path)) {
        err = "socket path too long";
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, s.data(), s.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("connect ") + s + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

MountdRpcClient::MountdRpcClient(Config cfg) : cfg_(std::move(cfg)) {}

RpcResponse MountdRpcClient::roundTrip(RpcOp op, const nlohmann::json& body) {
    std::string conn_err;
    int fd = connectTo(cfg_.socket_path, cfg_.rpc_timeout, conn_err);
    if (fd < 0) {
        return RpcResponse::fail("rpc connect: " + conn_err, "rpc_connect_failed");
    }

    nlohmann::json req = body.is_object() ? body : nlohmann::json::object();
    req["op"] = toString(op);
    std::string werr;
    if (!writeFrame(fd, req, werr)) {
        ::close(fd);
        return RpcResponse::fail("rpc write: " + werr, "rpc_write_failed");
    }

    nlohmann::json resp;
    std::string rerr;
    if (!readFrame(fd, resp, rerr)) {
        ::close(fd);
        return RpcResponse::fail("rpc read: " + rerr, "rpc_read_failed");
    }
    ::close(fd);

    return RpcResponse::fromJson(resp);
}

RpcResponse MountdRpcClient::applyMount(const MountSpec& spec) {
    return roundTrip(RpcOp::ApplyMount,
                     nlohmann::json{{"spec", spec.toJson(/*include_password=*/true)}});
}

RpcResponse MountdRpcClient::removeMount(std::int64_t id,
                                         std::string_view target) {
    return roundTrip(RpcOp::RemoveMount,
                     nlohmann::json{{"id", id},
                                    {"target", std::string(target)}});
}

RpcResponse MountdRpcClient::testMount(const MountSpec& spec) {
    return roundTrip(RpcOp::TestMount,
                     nlohmann::json{{"spec", spec.toJson(true)}});
}

RpcResponse MountdRpcClient::status(const std::vector<StatusQueryItem>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& it : items) {
        arr.push_back({{"id", it.id}, {"target", it.target}});
    }
    return roundTrip(RpcOp::Status, nlohmann::json{{"items", std::move(arr)}});
}

}  // namespace liveqx::mounts
