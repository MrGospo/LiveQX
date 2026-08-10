#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "output/HlsOutput.h"
#include "output/HlsOutputCfg.h"

extern char** environ;

using liveqx::hls::HlsCfg;
using liveqx::hls::HlsOutput;
using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

constexpr const char* kFfmpegBin   = "/usr/bin/ffmpeg";
constexpr const char* kFfprobeBin  = "/usr/bin/ffprobe";

bool ffmpegToolsAvailable() {
    return ::access(kFfmpegBin, X_OK) == 0
        && ::access(kFfprobeBin, X_OK) == 0;
}

// fix14 c7 — end-to-end HLS muxer integration. Spawns ffmpeg as an
// MPEG-TS encoder writing to a pipe; the test reads the pipe in 64 KiB
// chunks and feeds them as Packets into HlsOutput, which runs the same
// SPSC + AVIO + mpegts demuxer + HLS muxer pipeline a real channel uses.
// After ~12s we stop and verify the playlist + segments with ffprobe.
//
// This is the test that catches "muxer not actually wired" regressions —
// the unit tests cover cfg/orphan logic only and never touch libav.

struct TmpDir {
    fs::path path;
    TmpDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto cand = base / ("hls_real_test_" + std::to_string(::getpid())
                                       + "_" + std::to_string(i));
            std::error_code ec;
            if (fs::create_directory(cand, ec)) {
                path = cand;
                return;
            }
        }
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Spawns ffmpeg writing MPEG-TS to its stdout. Returns the read-end fd
// and the pid; caller closes the fd and reaps the pid. Stderr is dropped
// — encoder warnings would noise up the test log.
struct EncoderProc {
    pid_t pid     = -1;
    int   stdout_ = -1;

