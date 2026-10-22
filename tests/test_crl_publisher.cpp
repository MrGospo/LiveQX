// players/CrlPublisher + mqtt/BrokerReload — the revocation list the broker reads.
//
// What these tests protect (OpenSSL itself is the judge: the lists are checked
// the way a broker checks them, with CRL checking switched on):
//   * a revoked certificate is refused and every other one is accepted;
//   * EVERY generation of the device CA has a list in the file, empty or not —
//     without one, the broker would turn away all certificates of that
//     generation (shown here, not just asserted);
//   * an older generation's revocations are signed by that generation's key;
//   * a failed publication writes nothing: the old file stays in force;
//   * the file is rewritten on a schedule well before any list runs out, since
//     an expired list shuts out everyone;
//   * the broker is told after the file is written, and being unable to tell it
//     is reported without undoing the file;
//   * the signal goes only to a process that really is the broker.

#include "auth/KeyWrap.h"
#include "auth/MasterKey.h"
#include "mqtt/BrokerReload.h"
#include "players/CrlPublisher.h"
#include "utils/DeviceCa.h"
#include "utils/DeviceRootCa.h"

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
namespace tls = liveqx::tls;
using namespace liveqx::players;
using namespace std::chrono_literals;

extern char** environ;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct X509Free { void operator()(X509* x) const { X509_free(x); } };
using CertPtr = std::unique_ptr<X509, X509Free>;

CertPtr firstCert(const std::string& pem) {
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    CertPtr c(PEM_read_bio_X509(b, nullptr, nullptr, nullptr));
    BIO_free_all(b);
    return c;
}

std::string makeCsr() {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509_REQ* req = X509_REQ_new();
    X509_REQ_set_pubkey(req, key);
    X509_REQ_sign(req, key, EVP_sha256());
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string pem(d, static_cast<size_t>(n));
    BIO_free_all(b);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    return pem;
}

// How many revocation lists a PEM file holds.
int countLists(const std::string& pem) {
    int n = 0;
    for (std::size_t at = 0; (at = pem.find("-----BEGIN X509 CRL-----", at)) != std::string::npos; ++at) ++n;
    return n;
}

class CrlPublisherTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<liveqx::auth::MasterKey> mk_;
    std::unique_ptr<PlayersDb> db_;
    fs::path root_dir_;
    std::string root_pem_;
    std::int64_t now_{0};
    int reloads_{0};
    std::string reload_result_;

    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-crl-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
        now_ = std::time(nullptr);
        mk_ = std::make_unique<liveqx::auth::MasterKey>((dir_ / "master.key").string());
        ASSERT_TRUE(mk_->load());
        db_ = std::make_unique<PlayersDb>(dir_ / "players.db");
        ASSERT_TRUE(db_->open());
        root_dir_ = dir_ / "root";
        std::string err;
        ASSERT_TRUE(tls::createDeviceRoot(root_dir_, "pass", err, liveqx::auth::keywrap::KdfParams::minimal())) << err;
        root_pem_ = slurp(root_dir_ / "root.crt");
        addGeneration();
    }
    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void addGeneration() {
        std::string err, csr, inter;
        ASSERT_TRUE(tls::DeviceCa::createIntermediateRequest(dir_ / "ca", *mk_, csr, err)) << err;
        ASSERT_TRUE(tls::signDeviceIntermediate(root_dir_, "pass", csr, 1825, inter, err)) << err;
        ASSERT_TRUE(tls::DeviceCa::importIntermediate(dir_ / "ca", *mk_, inter, root_pem_, err)) << err;
    }

    CrlPublisherOptions options(const fs::path& file) {
        CrlPublisherOptions o;
        o.path = file;
        o.now = [this] { return now_; };
        o.generations = [this] { return tls::DeviceCa::generations(dir_ / "ca"); };
        o.open_generation = [this](int g, std::string& err) { return tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, g, err); };
        o.reload = [this] { ++reloads_; return reload_result_; };
        return o;
    }

    struct Issued {
        std::string id, cert, chain, serial;
        int generation{0};
    };
    // A paired player with a real certificate from `generation`, recorded in the database.
    Issued enroll(int generation, const std::string& name) {
        std::string err;
        auto ca = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, generation, err);
        EXPECT_TRUE(ca) << err;
        Issued out;
        auto created = db_->createPlayer(name, std::nullopt, "strict", 86400, now_);
        EXPECT_TRUE(created.has_value());
        out.id = created->player.id;
        tls::IssuedClientCert cert;
        EXPECT_TRUE(ca->signClientCsr(makeCsr(), out.id, 365, cert, err, now_)) << err;
        out.cert = cert.cert_pem;
        out.chain = cert.chain_pem;
        out.serial = cert.serial_hex;
        out.generation = cert.generation;
        auto claim = db_->claimToken(out.id, created->token, now_);
        CertRow row;
        row.serial_hex = cert.serial_hex; row.generation = cert.generation; row.player_id = out.id;
        row.issued_at = now_; row.not_before = cert.not_before_unix; row.not_after = cert.not_after_unix;
        EXPECT_TRUE(db_->finishEnrollment(claim.token_id, row, now_));
        return out;
    }

    // Does OpenSSL, checking the lists the way a broker does, accept this certificate?
    // Returns X509_V_OK or the reason it did not.
    int verdict(const std::string& crl_pem, const Issued& who, const std::string& ca_chain_pem) {
        X509_STORE* store = X509_STORE_new();
        CertPtr root = firstCert(root_pem_);
        X509_STORE_add_cert(store, root.get());
        BIO* b = BIO_new_mem_buf(crl_pem.data(), static_cast<int>(crl_pem.size()));
        while (X509_CRL* crl = PEM_read_bio_X509_CRL(b, nullptr, nullptr, nullptr)) {
            X509_STORE_add_crl(store, crl);
            X509_CRL_free(crl);
        }
        ERR_clear_error();
        BIO_free_all(b);
        X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);        // the leaf only, as the broker does
        CertPtr leaf = firstCert(who.cert);
        STACK_OF(X509)* chain = sk_X509_new_null();
        CertPtr inter = firstCert(ca_chain_pem);
        sk_X509_push(chain, inter.get());
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        X509_STORE_CTX_init(ctx, store, leaf.get(), chain);
        X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT);
        X509_STORE_CTX_set_time(ctx, 0, static_cast<time_t>(now_));
        const int ok = X509_verify_cert(ctx);
        const int err = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx);
        X509_STORE_CTX_free(ctx);
        sk_X509_free(chain);
        X509_STORE_free(store);
        ERR_clear_error();
        return err;
    }
};

}  // namespace

