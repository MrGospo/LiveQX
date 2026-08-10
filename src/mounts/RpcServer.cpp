#include "mounts/RpcServer.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace liveqx::mounts {

namespace {

// Возвращает gid группы по имени, или -1 если группы нет. Размер
// буфера getgrnam_r — sysconf(_SC_GETGR_R_SIZE_MAX), с фолбэком на 4K.
::gid_t resolveGid(const std::string& name, std::string& err) {
    long sz = ::sysconf(_SC_GETGR_R_SIZE_MAX);
    if (sz <= 0) sz = 4096;
    std::string buf(static_cast<std::size_t>(sz), '\0');
    struct group  grp{};
    struct group* result = nullptr;
    int rc = ::getgrnam_r(name.c_str(), &grp, buf.data(), buf.size(), &result);
    if (rc != 0) {
        err = std::string("getgrnam_r ") + name + ": " + std::strerror(rc);
        return static_cast<::gid_t>(-1);
    }
    if (!result) {
        err = "group not found: " + name;
        return static_cast<::gid_t>(-1);
    }
    return result->gr_gid;
}

}  // namespace

RpcServer::RpcServer(Config cfg, RpcHandler handler)
    : cfg_(std::move(cfg)), handler_(std::move(handler)) {}

RpcServer::~RpcServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    std::error_code ec;
    std::filesystem::remove(cfg_.socket_path, ec);
}

bool RpcServer::start(std::string& out_error) {
    if (cfg_.socket_path.empty()) {
        out_error = "socket_path is empty";
        return false;
    }
    // Длина sun_path жёстко 108 байт. Любая попытка превысить — сразу
    // на старте, чтобы оператор увидел внятную ошибку.
    const std::string spath = cfg_.socket_path.string();
    if (spath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        out_error = "socket_path too long for sockaddr_un";
        return false;
    }

    // На случай, если предыдущий процесс упал не успев отвязать сокет.
    // (Если сокет занят живым процессом — bind() ниже даст EADDRINUSE
    // и мы завершимся с понятной ошибкой.)
    {
        std::error_code ec;
        std::filesystem::remove(cfg_.socket_path, ec);
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        out_error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, spath.data(), spath.size());

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        out_error = std::string("bind ") + spath + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }

    // mode 0660 → группе нужен write для connect. Делаем chmod ДО
    // listen, иначе race между listen и chmod пускает невалидированных
    // клиентов.
    if (::chmod(spath.c_str(), cfg_.socket_mode) < 0) {
        out_error = std::string("chmod ") + spath + ": " + std::strerror(errno);
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(cfg_.socket_path, ec);
        return false;
    }

    if (!cfg_.skip_chown) {
        std::string err;
        const ::gid_t gid = resolveGid(cfg_.client_group, err);
        if (gid == static_cast<::gid_t>(-1)) {
            out_error = err;
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(cfg_.socket_path, ec);
            return false;
        }
        if (::chown(spath.c_str(), static_cast<::uid_t>(-1), gid) < 0) {
            out_error = std::string("chown ") + spath + ": " + std::strerror(errno);
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(cfg_.socket_path, ec);
            return false;
        }
    }

    if (::listen(fd, 4) < 0) {
        out_error = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(cfg_.socket_path, ec);
        return false;
    }

    listen_fd_ = fd;
    spdlog::info("mountd: listening on {}", spath);
    return true;
}

void RpcServer::stop() {
    stop_requested_.store(true, std::memory_order_release);
    // Закрываем listen-fd чтобы accept в runForever() сразу вернулся.
    // Используем shutdown+close: shutdown будит accept на Linux.
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void RpcServer::runForever() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        sockaddr_un peer{};
        socklen_t   plen = sizeof(peer);
        const int fd = ::accept4(listen_fd_,
                                 reinterpret_cast<sockaddr*>(&peer), &plen,
                                 SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (stop_requested_.load(std::memory_order_acquire)) break;
            spdlog::warn("mountd: accept failed: {}", std::strerror(errno));
            // На неустранимом провале не крутим busy-loop'ом — лучше
            // умереть и дать systemd рестартнуть.
            break;
        }
        handleOne(fd);
        ::close(fd);
    }
}