    ~EncoderProc() {
        if (stdout_ >= 0) ::close(stdout_);
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

EncoderProc spawnTsEncoder() {
    EncoderProc out;

    int pipefd[2];
    if (::pipe(pipefd) != 0) return out;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Child: stdout → write end, stderr → /dev/null (warnings noise the log).
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2 (&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    posix_spawn_file_actions_addopen (&fa, STDERR_FILENO, "/dev/null",
                                      O_WRONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags  (&attr, POSIX_SPAWN_SETPGROUP);

    std::vector<std::string> argv_owner = {
        kFfmpegBin,
        "-hide_banner", "-loglevel", "warning",
        "-re",
        "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10",
        "-f", "lavfi", "-i", "sine=frequency=1000",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-g", "10", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-ar", "48000", "-ac", "2", "-b:a", "64k",
        "-f", "mpegts", "pipe:1",
    };
    std::vector<char*> argv;
    argv.reserve(argv_owner.size() + 1);
    for (auto& s : argv_owner) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, kFfmpegBin, &fa, &attr,
                                  argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(pipefd[1]);   // parent only reads
    if (rc != 0) {
        ::close(pipefd[0]);
        return out;
    }
    out.pid     = pid;
    out.stdout_ = pipefd[0];
    return out;
}

// Runs ffprobe against `arg` and returns its exit code + captured stdout.
struct ProbeResult {
    int          exit_code = -1;
    std::string  out;
};
ProbeResult runFfprobe(std::initializer_list<const char*> extra,
                       const std::string& target) {
    ProbeResult r;

    int pipefd[2];
    if (::pipe(pipefd) != 0) return r;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2 (&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    posix_spawn_file_actions_addopen (&fa, STDERR_FILENO, "/dev/null",
                                      O_WRONLY, 0);

    std::vector<std::string> argv_owner = {
        kFfprobeBin, "-hide_banner", "-loglevel", "error",
    };
    for (const char* x : extra) argv_owner.emplace_back(x);
    argv_owner.push_back(target);

    std::vector<char*> argv;
    argv.reserve(argv_owner.size() + 1);
    for (auto& s : argv_owner) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, kFfprobeBin, &fa, nullptr,
                                  argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        return r;
    }

    char buf[4096];
    while (true) {
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        r.out.append(buf, buf + n);
    }
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

int countSegments(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code lec;
        if (!e.is_regular_file(lec)) continue;
        const auto name = e.path().filename().string();
        // matches default seg_NNNNN.ts
        static const std::regex re(R"(^seg_\d+\.ts$)");
        if (std::regex_match(name, re)) ++n;
    }
    return n;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST(HlsOutputItest, EndToEndProducesValidPlaylistAndSegments) {
    if (!ffmpegToolsAvailable())
        GTEST_SKIP() << "ffmpeg/ffprobe binaries not present";

    TmpDir d;

    HlsCfg cfg;
    cfg.output_dir                = d.path.string();
    cfg.segment_duration_sec      = 2;     // tighter than default to keep the test brief
    cfg.playlist_size             = 3;
    cfg.delete_segments_after_sec = 30;    // >= 2 * 3 (window guard)
    cfg.playlist_filename         = "stream.m3u8";
    cfg.segment_filename_pattern  = "seg_%05d.ts";

    HlsOutput out(std::move(cfg));
    ASSERT_TRUE(out.start());

    auto enc = spawnTsEncoder();
    ASSERT_GT(enc.pid, 0)    << "posix_spawnp(ffmpeg encoder) failed";
    ASSERT_GE(enc.stdout_, 0);

    // Pump 12s of MPEG-TS bytes from the encoder → HlsOutput::send. 12s
    // gives ~5-6 segments at hls_time=2s, comfortably more than
    // playlist_size=3 so we exercise the sliding-window rotation.
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    std::vector<uint8_t> buf(64 * 1024);
    uint64_t bytes_pumped = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(enc.stdout_, buf.data(), buf.size());
        if (n <= 0) break;
        Packet p;
        p.data.assign(buf.data(), buf.data() + n);
        out.send(p);
        bytes_pumped += static_cast<uint64_t>(n);
    }

    // Stop encoder first so the writer thread sees clean EOF on its
    // demuxer rather than a cut-mid-segment SIGKILL race.
    ::kill(-enc.pid, SIGKILL);
    int status = 0;
    ::waitpid(enc.pid, &status, 0);
    enc.pid = -1;
    ::close(enc.stdout_);
    enc.stdout_ = -1;

    // Give the muxer a beat to flush whatever was in flight before we tear
    // it down, so the trailer + final rename actually land on disk.
    std::this_thread::sleep_for(500ms);

    out.stop();

    EXPECT_GT(bytes_pumped, 64u * 1024u)
        << "ffmpeg encoder produced almost nothing — pipe broken?";

    // ── Playlist sanity ────────────────────────────────────────────────
    const fs::path playlist = d.path / "stream.m3u8";
    ASSERT_TRUE(fs::exists(playlist)) << "stream.m3u8 was never written";
    const std::string m3u8 = slurp(playlist);
    EXPECT_NE(m3u8.find("#EXTM3U"),                   std::string::npos);
    EXPECT_NE(m3u8.find("#EXT-X-VERSION:"),           std::string::npos);
    EXPECT_NE(m3u8.find("#EXT-X-TARGETDURATION:"),    std::string::npos);
    EXPECT_NE(m3u8.find("#EXT-X-MEDIA-SEQUENCE:"),    std::string::npos);

    // ── Segment count ──────────────────────────────────────────────────
    const int n_segs = countSegments(d.path);
    EXPECT_GE(n_segs, 1) << "no .ts segments produced";
    // playlist_size + delete_threshold guarantees a small upper bound; we
    // don't pin an exact number because timing on a loaded CI host varies.
    EXPECT_LE(n_segs, 20)
        << "too many segments — delete_segments flag may be misconfigured";

    // ── ffprobe the playlist as HLS ────────────────────────────────────
    auto probe_pl = runFfprobe(
        {"-show_entries", "stream=codec_name,codec_type", "-of", "csv=p=0",
         "-i"},
        playlist.string());
    EXPECT_EQ(probe_pl.exit_code, 0)
        << "ffprobe rejected the playlist:\n" << probe_pl.out;
    // Expect at minimum h264 + aac in the dump.
    EXPECT_NE(probe_pl.out.find("h264"), std::string::npos)
        << "no H.264 stream visible to ffprobe; got:\n" << probe_pl.out;
    EXPECT_NE(probe_pl.out.find("aac"),  std::string::npos)
        << "no AAC stream visible to ffprobe; got:\n" << probe_pl.out;

    // ── statusJson surface check ───────────────────────────────────────
    const auto status_json = out.statusJson();
    EXPECT_EQ(status_json["transport"].get<std::string>(), "hls");
    EXPECT_GT(status_json["packets_sent"].get<uint64_t>(), 0u);
    EXPECT_GT(status_json["bytes_sent"].get<uint64_t>(),   0u);
    EXPECT_GE(status_json["segments_present"].get<int>(),  1);
}