TEST_F(CrlPublisherTest, WithNoRevocationsEveryCertificateIsAcceptedAndTheListIsStillThere) {
    auto a = enroll(1, "a");
    CrlPublisher pub(*db_, options(dir_ / "broker" / "crl.pem"));
    auto st = pub.publishNow();
    ASSERT_TRUE(st.last_ok) << st.error;
    EXPECT_EQ(st.revoked, 0u);
    EXPECT_EQ(st.generations, 1u);
    const std::string file = slurp(dir_ / "broker" / "crl.pem");
    EXPECT_EQ(countLists(file), 1) << "an empty list, not no list";
    EXPECT_EQ(verdict(file, a, a.chain), X509_V_OK);
}

TEST_F(CrlPublisherTest, ARevokedCertificateIsRefusedAndOthersAreNot) {
    auto a = enroll(1, "a");
    auto b = enroll(1, "b");
    ASSERT_TRUE(db_->revokePlayer(a.id, "stolen", now_).has_value());

    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    auto st = pub.publishNow();
    ASSERT_TRUE(st.last_ok) << st.error;
    EXPECT_EQ(st.revoked, 1u);
    const std::string file = slurp(dir_ / "crl.pem");
    EXPECT_EQ(verdict(file, a, a.chain), X509_V_ERR_CERT_REVOKED);
    EXPECT_EQ(verdict(file, b, b.chain), X509_V_OK);
}

TEST_F(CrlPublisherTest, EveryGenerationHasAListAndAnOlderOnesRevocationsAreSignedByItsOwnKey) {
    auto old_screen = enroll(1, "old");
    auto old_stolen = enroll(1, "old-stolen");
    addGeneration();                                       // the intermediate is replaced; generation 1 certificates live on
    auto new_screen = enroll(2, "new");
    ASSERT_EQ(old_screen.generation, 1);
    ASSERT_EQ(new_screen.generation, 2);
    ASSERT_TRUE(db_->revokePlayer(old_stolen.id, "stolen", now_).has_value());

    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    auto st = pub.publishNow();
    ASSERT_TRUE(st.last_ok) << st.error;
    EXPECT_EQ(st.generations, 2u);
    const std::string file = slurp(dir_ / "crl.pem");
    EXPECT_EQ(countLists(file), 2);

    std::string err;
    auto g1 = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 1, err);
    auto g2 = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 2, err);
    EXPECT_EQ(verdict(file, old_stolen, g1->certPem()), X509_V_ERR_CERT_REVOKED) << "revoked after the change of generation";
    EXPECT_EQ(verdict(file, old_screen, g1->certPem()), X509_V_OK);
    EXPECT_EQ(verdict(file, new_screen, g2->certPem()), X509_V_OK);
}

