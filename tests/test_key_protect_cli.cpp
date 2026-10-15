// Unit tests for auth/KeyProtectCli — turning passphrase protection of the
// master key on and off, and changing the passphrase.
//
// The data key K never changes in any of these operations, so nothing in the
// databases is touched; the tests check exactly that, plus every refusal
// leaves the key file byte-for-byte as it was.

#include "auth/KeyProtectCli.h"

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/RuntimeLock.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
namespace sa = liveqx::auth;
namespace kw = liveqx::auth::keywrap;

namespace {

struct CliRun {
    int         rc{0};
    std::string out;
    std::string err;
};

// Feeds the prompts from a fixed list; returns nullopt when it runs out.
sa::PassphraseSource script(std::vector<std::string> answers) {
    auto idx = std::make_shared<std::size_t>(0);
    return [answers = std::move(answers), idx](std::string_view) -> std::optional<std::string> {
        if (*idx >= answers.size()) return std::nullopt;
        return answers[(*idx)++];
    };
}

class ProtectTest : public ::testing::Test {
protected:
    fs::path dir_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-protect-" + std::to_string(::getpid()) + "-" +
                std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        ::unsetenv("LIVEQX_MASTER_KEY");
        ::unsetenv("LIVEQX_MASTER_KEY_FILE");
        sa::MasterKey mk(keyPath().string());
        ASSERT_TRUE(mk.load());
        fingerprint_ = mk.fingerprint();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
    }

    fs::path keyPath() const { return dir_ / "master.key"; }

    static std::string slurp(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    template <class Fn>
    CliRun capture(Fn fn) {
        std::stringstream out, err;
        auto* oo = std::cout.rdbuf(out.rdbuf());
        auto* oe = std::cerr.rdbuf(err.rdbuf());
        CliRun r;
        r.rc = fn();
        std::cout.rdbuf(oo);
        std::cerr.rdbuf(oe);
        r.out = out.str();
        r.err = err.str();
        return r;
    }

    CliRun protect(std::vector<std::string> answers, const std::string& store = "file") {
        return capture([&] {
            return sa::protectMasterKey(keyPath(), dir_, store, script(std::move(answers)),
                                        kw::KdfParams::minimal());
        });
    }
    CliRun unprotect(std::vector<std::string> answers) {
        return capture([&] {
            return sa::unprotectMasterKey(keyPath(), dir_, "file", script(std::move(answers)));
        });
    }
    CliRun change(std::vector<std::string> answers) {
        return capture([&] {
            return sa::changeMasterPassphrase(keyPath(), dir_, "file", script(std::move(answers)),
                                              kw::KdfParams::minimal());
        });
    }

    // Opens the key file with a passphrase and returns its fingerprint ("" on failure).
    std::string openWith(const std::string& pass) {
        sa::MasterKey mk(keyPath().string());
        if (mk.load()) return mk.fingerprint();
        std::string err;
        if (!mk.unlock(pass, err)) return {};
        return mk.fingerprint();
    }

    std::string fingerprint_;
};

constexpr const char* kGood = "a long enough passphrase";

}  // namespace