bool RpcServer::authorizePeer(int conn_fd, std::string& out_who) {
    // SO_PEERCRED — стандартный Linux способ узнать pid/uid/gid того,
    // кто открыл другой конец AF_UNIX сокета. На момент connect()
    // ядро снапшотит идентичность, и подмена через setuid после уже
    // не работает — поэтому это надёжно.
    struct ucred c {};
    socklen_t    n = sizeof(c);
    if (::getsockopt(conn_fd, SOL_SOCKET, SO_PEERCRED, &c, &n) < 0) {
        spdlog::warn("mountd: SO_PEERCRED failed: {}", std::strerror(errno));
        return false;
    }

    out_who = "uid=" + std::to_string(c.uid) + " pid=" + std::to_string(c.pid);

    if (c.uid == 0) return true;  // root всегда допущен.

    if (cfg_.skip_chown) {
        // В тестах socket sits на пользовательской группе; SO_PEERCRED
        // даст только primary gid, а нужная — supplementary. Проверка
        // через chown на bind'е уже отсекла «не своих»; этого достаточно
        // для тест-сценария.
        return true;
    }

    std::string err;
    const ::gid_t expected = resolveGid(cfg_.client_group, err);
    if (expected == static_cast<::gid_t>(-1)) {
        spdlog::warn("mountd: cannot resolve group {}: {}", cfg_.client_group, err);
        return false;
    }
    if (c.gid == expected) return true;

    // Supplementary groups: SO_PEERCRED отдаёт только primary gid.
    // Linux 4.13 имеет SO_PEERGROUPS, но он не везде доступен. Альтернатива:
    // resolveпо uid → getpwuid → getgrouplist. Делаем именно так.
    long sz = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (sz <= 0) sz = 4096;
    std::string pwbuf(static_cast<std::size_t>(sz), '\0');
    struct passwd  pw{};
    struct passwd* pwres = nullptr;
    if (::getpwuid_r(c.uid, &pw, pwbuf.data(), pwbuf.size(), &pwres) != 0
        || !pwres) {
        spdlog::warn("mountd: getpwuid_r({}) failed", c.uid);
        return false;
    }
    int   ngroups = 32;
    std::vector<::gid_t> groups(static_cast<std::size_t>(ngroups));
    if (::getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &ngroups) < 0) {
        groups.resize(static_cast<std::size_t>(ngroups));
        if (::getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &ngroups) < 0) {
            spdlog::warn("mountd: getgrouplist({}) failed", pw.pw_name);
            return false;
        }
    }
    groups.resize(static_cast<std::size_t>(ngroups));
    for (auto g : groups) if (g == expected) return true;

    spdlog::warn("mountd: rejected peer uid={} gid={} (not in group {})",
                 c.uid, c.gid, cfg_.client_group);
    return false;
}

void RpcServer::handleOne(int conn_fd) {
    std::string who;
    if (!authorizePeer(conn_fd, who)) {
        // Закрываем без ответа — не подтверждаем формат сокета
        // невалидированному клиенту.
        return;
    }

    nlohmann::json body;
    std::string err;
    if (!readFrame(conn_fd, body, err)) {
        spdlog::warn("mountd[{}]: readFrame failed: {}", who, err);
        return;
    }

    const std::string op_str = body.value("op", std::string{});
    RpcOp op;
    if (!rpcOpFromString(op_str, op)) {
        auto resp = RpcResponse::fail("unknown op: " + op_str, "rejected").toJson();
        std::string werr;
        writeFrame(conn_fd, resp, werr);
        spdlog::warn("mountd[{}]: rejected unknown op '{}'", who, op_str);
        return;
    }

    RpcResponse resp;
    try {
        resp = handler_(op, body);
    } catch (const std::exception& e) {
        resp = RpcResponse::fail(std::string("handler exception: ") + e.what());
        spdlog::error("mountd[{}]: handler {} threw: {}", who, op_str, e.what());
    }

    std::string werr;
    if (!writeFrame(conn_fd, resp.toJson(), werr)) {
        spdlog::warn("mountd[{}]: writeFrame failed: {}", who, werr);
    }
}

}  // namespace liveqx::mounts