TEST_F(CrlPublisherTest, WithoutAListForItsGenerationEveryCertificateOfThatGenerationIsTurnedAway) {
    // Why the file must hold a list for every generation, even an empty one.
    auto old_screen = enroll(1, "old");
    addGeneration();
    auto new_screen = enroll(2, "new");
    std::string err;
    auto g1 = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 1, err);
    auto g2 = tls::DeviceCa::openGeneration(dir_ / "ca", *mk_, 2, err);
    std::string only_gen1;
    ASSERT_TRUE(g1->generateCrl({}, 1, 30, only_gen1, err, now_)) << err;
    EXPECT_EQ(verdict(only_gen1, old_screen, g1->certPem()), X509_V_OK);
    EXPECT_EQ(verdict(only_gen1, new_screen, g2->certPem()), X509_V_ERR_UNABLE_TO_GET_CRL)
        << "no list for the issuer: not accepted";
}

TEST_F(CrlPublisherTest, EachPublicationCarriesANewListNumberAndTheUsualTimeToLive) {
    enroll(1, "a");
    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    ASSERT_TRUE(pub.publishNow().last_ok);
    const std::string first = slurp(dir_ / "crl.pem");
    now_ += 3600;
    auto st = pub.publishNow();
    ASSERT_TRUE(st.last_ok);
    const std::string second = slurp(dir_ / "crl.pem");
    EXPECT_NE(first, second) << "a fresh list every time";
    EXPECT_EQ(st.valid_until, now_ + 30LL * 86400);

    BIO* b = BIO_new_mem_buf(second.data(), static_cast<int>(second.size()));
    X509_CRL* crl = PEM_read_bio_X509_CRL(b, nullptr, nullptr, nullptr);
    BIO_free_all(b);
    ASSERT_TRUE(crl);
    auto* n = static_cast<ASN1_INTEGER*>(X509_CRL_get_ext_d2i(crl, NID_crl_number, nullptr, nullptr));
    std::int64_t number = 0;
    ASN1_INTEGER_get_int64(&number, n);
    ASN1_INTEGER_free(n);
    EXPECT_EQ(number, 2) << "numbers only go up";
    X509_CRL_free(crl);
    EXPECT_LT(CrlPublisherOptions{}.refresh_interval_s, std::int64_t{CrlPublisherOptions{}.next_update_days} * 86400 / 10)
        << "refreshed long before a list can run out";
}

TEST_F(CrlPublisherTest, AFailedPublicationLeavesTheOldFileInForceAndSaysWhy) {
    enroll(1, "a");
    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    ASSERT_TRUE(pub.publishNow().last_ok);
    const std::string before = slurp(dir_ / "crl.pem");
    const int reloads = reloads_;

    auto broken = options(dir_ / "crl.pem");
    broken.open_generation = [](int, std::string& err) -> std::unique_ptr<tls::DeviceCa> {
        err = "the key cannot be unsealed";
        return nullptr;
    };
    CrlPublisher failing(*db_, std::move(broken));
    auto st = failing.publishNow();
    EXPECT_FALSE(st.last_ok);
    EXPECT_NE(st.error.find("generation_1"), std::string::npos) << st.error;
    EXPECT_NE(st.error.find("cannot be unsealed"), std::string::npos);
    EXPECT_EQ(slurp(dir_ / "crl.pem"), before) << "nothing was written";
    EXPECT_EQ(reloads_, reloads) << "and the broker was not told to read something that did not change";
    EXPECT_FALSE(fs::exists(dir_ / "crl.pem.tmp"));
}

TEST_F(CrlPublisherTest, WithoutADeviceCaThereIsNothingToPublishAndNoFile) {
    auto o = options(dir_ / "crl.pem");
    o.generations = [] { return std::vector<int>{}; };
    CrlPublisher pub(*db_, std::move(o));
    auto st = pub.publishNow();
    EXPECT_FALSE(st.last_ok);
    EXPECT_EQ(st.error, "no_device_ca");
    EXPECT_FALSE(fs::exists(dir_ / "crl.pem"));
}

