// Unit tests for auth/SealedServer — the minimal HTTP(S) server that runs
// instead of the real service while the master key is passphrase-wrapped
// and has not been unlocked yet.
//
// Coverage:
//   * UnlockLimiter: free attempts, exponential delay, cap, reset on success,
//     per-address independence, forgetting idle addresses;
//   * status/unlock endpoints over plain HTTP and over HTTPS;
//   * no oracle during a lockout (even the right passphrase is refused);
//   * malformed / oversized bodies, every other path answers "sealed";
//   * waitUnlocked() returns when unlocked or when asked to give up.

#include "auth/SealedServer.h"
#include "auth/UnsealCli.h"

#include "api/TlsBootstrap.h"
#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/Paths.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
using nlohmann::json;
using namespace std::chrono_literals;

// ─── UnlockLimiter ──────────────────────────────────────────────────────────
namespace {
struct FakeClock {
    std::int64_t now{1000};
    sa::UnlockLimiter::Clock fn() { return [this] { return now; }; }
};
}  // namespace

TEST(UnlockLimiter, FirstAttemptsAreFree) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), /*free_attempts=*/3);
    EXPECT_EQ(l.retryAfter("1.1.1.1"), 0);
    l.recordFailure("1.1.1.1");
    l.recordFailure("1.1.1.1");
    EXPECT_EQ(l.retryAfter("1.1.1.1"), 0) << "two failures are still free";
}

TEST(UnlockLimiter, DelayDoublesAfterTheFreeAttempts) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), 3);
    for (int i = 0; i < 3; ++i) l.recordFailure("a");
    EXPECT_EQ(l.retryAfter("a"), 2);
    c.now += 2;
    EXPECT_EQ(l.retryAfter("a"), 0);
    l.recordFailure("a");
    EXPECT_EQ(l.retryAfter("a"), 4);
    c.now += 4;
    l.recordFailure("a");
    EXPECT_EQ(l.retryAfter("a"), 8);
}

TEST(UnlockLimiter, DelayIsCapped) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), 3, /*max_delay_s=*/300);
    for (int i = 0; i < 40; ++i) {
        l.recordFailure("a");
        c.now += 1000;   // always wait the delay out
    }
    l.recordFailure("a");
    EXPECT_EQ(l.retryAfter("a"), 300);
}

TEST(UnlockLimiter, SuccessForgetsTheAddress) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), 3);
    for (int i = 0; i < 5; ++i) l.recordFailure("a");
    ASSERT_GT(l.retryAfter("a"), 0);
    l.recordSuccess("a");
    EXPECT_EQ(l.retryAfter("a"), 0);
}

TEST(UnlockLimiter, AddressesAreIndependent) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), 3);
    for (int i = 0; i < 5; ++i) l.recordFailure("a");
    EXPECT_GT(l.retryAfter("a"), 0);
    EXPECT_EQ(l.retryAfter("b"), 0);
}

TEST(UnlockLimiter, IdleAddressIsForgotten) {
    FakeClock c;
    sa::UnlockLimiter l(c.fn(), 3, 300, /*forget_after_s=*/3600);
    for (int i = 0; i < 5; ++i) l.recordFailure("a");
    c.now += 3601;
    EXPECT_EQ(l.retryAfter("a"), 0);
    l.recordFailure("a");
    l.recordFailure("a");
    EXPECT_EQ(l.retryAfter("a"), 0) << "the old failures must not count any more";
}

// ─── SealedServer ───────────────────────────────────────────────────────────
namespace {

constexpr const char* kPass = "correct horse battery staple";

class SealedServerTest : public ::testing::Test {
protected:
    fs::path root_;
    std::unique_ptr<sa::MasterKey> key_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        root_ = fs::temp_directory_path() /
                ("lqx-sealed-" + std::to_string(::getpid()) + "-" +
                 std::to_string(seq.fetch_add(1)));
        fs::create_directories(root_);
        // Create a bare key, then wrap it in place (fast KDF for tests).
        {
            sa::MasterKey mk((root_ / "master.key").string());
            ASSERT_TRUE(mk.load());
        }
        sa::keywrap::Key k{};
        {
            std::ifstream in(root_ / "master.key", std::ios::binary);
            in.read(reinterpret_cast<char*>(k.data()), k.size());
        }
        const auto file = sa::keywrap::wrap(k, kPass, sa::keywrap::KdfParams::minimal());
        {
            std::ofstream out(root_ / "master.key", std::ios::binary | std::ios::trunc);
            out << file;
        }
        key_ = std::make_unique<sa::MasterKey>((root_ / "master.key").string());
        ASSERT_FALSE(key_->load());
        ASSERT_TRUE(key_->sealed());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    sa::SealedServer::Options plainOptions() const {
        sa::SealedServer::Options o;
        o.bind = "127.0.0.1";
        o.port = 0;   // pick a free port
        return o;
    }

