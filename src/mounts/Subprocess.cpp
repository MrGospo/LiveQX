#include "mounts/Subprocess.h"

#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace liveqx::mounts {

namespace {

bool setNonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Drain a non-blocking fd, append into `out` up to `cap`. Returns:
//   1 — read more bytes (still data may follow)
//   0 — EOF reached
//  -1 — fatal read error
int drain(int fd, std::string& out, std::size_t cap) {
    char buf[4096];
    int  read_some = 0;
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < cap) {
                const auto room = cap - out.size();
                out.append(buf, std::min(static_cast<std::size_t>(n), room));
            }
            read_some = 1;
            continue;
        }
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return read_some;
        if (errno == EINTR) continue;
        return -1;
    }
}

void killAndReap(::pid_t pid) {
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        int    status = 0;
        ::pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

}  // namespace

ProcessResult Subprocess::run() const {
    ProcessResult res;
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/') {
        res.error = "argv[0] must be an absolute path";
        return res;
    }

    int  in_pipe [2] = {-1, -1};
    int  out_pipe[2] = {-1, -1};
    int  err_pipe[2] = {-1, -1};
    auto closePair = [](int p[2]) {
        if (p[0] >= 0) ::close(p[0]);
        if (p[1] >= 0) ::close(p[1]);
    };

    if (::pipe(in_pipe) < 0 || ::pipe(out_pipe) < 0 || ::pipe(err_pipe) < 0) {
        res.error = std::string("pipe: ") + std::strerror(errno);
        closePair(in_pipe); closePair(out_pipe); closePair(err_pipe);
        return res;
    }

    const ::pid_t pid = ::fork();
    if (pid < 0) {
        res.error = std::string("fork: ") + std::strerror(errno);
        closePair(in_pipe); closePair(out_pipe); closePair(err_pipe);
        return res;
    }

    if (pid == 0) {
        // Child. Close parent ends.
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        // Wire pipes to stdio.
        if (::dup2(in_pipe[0],  STDIN_FILENO ) < 0) ::_exit(127);
        if (::dup2(out_pipe[1], STDOUT_FILENO) < 0) ::_exit(127);
        if (::dup2(err_pipe[1], STDERR_FILENO) < 0) ::_exit(127);
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (auto& s : argv) raw.push_back(const_cast<char*>(s.c_str()));
        raw.push_back(nullptr);

        // execv: PATH-resolution не нужен, argv[0] абсолютный.
        ::execv(raw[0], raw.data());
        ::_exit(127);
    }

    // Parent. Close child ends.
    ::close(in_pipe[0]);  in_pipe[0]  = -1;
    ::close(out_pipe[1]); out_pipe[1] = -1;
    ::close(err_pipe[1]); err_pipe[1] = -1;

    setNonblock(in_pipe[1]);
    setNonblock(out_pipe[0]);
    setNonblock(err_pipe[0]);

    std::size_t stdin_pos = 0;
    bool stdin_done    = stdin_data.empty();
    bool stdout_eof    = false;
    bool stderr_eof    = false;

    if (stdin_done) {
        ::close(in_pipe[1]);
        in_pipe[1] = -1;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout_ms;

    while (!(stdin_done && stdout_eof && stderr_eof)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.timed_out = true;
            killAndReap(pid);
            closePair(in_pipe); closePair(out_pipe); closePair(err_pipe);
            return res;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        struct pollfd pfds[3];
        int n_pfds = 0;
        if (!stdin_done) {
            pfds[n_pfds++] = {in_pipe[1],  POLLOUT, 0};
        }
        if (!stdout_eof) {
            pfds[n_pfds++] = {out_pipe[0], POLLIN,  0};
        }
        if (!stderr_eof) {
            pfds[n_pfds++] = {err_pipe[0], POLLIN,  0};
        }
        const int pr = ::poll(pfds, n_pfds, static_cast<int>(remaining));
        if (pr < 0) {
            if (errno == EINTR) continue;
            res.error = std::string("poll: ") + std::strerror(errno);
            killAndReap(pid);
            closePair(in_pipe); closePair(out_pipe); closePair(err_pipe);
            return res;
        }
        // pr==0 → poll сам не таймаутит нам, мы это поймаем на следующем
        // витке через deadline-проверку.

        for (int i = 0; i < n_pfds; ++i) {
            if (pfds[i].revents == 0) continue;
            const int fd = pfds[i].fd;
            if (fd == in_pipe[1]) {
                const auto remaining_in = stdin_data.size() - stdin_pos;
                ssize_t w = ::write(fd,
                                    stdin_data.data() + stdin_pos,
                                    remaining_in);
                if (w > 0) {
                    stdin_pos += static_cast<std::size_t>(w);
                    if (stdin_pos >= stdin_data.size()) {
                        ::close(in_pipe[1]);
                        in_pipe[1]  = -1;
                        stdin_done  = true;
                    }
                } else if (w < 0
                           && errno != EAGAIN && errno != EINTR) {
                    // Child закрылся раньше — done.
                    ::close(in_pipe[1]);
                    in_pipe[1]  = -1;
                    stdin_done  = true;
                }
            } else if (fd == out_pipe[0]) {
                int rc = drain(fd, res.stdout_data, max_stdout_bytes);
                if (rc <= 0) {
                    ::close(out_pipe[0]);
                    out_pipe[0] = -1;
                    stdout_eof  = true;
                }
            } else if (fd == err_pipe[0]) {
                int rc = drain(fd, res.stderr_data, max_stderr_bytes);
                if (rc <= 0) {
                    ::close(err_pipe[0]);
                    err_pipe[0] = -1;
                    stderr_eof  = true;
                }
            }
        }
    }

    int    status = 0;
    ::pid_t r = 0;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.timed_out = true;
            killAndReap(pid);
            return res;
        }
        r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            res.error = std::string("waitpid: ") + std::strerror(errno);
            killAndReap(pid);
            return res;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (WIFEXITED(status)) {
        res.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.signaled  = true;
        res.exit_code = 128 + WTERMSIG(status);
    }
    return res;
}

}  // namespace liveqx::mounts