// ── protect ─────────────────────────────────────────────────────────────────
TEST_F(ProtectTest, ProtectWrapsTheKeyAndKeepsTheSameKey) {
    const auto r = protect({kGood, kGood});
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find(fingerprint_), std::string::npos) << "prints the fingerprint";
    EXPECT_TRUE(kw::isWrapped(slurp(keyPath())));
    EXPECT_EQ(openWith(kGood), fingerprint_) << "K must be unchanged";
    EXPECT_EQ(openWith("another passphrase!!"), "");

    struct stat st{};
    ASSERT_EQ(::stat(keyPath().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST_F(ProtectTest, ProtectKeepsThePermissionsOfTheOriginalFile) {
    // The key may live in /etc/liveqx as root:liveqx 0640 so the service
    // (user liveqx) can read it; a rewrite by root must not lock it out.
    ASSERT_EQ(::chmod(keyPath().c_str(), 0640), 0);
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    struct stat st{};
    ASSERT_EQ(::stat(keyPath().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);
}

TEST_F(ProtectTest, UnprotectAndChangeKeepThePermissionsToo) {
    ASSERT_EQ(::chmod(keyPath().c_str(), 0640), 0);
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    ASSERT_EQ(change({kGood, "the new long passphrase", "the new long passphrase"}).rc, 0);
    struct stat st{};
    ASSERT_EQ(::stat(keyPath().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);
    ASSERT_EQ(unprotect({"the new long passphrase"}).rc, 0);
    ASSERT_EQ(::stat(keyPath().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);
}

TEST_F(ProtectTest, ProtectLeavesNoPlaintextCopyBehind) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    for (const auto& e : fs::directory_iterator(dir_)) {
        const auto n = e.path().filename().string();
        // liveqx.lock is the single-instance lock file, not a key copy.
        EXPECT_TRUE(n == "master.key" || n == "liveqx.lock")
            << "unexpected leftover file: " << n;
    }
}

TEST_F(ProtectTest, ProtectRefusesAShortPassphrase) {
    const auto before = slurp(keyPath());
    const auto r = protect({"short", "short"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("at least"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesWhenTheRepeatDiffers) {
    const auto before = slurp(keyPath());
    const auto r = protect({kGood, "a different long passphrase"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("do not match"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesWhenNoPassphraseIsAvailable) {
    const auto before = slurp(keyPath());
    EXPECT_EQ(protect({}).rc, 1);
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesAnAlreadyWrappedKey) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto before = slurp(keyPath());
    const auto r = protect({"yet another long one", "yet another long one"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("already"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesWhileTheServiceRuns) {
    const auto before = slurp(keyPath());
    auto lock = liveqx::util::RuntimeLock::tryAcquire(dir_ / "liveqx.lock");
    ASSERT_TRUE(lock.held());
    const auto r = protect({kGood, kGood});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("running"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesAKeyStoreItCannotRewrite) {
    const auto before = slurp(keyPath());
    const auto r = protect({kGood, kGood}, "creds");
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("master_key_store"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectRefusesWhenTheKeyComesFromTheEnvironment) {
    const auto before = slurp(keyPath());
    ::setenv("LIVEQX_MASTER_KEY", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", 1);
    const auto r = protect({kGood, kGood});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("LIVEQX_MASTER_KEY"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ProtectDoesNotCreateAKeyThatIsNotThere) {
    fs::remove(keyPath());
    const auto r = protect({kGood, kGood});
    EXPECT_EQ(r.rc, 1);
    EXPECT_FALSE(fs::exists(keyPath()));
}

// ── unprotect ───────────────────────────────────────────────────────────────
TEST_F(ProtectTest, UnprotectRestoresTheBareKey) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto r = unprotect({kGood});
    ASSERT_EQ(r.rc, 0) << r.err;
    const auto bytes = slurp(keyPath());
    EXPECT_EQ(bytes.size(), sa::MasterKey::kKeyBytes);
    EXPECT_EQ(openWith(""), fingerprint_) << "same K, now without a passphrase";
}

TEST_F(ProtectTest, UnprotectWithTheWrongPassphraseChangesNothing) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto before = slurp(keyPath());
    const auto r = unprotect({"this is not the passphrase"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, UnprotectRefusesABareKey) {
    const auto before = slurp(keyPath());
    const auto r = unprotect({kGood});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("not passphrase"), std::string::npos) << r.err;
    EXPECT_EQ(slurp(keyPath()), before);
}

// ── change ──────────────────────────────────────────────────────────────────
TEST_F(ProtectTest, ChangeMovesTheKeyToTheNewPassphrase) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto r = change({kGood, "the new long passphrase", "the new long passphrase"});
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_EQ(openWith("the new long passphrase"), fingerprint_) << "K must be unchanged";
    EXPECT_EQ(openWith(kGood), "") << "the old passphrase must stop working";
}

TEST_F(ProtectTest, ChangeWithTheWrongOldPassphraseChangesNothing) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto before = slurp(keyPath());
    const auto r = change({"not the old passphrase", "the new long passphrase",
                           "the new long passphrase"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ChangeRefusesAShortNewPassphrase) {
    ASSERT_EQ(protect({kGood, kGood}).rc, 0);
    const auto before = slurp(keyPath());
    const auto r = change({kGood, "short", "short"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(slurp(keyPath()), before);
}

TEST_F(ProtectTest, ChangeRefusesABareKey) {
    const auto before = slurp(keyPath());
    EXPECT_EQ(change({kGood, "the new long passphrase", "the new long passphrase"}).rc, 1);
    EXPECT_EQ(slurp(keyPath()), before);
}

// ── stdin source ────────────────────────────────────────────────────────────
TEST(PassphraseSourceTest, StreamSourceReadsOneLinePerPrompt) {
    std::istringstream in("first\nsecond\r\nthird");
    auto src = sa::makeStreamPassphraseSource(in);
    EXPECT_EQ(src("p").value_or("<none>"), "first");
    EXPECT_EQ(src("p").value_or("<none>"), "second") << "CRLF must be trimmed";
    EXPECT_EQ(src("p").value_or("<none>"), "third");
    EXPECT_FALSE(src("p").has_value());
}
