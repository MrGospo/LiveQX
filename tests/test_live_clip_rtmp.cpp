#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "clips/LiveClip.h"
#include "clips/RtmpInput.h"

extern char** environ;

using liveqx::LiveClip;
using liveqx::rtmp::InputCfg;
using namespace std::chrono_literals;

namespace {

// fix13 c11 — end-to-end RTMP integration. Unit-level state-machine
// behaviour is already pinned in test_live_clip.cpp via a fake input.
// Here we exercise the full pipeline against a real ffmpeg publisher so
// the bundled libav build (rebuilt 2026-05-05 with --enable-network
// --enable-protocol=rtmp + OpenSSL) is verified end-to-end and we don't
// ship a regression where avformat_open_input silently returns "Protocol
// not found".
//
// Test rig: RtmpInput in server-mode listens on a free loopback port
// (?listen=1 in the URL). A spawned `ffmpeg` publishes a 320x240 lavfi
// testsrc + sine wave into the listener. LiveClip is driven through
// Idle → WarmingUp → Live, then the publisher is SIGKILL'd to verify
// loss detection.

constexpr const char* kFfmpegBin   = "/usr/bin/ffmpeg";
constexpr const char* kStreamKey   = "rtmp-itest-key";

// Acquire an unused TCP port by binding to 0 and reading the assigned
// port back. Race window between close() and the next bind is
// acceptable for an isolated test that nobody else hammers.
int pickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        ::close(fd); return 0;
    }
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool ffmpegAvailable() {
    return ::access(kFfmpegBin, X_OK) == 0;
}

// Steady-clock now in nanoseconds — same epoch RtmpInput uses for
// lastPacketNs(), so LiveClip's loss-threshold compare is meaningful.
std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// RAII wrapper around a posix_spawnp'd ffmpeg subprocess so a failing
// assertion never leaves a zombie publisher running.
struct SpawnedFfmpeg {
    pid_t pid = -1;