    static httplib::Result post(httplib::Client& c, const std::string& body) {
        return c.Post("/api/seal/unlock", body, "application/json");
    }
};

}  // namespace

TEST_F(SealedServerTest, StatusSaysSealedUntilUnlocked) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    auto r = c.Get("/api/seal/status");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(json::parse(r->body).at("sealed"), true);

    auto u = post(c, json{{"passphrase", kPass}}.dump());
    ASSERT_TRUE(u);
    EXPECT_EQ(u->status, 200);
    auto after = c.Get("/api/seal/status");
    ASSERT_TRUE(after);
    EXPECT_EQ(json::parse(after->body).at("sealed"), false);
    EXPECT_TRUE(key_->loaded());
}

TEST_F(SealedServerTest, WrongPassphraseIsRefusedAndStaysSealed) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    auto r = post(c, json{{"passphrase", "nope"}}.dump());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(json::parse(r->body).at("error"), "wrong_passphrase");
    EXPECT_FALSE(key_->loaded());
    EXPECT_TRUE(key_->sealed());
}

TEST_F(SealedServerTest, LockoutRefusesEvenTheRightPassphrase) {
    auto o = plainOptions();
    o.free_attempts = 2;
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    for (int i = 0; i < 2; ++i) {
        auto r = post(c, json{{"passphrase", "wrong"}}.dump());
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    // Locked out now: the correct passphrase must not be tried (no oracle).
    auto locked = post(c, json{{"passphrase", kPass}}.dump());
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->status, 429);
    EXPECT_TRUE(locked->has_header("Retry-After"));
    EXPECT_GT(json::parse(locked->body).at("retry_after").get<int>(), 0);
    EXPECT_FALSE(key_->loaded());
}

TEST_F(SealedServerTest, MalformedBodiesAreRejected) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    for (const char* body : {"", "not json", "[]", "{}", "{\"passphrase\": 5}"}) {
        auto r = post(c, body);
        ASSERT_TRUE(r) << body;
        EXPECT_EQ(r->status, 400) << body;
    }
    auto big = post(c, json{{"passphrase", std::string(20000, 'a')}}.dump());
    ASSERT_TRUE(big);
    EXPECT_EQ(big->status, 413);
    EXPECT_FALSE(key_->loaded());
}

TEST_F(SealedServerTest, EverythingElseAnswersSealed) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    for (const char* path : {"/api/channels", "/api/auth/login", "/healthz", "/api/seal"}) {
        auto r = c.Get(path);
        ASSERT_TRUE(r) << path;
        EXPECT_EQ(r->status, 503) << path;
        EXPECT_EQ(json::parse(r->body).at("error"), "sealed") << path;
    }
    auto p = c.Post("/api/channels", "{}", "application/json");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->status, 503);
}

TEST_F(SealedServerTest, RootServesAPageWithAPassphraseField) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    httplib::Client c("127.0.0.1", s.port());

    for (const char* path : {"/", "/index.html", "/some/ui/route"}) {
        auto r = c.Get(path);
        ASSERT_TRUE(r) << path;
        EXPECT_EQ(r->status, 200) << path;
        EXPECT_NE(r->body.find("type=\"password\""), std::string::npos) << path;
        EXPECT_EQ(r->get_header_value("Cache-Control"), "no-store");
        EXPECT_FALSE(r->get_header_value("Content-Security-Policy").empty());
    }
}

TEST_F(SealedServerTest, WaitUnlockedReturnsTrueAfterUnlock) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    std::thread t([&] {
        std::this_thread::sleep_for(150ms);
        httplib::Client c("127.0.0.1", s.port());
        post(c, json{{"passphrase", kPass}}.dump());
    });
    EXPECT_TRUE(s.waitUnlocked([] { return false; }));
    t.join();
    EXPECT_TRUE(key_->loaded());
}

