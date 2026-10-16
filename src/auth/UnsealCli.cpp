// `liveqx --unseal` client — see auth/UnsealCli.h.

#include "auth/UnsealCli.h"

#include <nlohmann/json.hpp>

#include "utils/UnixSocketAddress.h"

#include <sodium.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace fs = std::filesystem;

namespace liveqx::auth {
namespace {

int fail(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
    return 1;
}

}  // namespace

int unsealViaSocket(const fs::path& socket_path, const PassphraseSource& passphrase) {
    const std::string path = socket_path.string();
    util::UnixSocketAddress addr;
    std::string addr_err;
    if (!addr.set(socket_path, addr_err)) {
        // A missing directory means the same as a missing socket.
        return fail("LiveQX is not sealed (or not running): " + addr_err);
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return fail(std::string("socket: ") + std::strerror(errno));
    if (::connect(fd, addr.address(), addr.length()) != 0) {
        const int e = errno;
        ::close(fd);
        if (e == ENOENT || e == ECONNREFUSED)
            return fail("LiveQX is not sealed (or not running): nothing is listening on " + path);
        return fail("cannot connect to " + path + ": " + std::strerror(e));
    }

    auto pass = passphrase("Master key passphrase");
    if (!pass) { ::close(fd); return fail("no passphrase available"); }

    std::string request = nlohmann::json{{"passphrase", *pass}}.dump() + "\n";
    sodium_memzero(pass->data(), pass->size());
    const bool sent = ::write(fd, request.data(), request.size()) ==
                      static_cast<ssize_t>(request.size());
    sodium_memzero(request.data(), request.size());
    if (!sent) { ::close(fd); return fail("cannot send the request"); }

    // One line comes back. Argon2 on the server side can take a second or two.
    std::string reply;
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 30'000) <= 0) { ::close(fd); return fail("no answer from the service"); }
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        reply.append(buf, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos || reply.size() > 4096) break;
    }
    ::close(fd);

    const auto j = nlohmann::json::parse(reply, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("unexpected answer from the service");
    if (j.value("ok", false)) {
        std::cout << "unsealed: true\n";
        return 0;
    }
    const std::string code = j.value("error", std::string{"unknown"});
    if (code == "wrong_passphrase") return fail("wrong passphrase");
    if (code == "too_many_attempts")
        return fail("Too many attempts. Try again in " +
                    std::to_string(j.value("retry_after", 0)) + " s.");
    if (code == "busy") return fail("another attempt is being checked; try again in a moment");
    return fail("the service refused the request: " + code);
}

}  // namespace liveqx::auth