    ~SpawnedFfmpeg() {
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

// Spawns a publisher that pushes lavfi testsrc + sine to
// rtmp://127.0.0.1:PORT/live/{streamKey}. Backoff is built-in via
// ffmpeg's reconnect flags so a momentary connect race is tolerated.
SpawnedFfmpeg spawnRtmpPublisher(int port, const std::string& stream_key) {
    SpawnedFfmpeg out;

    const std::string url = "rtmp://127.0.0.1:" + std::to_string(port)
                          + "/live/" + stream_key;

    std::vector<std::string> argv_owner = {
        kFfmpegBin,
        "-hide_banner", "-loglevel", "warning",
        "-re",
        "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10",
        "-f", "lavfi", "-i", "sine=frequency=1000",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-g", "10", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-ar", "48000", "-ac", "2", "-b:a", "64k",
        "-f", "flv", url,
    };
    std::vector<char*> argv;
    argv.reserve(argv_owner.size() + 1);
    for (auto& s : argv_owner) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Own process group so the destructor can SIGKILL -pid and reap any
    // libav-side helper threads ffmpeg may fork.
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, kFfmpegBin, nullptr, &attr,
                                  argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return out;
    out.pid = pid;
    return out;
}

// Constructs a server-mode LiveClip listening on `port` with the test
// stream key. Tight reconnect/buffer values keep the test fast.
LiveClip makeServerLiveClip(int port,
                            std::uint64_t duration_ns,
                            std::uint64_t warm_up_ns,
                            std::uint64_t loss_threshold_ns) {
    InputCfg cfg;
    cfg.mode                 = "server";
    cfg.url                  = "rtmp://127.0.0.1:" + std::to_string(port) + "/live";
    cfg.stream_key           = kStreamKey;
    cfg.reconnect_initial_ms = 200;
    cfg.reconnect_max_ms     = 2000;
    cfg.buffer_ms            = 100;
    auto input = std::make_unique<RtmpInput>(std::move(cfg), 320, 240);

    LiveClip::Cfg lc;
    lc.id                = "rtmp-itest";
    lc.duration_ns       = duration_ns;
    lc.warm_up_ns        = warm_up_ns;
    lc.loss_threshold_ns = loss_threshold_ns;
    return LiveClip(std::move(lc), std::move(input));
}

// Polls /proc/net/tcp for a LISTEN-state socket on the given port. Used
// instead of an active TCP connect probe — connect()ing into FFmpeg's
// listening socket would consume the first accept() and tear down the
// (single-shot) listener before our actual publisher gets to it.
bool waitForListener(int port, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    // Linux tcp listening sockets show up as `<local_ip>:<hex_port>` with
    // state 0A (TCP_LISTEN). We scan both /proc/net/tcp (IPv4) and
    // /proc/net/tcp6 (in case FFmpeg binds a v6 wildcard).
    char hex[64];
    std::snprintf(hex, sizeof(hex), ":%04X 00000000:0000 0A", port);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const char* path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
            FILE* f = std::fopen(path, "re");
            if (!f) continue;
            char buf[512];
            bool found = false;
            while (std::fgets(buf, sizeof(buf), f)) {
                if (std::strstr(buf, hex)) { found = true; break; }
            }
            std::fclose(f);
            if (found) return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

} // namespace

TEST(LiveClipRtmpItest, EndToEndIdleToLiveAndLost) {
    if (!ffmpegAvailable()) GTEST_SKIP() << "ffmpeg binary not present";

    const int port = pickFreePort();
    ASSERT_GT(port, 0) << "could not allocate a free TCP port";

    // Pin start far enough past now that the warm-up window itself sits
    // in the future — first tick must land in Idle. Layout:
    //
    //   [base ........ warm_at .......... start_ns .......... start+duration]
    //   |  <-300ms->  | <-- warm_up_ns -> |
    //
    // duration is far past the test horizon so we never hit hard-finish.
    const std::uint64_t base       = nowNs();
    const std::uint64_t warm_up    = 8'000'000'000ULL;             // 8s
    const std::uint64_t start_ns   = base + warm_up + 300'000'000ULL;
    const std::uint64_t duration   = 60'000'000'000ULL;            // 60s
    const std::uint64_t loss_thr   = 1'500'000'000ULL;             // 1.5s

    auto clip = makeServerLiveClip(port, duration, warm_up, loss_thr);
    clip.setScheduledStart(start_ns);

    // First tick (before warm-up window): clip is still Idle and the
    // input has not been opened — verifies preroll has not fired early.
    clip.onTick(base);
    EXPECT_EQ(clip.state(), LiveClip::State::Idle);

    // Drive ticks until the warm-up window opens; this triggers
    // LiveClip::prepare → RtmpInput::prepare → openContext (bind+listen).
    // Need to wait for steady_clock to cross start_ns - warm_up.
    const auto warmup_deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < warmup_deadline) {
        clip.onTick(nowNs());
        if (clip.state() == LiveClip::State::WarmingUp) break;
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_EQ(clip.state(), LiveClip::State::WarmingUp)
        << "LiveClip did not enter WarmingUp within 1s of the warm-up boundary";

    // Once WarmingUp is reached, RtmpInput is in avformat_open_input
    // waiting for a publisher. Wait until the listening socket is bound,
    // then spawn the publisher.
    ASSERT_TRUE(waitForListener(port, 3000ms))
        << "RtmpInput server-mode never bound port " << port;

    auto publisher = spawnRtmpPublisher(port, kStreamKey);
    ASSERT_GT(publisher.pid, 0) << "posix_spawnp(ffmpeg publisher) failed";

    // Spin onTick at ~50 Hz until LiveClip flips to Live. ffmpeg + libav
    // need a few seconds on loopback to negotiate the handshake and emit
    // the first keyframe; 12s is generous but bounded.
    const auto deadline_live = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < deadline_live) {
        clip.onTick(nowNs());
        if (clip.state() == LiveClip::State::Live) break;
        std::this_thread::sleep_for(20ms);
    }

    ASSERT_EQ(clip.state(), LiveClip::State::Live)
        << "LiveClip did not reach Live within 12s — RTMP open likely "
           "failed (verify bundled FFmpeg has --enable-protocol=rtmp)";

    auto status = clip.statusJson();
    ASSERT_TRUE(status.contains("input"));
    EXPECT_GT(status["last_packet_ns"].get<std::int64_t>(), 0);
    EXPECT_EQ(status["state"].get<std::string>(), "Live");

    // Kill the publisher. RtmpInput's read loop sees the TCP RST or read
    // timeout, marks the input unhealthy, and onTick flips LiveClip to
    // Lost once loss_threshold elapses without a fresh packet.
    ::kill(-publisher.pid, SIGKILL);
    int status_code = 0;
    ::waitpid(publisher.pid, &status_code, 0);
    publisher.pid = -1;

    const auto deadline_lost =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline_lost) {
        clip.onTick(nowNs());
        if (clip.state() == LiveClip::State::Lost) break;
        std::this_thread::sleep_for(50ms);
    }

    EXPECT_EQ(clip.state(), LiveClip::State::Lost)
        << "LiveClip did not detect upstream loss within 8s of SIGKILL";
}
