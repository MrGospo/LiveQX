#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
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

constexpr const char* kFfmpegBin = "/usr/bin/ffmpeg";

bool ffmpegAvailable() { return ::access(kFfmpegBin, X_OK) == 0; }

// fix14 c8 — concurrent reader stress. While the c7 pipeline is producing
// segments + rewriting the playlist, a parallel reader thread polls at
// 150ms (well below segment_duration_sec=2 so we routinely catch the
// muxer mid-rotation). We assert two invariants the `temp_file` HLS flag
// is supposed to guarantee:
//   1. Every successful playlist read starts with `#EXTM3U` (no torn
//      writes — atomic POSIX rename swaps a fully-written file in).
//   2. Every referenced .ts file's first byte is 0x47 (MPEG-TS sync).
//
// If a reader sees a half-written playlist or a zero-length segment,
// downstream nginx/clients would too — and that's the bug class this
// test pins down. The cfg-validator unit tests can't cover it because
// the regression would be a *muxer flag* error, not a config error.

struct TmpDir {
    fs::path path;
    TmpDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto cand = base / ("hls_concurrent_test_"
                                       + std::to_string(::getpid())
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
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        return out;
    }
    out.pid     = pid;
    out.stdout_ = pipefd[0];
    return out;
}

// Read a file fully via std::ifstream (open() under the hood — atomic at
// the inode level on Linux, so a rename racing with us either lands us on
// the old inode or the new one, never both).
bool readWhole(const fs::path& p, std::string& out_body) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out_body = ss.str();
    return true;
}

bool startsWith(const std::string& s, const std::string& pfx) {
    return s.size() >= pfx.size()
        && std::memcmp(s.data(), pfx.data(), pfx.size()) == 0;
}

} // namespace

TEST(HlsOutputItest, ConcurrentReadersNeverSeePartialFiles) {
    if (!ffmpegAvailable())
        GTEST_SKIP() << "ffmpeg binary not present";

    TmpDir d;

    HlsCfg cfg;
    cfg.output_dir                = d.path.string();
    cfg.segment_duration_sec      = 2;
    cfg.playlist_size             = 3;
    cfg.delete_segments_after_sec = 30;
    cfg.playlist_filename         = "stream.m3u8";
    cfg.segment_filename_pattern  = "seg_%05d.ts";

    HlsOutput out(std::move(cfg));
    ASSERT_TRUE(out.start());

    auto enc = spawnTsEncoder();
    ASSERT_GT(enc.pid, 0);
    ASSERT_GE(enc.stdout_, 0);

    // ── Reader thread ────────────────────────────────────────────────────
    std::atomic<bool> reader_stop{false};
    std::atomic<int>  playlist_ok{0};
    std::atomic<int>  playlist_partial{0};
    std::atomic<int>  playlist_missing{0};
    std::atomic<int>  segment_ok{0};
    std::atomic<int>  segment_partial{0};
    std::atomic<int>  segment_missing{0};

    std::thread reader([&]() {
        const fs::path playlist = d.path / "stream.m3u8";
        const std::regex seg_re(R"((seg_\d+\.ts))");

        while (!reader_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(150ms);

            std::string body;
            if (!readWhole(playlist, body)) {
                playlist_missing.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (body.empty() || !startsWith(body, "#EXTM3U")) {
                playlist_partial.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            playlist_ok.fetch_add(1, std::memory_order_relaxed);

            // Walk all segment refs and pick the most recently listed —
            // that's the one most likely to be racing with a fresh write.
            std::string seg_name;
            for (auto it = std::sregex_iterator(body.begin(), body.end(), seg_re);
                 it != std::sregex_iterator(); ++it) {
                seg_name = (*it)[1].str();
            }
            if (seg_name.empty()) continue;

            std::string seg_body;
            if (!readWhole(d.path / seg_name, seg_body)) {
                // Listed in playlist but file gone — could be a sliding-
                // window deletion that fired between playlist read and
                // segment open. Distinct counter, not a test failure.
                segment_missing.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (seg_body.empty() ||
                static_cast<unsigned char>(seg_body[0]) != 0x47) {
                segment_partial.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            segment_ok.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ── Pump bytes (12s), same shape as c7 ──────────────────────────────
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    std::vector<uint8_t> buf(64 * 1024);
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(enc.stdout_, buf.data(), buf.size());
        if (n <= 0) break;
        Packet p;
        p.data.assign(buf.data(), buf.data() + n);
        out.send(p);
    }

    ::kill(-enc.pid, SIGKILL);
    int status = 0;
    ::waitpid(enc.pid, &status, 0);
    enc.pid = -1;
    ::close(enc.stdout_);
    enc.stdout_ = -1;

    std::this_thread::sleep_for(500ms);
    out.stop();

    reader_stop.store(true, std::memory_order_relaxed);
    reader.join();

    // ── Invariants ───────────────────────────────────────────────────────
    // The reader must have done some real work — otherwise the test is a
    // no-op and we'd never see a regression.
    EXPECT_GE(playlist_ok.load(), 20)
        << "reader didn't observe enough playlists ("
        << playlist_ok.load() << ") — was the muxer running?";
    EXPECT_GE(segment_ok.load(),  10)
        << "reader didn't observe enough segments — was hls_segment_filename right?";

    // The two atomic-rename invariants. ANY non-zero count means a reader
    // saw a torn write, i.e. nginx-side clients would too.
    EXPECT_EQ(playlist_partial.load(), 0)
        << "reader saw " << playlist_partial.load()
        << " playlist(s) without #EXTM3U prefix — atomic rename broken";
    EXPECT_EQ(segment_partial.load(),  0)
        << "reader saw " << segment_partial.load()
        << " segment(s) without 0x47 sync byte — atomic rename broken";
    // playlist_missing / segment_missing are informational only — both
    // are legitimate at startup (before write_header) and across the
    // sliding-window-delete window respectively.
}
