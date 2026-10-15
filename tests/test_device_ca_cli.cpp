// Operator commands for the two-tier device CA ceremony:
//   --device-ca-root-init   (offline machine)   create the root
//   --device-ca-request     (server)            key + CSR of the next intermediate
//   --device-ca-root-sign   (offline machine)   sign the CSR with the root
//   --device-ca-import      (server)            install the signed intermediate
//
// The tests drive the same functions main() calls.

#include "utils/DeviceCaCli.h"

#include "auth/KeyProtectCli.h"
#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "utils/DeviceCa.h"

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
namespace tls = liveqx::tls;

namespace {

constexpr const char* kRootPass = "offline root passphrase";
constexpr const char* kMasterPass = "master key passphrase 1";

struct CliRun {
    int         rc{0};
    std::string out;
    std::string err;
};

sa::PassphraseSource script(std::vector<std::string> answers) {
    auto idx = std::make_shared<std::size_t>(0);
    return [answers = std::move(answers), idx](std::string_view) -> std::optional<std::string> {
        if (*idx >= answers.size()) return std::nullopt;
        return answers[(*idx)++];
    };
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

class CaCliTest : public ::testing::Test {
protected:
    fs::path dir_, rootDir_, caDir_, keyFile_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-cacli-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        rootDir_ = dir_ / "root";
        caDir_   = dir_ / "device_ca";
        keyFile_ = dir_ / "master.key";
        ::unsetenv("LIVEQX_MASTER_KEY");
        sa::MasterKey mk(keyFile_.string());
        ASSERT_TRUE(mk.load());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        ::unsetenv("LIVEQX_MASTER_KEY");
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

    CliRun rootInit(std::vector<std::string> a) {
        return capture([&] {
            return tls::deviceCaRootInit(rootDir_, script(std::move(a)), kw::KdfParams::minimal());
        });
    }
    CliRun request(std::vector<std::string> a = {}, const std::string& store = "file") {
        return capture([&] { return tls::deviceCaRequest(keyFile_, caDir_, store, script(std::move(a))); });
    }
    CliRun sign(const fs::path& csr, const fs::path& out, std::vector<std::string> a, int days = 1825) {
        return capture([&] {
            return tls::deviceCaRootSign(rootDir_, csr, out, days, script(std::move(a)));
        });
    }
    CliRun import(const fs::path& cert, const fs::path& root, std::vector<std::string> a = {},
                  const std::string& store = "file") {
        return capture([&] {
            return tls::deviceCaImport(keyFile_, caDir_, store, cert, root, script(std::move(a)));
        });
    }
};

}  // namespace

TEST_F(CaCliTest, RootInitCreatesTheRootAndPrintsWhatToKeep) {
    const auto r = rootInit({kRootPass, kRootPass});
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_TRUE(fs::exists(rootDir_ / "root.crt"));
    EXPECT_TRUE(fs::exists(rootDir_ / "root.key.sealed"));
    EXPECT_NE(r.out.find("root.crt"), std::string::npos);
    EXPECT_NE(r.out.find("root.key.sealed"), std::string::npos);
}

TEST_F(CaCliTest, RootInitRefusesWeakOrMismatchedPassphrases) {
    EXPECT_EQ(rootInit({"short", "short"}).rc, 1);
    EXPECT_EQ(rootInit({kRootPass, "something else entirely"}).rc, 1);
    EXPECT_EQ(rootInit({}).rc, 1);
    EXPECT_FALSE(fs::exists(rootDir_ / "root.crt"));
}

TEST_F(CaCliTest, RootInitDoesNotOverwriteAnExistingRoot) {
    ASSERT_EQ(rootInit({kRootPass, kRootPass}).rc, 0);
    const auto before = slurp(rootDir_ / "root.crt");
    const auto r = rootInit({"another long passphrase", "another long passphrase"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(slurp(rootDir_ / "root.crt"), before);
}

TEST_F(CaCliTest, FullCeremonyEndsWithAUsableTwoTierCa) {
    ASSERT_EQ(rootInit({kRootPass, kRootPass}).rc, 0);

    const auto req = request();
    ASSERT_EQ(req.rc, 0) << req.err;
    EXPECT_NE(req.out.find("BEGIN CERTIFICATE REQUEST"), std::string::npos)
        << "the CSR is printed so it can be carried to the offline machine";
    EXPECT_TRUE(fs::exists(caDir_ / "gen-1.pending" / "ca.csr"));

    const auto csr_file  = dir_ / "request.csr";
    const auto cert_file = dir_ / "intermediate.crt";
    spit(csr_file, req.out.substr(req.out.find("-----BEGIN CERTIFICATE REQUEST-----")));
    const auto signed_r = sign(csr_file, cert_file, {kRootPass});
    ASSERT_EQ(signed_r.rc, 0) << signed_r.err;
    ASSERT_TRUE(fs::exists(cert_file));

    const auto imp = import(cert_file, rootDir_ / "root.crt");
    ASSERT_EQ(imp.rc, 0) << imp.err;
    EXPECT_NE(imp.out.find("restart"), std::string::npos) << "tells the operator to restart the service";

    sa::MasterKey mk(keyFile_.string());
    ASSERT_TRUE(mk.load());
    std::string err;
    auto ca = tls::DeviceCa::open(caDir_, mk, err, /*allow_create=*/false);
    ASSERT_TRUE(ca) << err;
    EXPECT_TRUE(ca->twoTier());
}

TEST_F(CaCliTest, SignWithTheWrongPassphraseWritesNothing) {
    ASSERT_EQ(rootInit({kRootPass, kRootPass}).rc, 0);
    const auto req = request();
    const auto csr_file = dir_ / "request.csr";
    spit(csr_file, req.out.substr(req.out.find("-----BEGIN CERTIFICATE REQUEST-----")));
    const auto cert_file = dir_ / "intermediate.crt";
    const auto r = sign(csr_file, cert_file, {"not the root passphrase"});
    EXPECT_EQ(r.rc, 1);
    EXPECT_FALSE(fs::exists(cert_file));
}

TEST_F(CaCliTest, SignRefusesToOverwriteAnExistingOutputFile) {
    ASSERT_EQ(rootInit({kRootPass, kRootPass}).rc, 0);
    const auto req = request();
    const auto csr_file = dir_ / "request.csr";
    spit(csr_file, req.out.substr(req.out.find("-----BEGIN CERTIFICATE REQUEST-----")));
    const auto cert_file = dir_ / "intermediate.crt";
    spit(cert_file, "precious");
    EXPECT_EQ(sign(csr_file, cert_file, {kRootPass}).rc, 1);
    EXPECT_EQ(slurp(cert_file), "precious");
}

TEST_F(CaCliTest, SignAndImportReportMissingFilesPlainly) {
    ASSERT_EQ(rootInit({kRootPass, kRootPass}).rc, 0);
    EXPECT_EQ(sign(dir_ / "nope.csr", dir_ / "out.crt", {kRootPass}).rc, 1);
    EXPECT_EQ(import(dir_ / "nope.crt", rootDir_ / "root.crt").rc, 1);
}

TEST_F(CaCliTest, RequestWorksWithAPassphraseProtectedMasterKey) {
    // Wrap the master key in place.
    kw::Key k{};
    {
        std::ifstream in(keyFile_, std::ios::binary);
        in.read(reinterpret_cast<char*>(k.data()), k.size());
    }
    spit(keyFile_, kw::wrap(k, kMasterPass, kw::KdfParams::minimal()));

    EXPECT_EQ(request().rc, 1) << "without the passphrase it cannot seal the new key";
    EXPECT_FALSE(fs::exists(caDir_ / "gen-1.pending"));

    const auto r = request({kMasterPass});
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_TRUE(fs::exists(caDir_ / "gen-1.pending"));
}

TEST_F(CaCliTest, ServerSideCommandsRefuseAKeyTheyCannotOpen) {
    ::setenv("LIVEQX_MASTER_KEY", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", 1);
    const auto r = request();
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("LIVEQX_MASTER_KEY"), std::string::npos) << r.err;
    EXPECT_FALSE(fs::exists(caDir_ / "gen-1.pending"));
    ::unsetenv("LIVEQX_MASTER_KEY");

    const auto s = request({}, "creds");
    EXPECT_EQ(s.rc, 1);
    EXPECT_NE(s.err.find("master_key_store"), std::string::npos) << s.err;
}