TEST_F(CrlPublisherTest, TheBrokerIsToldAfterTheFileIsWrittenAndFailingToTellIsReportedNotFatal) {
    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    auto ok = pub.publishNow();
    EXPECT_TRUE(ok.last_ok);
    EXPECT_EQ(reloads_, 1);
    EXPECT_TRUE(ok.reload_error.empty());

    reload_result_ = "process_not_running";
    auto bad = pub.publishNow();
    EXPECT_TRUE(bad.last_ok) << "the file stands: the broker reads it when it next starts";
    EXPECT_EQ(bad.reload_error, "process_not_running");
    EXPECT_EQ(reloads_, 2);
    EXPECT_TRUE(fs::exists(dir_ / "crl.pem"));
}

TEST_F(CrlPublisherTest, WithNobodyToTellTheFileIsStillWritten) {
    auto o = options(dir_ / "crl.pem");
    o.reload = nullptr;                                    // a broker on another machine
    CrlPublisher pub(*db_, std::move(o));
    auto st = pub.publishNow();
    EXPECT_TRUE(st.last_ok);
    EXPECT_TRUE(fs::exists(dir_ / "crl.pem"));
}

TEST_F(CrlPublisherTest, TheBackgroundThreadPublishesAtOnceAndAgainWhenAsked) {
    enroll(1, "a");
    auto o = options(dir_ / "crl.pem");
    o.refresh_interval_s = 3600;
    CrlPublisher pub(*db_, std::move(o));
    pub.start();
    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 300; ++i) { if (pred()) return true; std::this_thread::sleep_for(10ms); }
        return pred();
    };
    ASSERT_TRUE(waitFor([&] { return pub.status().last_publish_at != 0; })) << "at start";
    const auto first = pub.status().last_attempt_at;
    now_ += 5;
    pub.request();
    EXPECT_TRUE(waitFor([&] { return pub.status().last_attempt_at > first; })) << "on request";
    pub.stop();
}

TEST_F(CrlPublisherTest, ARevokedCertificateThatHasExpiredAnyWayLeavesTheList) {
    auto a = enroll(1, "a");
    ASSERT_TRUE(db_->revokePlayer(a.id, "stolen", now_).has_value());
    CrlPublisher pub(*db_, options(dir_ / "crl.pem"));
    ASSERT_EQ(pub.publishNow().revoked, 1u);
    now_ += 400LL * 86400;                                 // past the certificate's end
    EXPECT_EQ(pub.publishNow().revoked, 0u) << "an expired certificate needs no entry";
}

// ── telling the broker ──────────────────────────────────────────────────────
class BrokerReloadTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        static std::atomic<int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("lqx-reload-" + std::to_string(::getpid()) + "-" + std::to_string(seq.fetch_add(1)));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void writePid(const std::string& text) { std::ofstream(dir_ / "broker.pid") << text; }
};

TEST_F(BrokerReloadTest, EveryWayThePidFileCanBeWrongIsNamed) {
    using liveqx::mqtt::reloadBrokerFromPidFile;
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "missing.pid"), "pid_file_unreadable");
    writePid("");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid");
    writePid("12abc");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid");
    writePid("-5");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid");
    writePid("0");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid") << "never our own group";
    writePid("1");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid") << "never init";
    writePid("999999999999");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "bad_pid") << "too long to be a pid";
    writePid("4194000");
    EXPECT_EQ(reloadBrokerFromPidFile(dir_ / "broker.pid"), "process_not_running");
}

TEST_F(BrokerReloadTest, AProcessThatIsNotTheBrokerIsLeftAlone) {
    writePid(std::to_string(::getpid()) + "\n");
    EXPECT_EQ(liveqx::mqtt::reloadBrokerFromPidFile(dir_ / "broker.pid"), "not_the_broker")
        << "this very test process must not be hung up";
}

TEST_F(BrokerReloadTest, TheRealBrokerGetsTheSignal) {
    // A stand-in process with the expected name; SIGHUP ends it, which is how we see it arrive.
    pid_t pid = 0;
    char arg0[] = "sleep", arg1[] = "30";
    char* argv[] = {arg0, arg1, nullptr};
    ASSERT_EQ(posix_spawnp(&pid, "sleep", nullptr, nullptr, argv, environ), 0);
    std::this_thread::sleep_for(100ms);
    writePid(std::to_string(pid));
    EXPECT_EQ(liveqx::mqtt::reloadBrokerFromPidFile(dir_ / "broker.pid", "sleep"), "");
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGHUP);
}