TEST_F(SealedServerTest, WaitUnlockedGivesUpWhenAsked) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    std::atomic<bool> stop{false};
    std::thread t([&] { std::this_thread::sleep_for(150ms); stop = true; });
    EXPECT_FALSE(s.waitUnlocked([&] { return stop.load(); }));
    t.join();
    EXPECT_FALSE(key_->loaded());
}

TEST_F(SealedServerTest, WorksOverHttps) {
    nlohmann::json cfg = nlohmann::json::object();
    liveqx::paths::Args a;
    a.state_dir = (root_ / "state").string();
    fs::create_directories(root_ / "state");
    liveqx::paths::Paths::Env env;
    env.get_env = [](const char*) { return std::optional<std::string>{}; };
    env.exists  = [](const fs::path&) { return false; };
    const auto paths = liveqx::paths::Paths::resolve(a, cfg, env);
    bool failed = false;
    const auto tls = resolveTlsBindings(cfg, paths, "127.0.0.1", failed);
    ASSERT_FALSE(failed);

    auto o = plainOptions();
    o.cert_path = tls.cert_path;
    o.key_path  = tls.key_path;
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    httplib::SSLClient c("127.0.0.1", s.port());
    c.enable_server_certificate_verification(false);
    auto st = c.Get("/api/seal/status");
    ASSERT_TRUE(st) << "TLS handshake with the sealed server must work";
    EXPECT_EQ(st->status, 200);
    auto u = c.Post("/api/seal/unlock", json{{"passphrase", kPass}}.dump(), "application/json");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->status, 200);
    EXPECT_TRUE(key_->loaded());
}

TEST_F(SealedServerTest, StartFailsWhenThePortIsTaken) {
    // Another program listening on the port (plain socket, no SO_REUSEPORT).
    const int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    ASSERT_EQ(::listen(blocker, 1), 0);
    socklen_t alen = sizeof addr;
    ::getsockname(blocker, reinterpret_cast<sockaddr*>(&addr), &alen);

    auto o = plainOptions();
    o.port = ntohs(addr.sin_port);
    sa::SealedServer s(*key_, o);
    std::string err;
    EXPECT_FALSE(s.start(err));
    EXPECT_FALSE(err.empty());
    ::close(blocker);
}

TEST_F(SealedServerTest, PortIsReusableByTheRealServerRightAfterStop) {
    sa::SealedServer s(*key_, plainOptions());
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    const int port = s.port();
    {
        httplib::Client c("127.0.0.1", port);
        ASSERT_TRUE(c.Get("/api/seal/status"));
    }
    s.stop();

    httplib::Server real;   // httplib defaults, as the control API uses
    EXPECT_TRUE(real.bind_to_port("127.0.0.1", port))
        << "connections left in TIME_WAIT must not block the next server";
}

// ── the local unseal socket (liveqx --unseal) ───────────────────────────────
namespace {

struct UnsealRun {
    int         rc{0};
    std::string out;
    std::string err;
};

sa::PassphraseSource oneAnswer(std::string a) {
    return [a](std::string_view) -> std::optional<std::string> { return a; };
}

UnsealRun runUnseal(const fs::path& socket, std::string pass) {
    std::stringstream o, e;
    auto* oo = std::cout.rdbuf(o.rdbuf());
    auto* oe = std::cerr.rdbuf(e.rdbuf());
    UnsealRun r;
    r.rc = sa::unsealViaSocket(socket, oneAnswer(std::move(pass)));
    std::cout.rdbuf(oo);
    std::cerr.rdbuf(oe);
    r.out = o.str();
    r.err = e.str();
    return r;
}

}  // namespace

TEST_F(SealedServerTest, LocalSocketUnlocksTheKey) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    const auto r = runUnseal(o.unseal_socket, kPass);
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_TRUE(key_->loaded());
    EXPECT_TRUE(s.waitUnlocked([] { return false; }));
}

TEST_F(SealedServerTest, LocalSocketRefusesAWrongPassphrase) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    const auto r = runUnseal(o.unseal_socket, "not the passphrase");
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("wrong"), std::string::npos) << r.err;
    EXPECT_FALSE(key_->loaded());
}

