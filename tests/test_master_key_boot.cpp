// Unit tests for auth/MasterKeyBoot — how the service obtains its master key
// at start: plain key, key from outside, or a passphrase-wrapped key that
// makes the process wait (sealed start).

#include "auth/MasterKeyBoot.h"

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/Paths.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPass = "correct horse battery staple";

int freePort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

class BootTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        root_ = fs::temp_directory_path() /
                ("lqx-boot-" + std::to_string(::getpid()) + "-" +
                 std::to_string(seq.fetch_add(1)));
        fs::create_directories(root_ / "state");
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
        ::unsetenv("CREDENTIALS_DIRECTORY");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
    }

    liveqx::paths::Paths makePaths(const json& cfg = json::object()) const {
        liveqx::paths::Args a;
        a.state_dir  = (root_ / "state").string();
        a.log_dir    = (root_ / "logs").string();
        a.plugin_dir = (root_ / "plugins").string();
        liveqx::paths::Paths::Env env;
        env.get_env = [](const char*) { return std::optional<std::string>{}; };
        env.exists  = [](const fs::path&) { return false; };
        return liveqx::paths::Paths::resolve(a, cfg, env);
    }

    fs::path keyPath() const { return root_ / "state" / "master.key"; }

    void wrapKeyFile(std::string_view pass) {
        sa::keywrap::Key k{};
        {
            std::ifstream in(keyPath(), std::ios::binary);
            in.read(reinterpret_cast<char*>(k.data()), k.size());
        }
        const auto file = sa::keywrap::wrap(k, pass, sa::keywrap::KdfParams::minimal());
        std::ofstream out(keyPath(), std::ios::binary | std::ios::trunc);
        out << file;
    }

    static json plainApiCfg(int port) {
        return {{"api_bind", "127.0.0.1"}, {"api_port", port},
                {"tls", {{"mode", "disabled"}}}};
    }
};

}  // namespace

TEST_F(BootTest, MissingKeyIsGeneratedForTheFileStore) {
    sa::MasterKey key(keyPath().string());
    const auto r = sa::bootMasterKey(key, makePaths(), json::object(),
                                     [] { return false; });
    EXPECT_EQ(r, sa::BootResult::Ready);
    EXPECT_TRUE(key.loaded());
    EXPECT_TRUE(fs::exists(keyPath()));
}

TEST_F(BootTest, UnknownKeyStoreFails) {
    sa::MasterKey key(keyPath().string());
    EXPECT_EQ(sa::bootMasterKey(key, makePaths({{"master_key_store", "vault"}}),
                                json::object(), [] { return false; }),
              sa::BootResult::Failed);
    EXPECT_FALSE(fs::exists(keyPath())) << "nothing may be generated on a bad setting";
}

TEST_F(BootTest, ExternalStoreNeverGeneratesAKey) {
    sa::MasterKey key(keyPath().string());
    EXPECT_EQ(sa::bootMasterKey(key, makePaths({{"master_key_store", "creds"}}),
                                json::object(), [] { return false; }),
              sa::BootResult::Failed);
    EXPECT_FALSE(fs::exists(keyPath()));
}

TEST_F(BootTest, CredsStoreFailsWhenTheKeyDidNotComeFromSystemd) {
    {
        sa::MasterKey tmp(keyPath().string());
        ASSERT_TRUE(tmp.load());
    }
    sa::MasterKey key(keyPath().string());
    EXPECT_EQ(sa::bootMasterKey(key, makePaths({{"master_key_store", "creds"}}),
                                json::object(), [] { return false; }),
              sa::BootResult::Failed);
}

TEST_F(BootTest, WrappedKeyWaitsForThePassphraseOverHttp) {
    {
        sa::MasterKey tmp(keyPath().string());
        ASSERT_TRUE(tmp.load());
    }
    wrapKeyFile(kPass);
    const int port = freePort();
    const auto cfg = plainApiCfg(port);

    sa::MasterKey key(keyPath().string());
    std::atomic<sa::BootResult> result{sa::BootResult::Failed};
    std::thread service([&] {
        result = sa::bootMasterKey(key, makePaths(cfg), cfg, [] { return false; });
    });

    // The sealed server answers on the API port; unlock it like the page does.
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(1, 0);
    bool up = false;
    for (int i = 0; i < 100 && !up; ++i) {
        auto st = c.Get("/api/seal/status");
        up = st && st->status == 200;
        if (!up) std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(up) << "the sealed server never came up";
    EXPECT_FALSE(key.loaded());

    auto u = c.Post("/api/seal/unlock", json{{"passphrase", kPass}}.dump(), "application/json");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->status, 200);
    service.join();

    EXPECT_EQ(result.load(), sa::BootResult::Ready);
    EXPECT_TRUE(key.loaded());

    // The port must be free again for the real service.
    httplib::Server again;
    EXPECT_TRUE(again.bind_to_port("127.0.0.1", port))
        << "the sealed server must release the port after unlocking";
}

TEST_F(BootTest, WrappedKeyGivesUpWhenAskedToStop) {
    {
        sa::MasterKey tmp(keyPath().string());
        ASSERT_TRUE(tmp.load());
    }
    wrapKeyFile(kPass);
    const auto cfg = plainApiCfg(freePort());

    sa::MasterKey key(keyPath().string());
    std::atomic<bool> stop{false};
    std::thread t([&] { std::this_thread::sleep_for(300ms); stop = true; });
    const auto r = sa::bootMasterKey(key, makePaths(cfg), cfg, [&] { return stop.load(); });
    t.join();
    EXPECT_EQ(r, sa::BootResult::Aborted);
    EXPECT_FALSE(key.loaded());
}

TEST_F(BootTest, WrappedKeyFailsWhenThePortCannotBeBound) {
    {
        sa::MasterKey tmp(keyPath().string());
        ASSERT_TRUE(tmp.load());
    }
    wrapKeyFile(kPass);

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
    const int port = ntohs(addr.sin_port);

    const auto cfg = plainApiCfg(port);
    sa::MasterKey key(keyPath().string());
    EXPECT_EQ(sa::bootMasterKey(key, makePaths(cfg), cfg, [] { return false; }),
              sa::BootResult::Failed);
    ::close(blocker);
}