TEST_F(SealedServerTest, LocalSocketHasTheSameLockoutAsTheWebPage) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    o.free_attempts = 2;
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    EXPECT_EQ(runUnseal(o.unseal_socket, "wrong one").rc, 1);
    EXPECT_EQ(runUnseal(o.unseal_socket, "wrong two").rc, 1);
    const auto locked = runUnseal(o.unseal_socket, kPass);
    EXPECT_EQ(locked.rc, 1) << "even the right passphrase waits out the lockout";
    EXPECT_NE(locked.err.find("Too many"), std::string::npos) << locked.err;
    EXPECT_FALSE(key_->loaded());
}

TEST_F(SealedServerTest, LocalSocketIsPrivateAndRemovedOnStop) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    {
        sa::SealedServer s(*key_, o);
        std::string err;
        ASSERT_TRUE(s.start(err)) << err;
        struct stat st{};
        ASSERT_EQ(::stat(o.unseal_socket.c_str(), &st), 0);
        EXPECT_TRUE(S_ISSOCK(st.st_mode));
        EXPECT_EQ(st.st_mode & 0777, 0600u) << "only the service user and root may connect";
    }
    EXPECT_FALSE(fs::exists(o.unseal_socket)) << "the socket file must not outlive the server";
}

TEST_F(SealedServerTest, LocalSocketReplacesAStaleFileFromACrashedRun) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    { std::ofstream f(o.unseal_socket); f << "stale"; }
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    EXPECT_EQ(runUnseal(o.unseal_socket, kPass).rc, 0);
}

TEST_F(SealedServerTest, LocalSocketWorksEvenWhenThePathIsLongerThanSunPath) {
    // A state directory deep in a long path must not make the socket unusable.
    const auto deep = root_ / std::string(120, 'x');
    fs::create_directories(deep);
    auto o = plainOptions();
    o.unseal_socket = deep / "unseal.sock";
    ASSERT_GT(o.unseal_socket.string().size(), 107u);
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    EXPECT_TRUE(s.localSocketActive());
    EXPECT_EQ(runUnseal(o.unseal_socket, kPass).rc, 0);
}

TEST_F(SealedServerTest, ABrokenLocalSocketDoesNotTakeTheWebPageDown) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "no-such-directory" / "unseal.sock";
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << "the web page is the first way in: " << err;
    EXPECT_FALSE(s.localSocketActive());

    httplib::Client c("127.0.0.1", s.port());
    auto r = post(c, json{{"passphrase", kPass}}.dump());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(key_->loaded());
}

TEST_F(SealedServerTest, LocalSocketSurvivesGarbage) {
    auto o = plainOptions();
    o.unseal_socket = root_ / "unseal.sock";
    sa::SealedServer s(*key_, o);
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;

    {   // a client that sends rubbish, then another that sends nothing at all
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, o.unseal_socket.c_str(), sizeof(addr.sun_path) - 1);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
        const char junk[] = "this is not json\n";
        (void)!::write(fd, junk, sizeof junk - 1);
        char buf[256];
        ssize_t n = ::read(fd, buf, sizeof buf);
        EXPECT_GT(n, 0);
        EXPECT_NE(std::string(buf, static_cast<size_t>(n)).find("bad_request"), std::string::npos);
        ::close(fd);
    }
    EXPECT_EQ(runUnseal(o.unseal_socket, kPass).rc, 0) << "the server must keep serving";
}

TEST(UnsealCli, ReportsAMissingSocketPlainly) {
    std::stringstream e;
    auto* oe = std::cerr.rdbuf(e.rdbuf());
    const int rc = sa::unsealViaSocket("/nonexistent/dir/unseal.sock", oneAnswer("x"));
    std::cerr.rdbuf(oe);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(e.str().find("not sealed"), std::string::npos) << e.str();
}

TEST(UnsealCli, ReportsNoPassphraseAvailable) {
    std::stringstream e;
    auto* oe = std::cerr.rdbuf(e.rdbuf());
    const int rc = sa::unsealViaSocket("/nonexistent/unseal.sock",
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; });
    std::cerr.rdbuf(oe);
    EXPECT_EQ(rc, 1);
}
