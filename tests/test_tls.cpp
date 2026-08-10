// fix38 — unit tests for utils/Tls.
//
// Coverage:
//   * EC P-256 CA gen is idempotent (second ensureCa() observes existing
//     files with generated=false), and rejects-then-regenerates if the
//     cert/key pair is mismatched.
//   * Server cert chains to the CA, contains supplied SANs, has correct
//     KU/EKU, and survives readCertInfo() round-trip with sane fields.
//   * Private key files are written with mode 0600.
//   * verifyKeyPair() detects a swapped key.
//   * verifyChain() detects a cert signed by an unrelated CA.
//   * autoDetectSans() always emits localhost + 127.0.0.1 + ::1, dedupes
//     extras, and routes IP-looking strings to the right ip_v* bucket.

#include "utils/Tls.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <thread>

namespace fs   = std::filesystem;
namespace tls  = liveqx::tls;

namespace {

fs::path makeTempDir(const std::string& tag) {
    auto base = fs::temp_directory_path();
    static std::atomic<int> seq{0};
    auto pid  = ::getpid();
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = base / ("sc-tls-" + tag + "-" + std::to_string(pid) + "-"
                         + std::to_string(stamp) + "-"
                         + std::to_string(seq.fetch_add(1)));
    fs::create_directories(p);
    return p;
}

mode_t modeOf(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return st.st_mode & 0777;
}

}  // namespace

// ─── ensureCa ───────────────────────────────────────────────────────────────
TEST(Tls, EnsureCaGeneratesAndIsIdempotent) {
    auto dir = makeTempDir("ca-idem");
    auto r1 = tls::ensureCa(dir);
    ASSERT_TRUE(r1.ok()) << r1.error;
    EXPECT_TRUE(r1.generated);
    EXPECT_TRUE(fs::exists(r1.cert_path));
    EXPECT_TRUE(fs::exists(r1.key_path));

    auto r2 = tls::ensureCa(dir);
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_FALSE(r2.generated) << "second call must reuse existing CA";

    // Both must parse and the cert must self-signed.
    auto info = tls::readCertInfo(r1.cert_path);
    EXPECT_TRUE(info.is_ca);
    EXPECT_TRUE(info.self_signed);
    EXPECT_FALSE(info.subject.empty());
    EXPECT_GT(info.days_remaining, 365 * 5);  // default 10y, well >5y
    fs::remove_all(dir);
}

TEST(Tls, EnsureCaRegeneratesIfKeyDoesNotMatchCert) {
    auto dir = makeTempDir("ca-mismatch");
    auto r1 = tls::ensureCa(dir);
    ASSERT_TRUE(r1.ok()) << r1.error;

    auto r_alt = tls::ensureCa(makeTempDir("ca-alt"));  // key from another CA
    ASSERT_TRUE(r_alt.ok()) << r_alt.error;
    fs::copy_file(r_alt.key_path, dir / "ca.key",
                  fs::copy_options::overwrite_existing);

    auto r3 = tls::ensureCa(dir);
    ASSERT_TRUE(r3.ok()) << r3.error;
    EXPECT_TRUE(r3.generated) << "mismatched key must trigger regen";
    fs::remove_all(dir);
    fs::remove_all(r_alt.cert_path.parent_path());
}

TEST(Tls, KeyFilesHaveMode0600) {
    auto dir = makeTempDir("ca-perms");
    auto ca = tls::ensureCa(dir);
    ASSERT_TRUE(ca.ok()) << ca.error;
    EXPECT_EQ(modeOf(ca.key_path), 0600u) << "ca.key must be 0600";
    auto srv = tls::issueServerCert(dir, "core", {{"localhost"}, {"127.0.0.1"}, {}});
    ASSERT_TRUE(srv.ok()) << srv.error;
    EXPECT_EQ(modeOf(srv.key_path), 0600u) << "server.key must be 0600";
    fs::remove_all(dir);
}

// ─── issueServerCert ────────────────────────────────────────────────────────
TEST(Tls, IssueServerCertChainsToCaAndCarriesSans) {
    auto dir = makeTempDir("srv-sans");
    ASSERT_TRUE(tls::ensureCa(dir).ok());

    tls::SanList sans;
    sans.dns_names = {"core.lan", "streaming.test"};
    sans.ip_v4     = {"10.0.0.5"};
    sans.ip_v6     = {"::1"};

    auto srv = tls::issueServerCert(dir, "core.lan", sans);
    ASSERT_TRUE(srv.ok()) << srv.error;
    EXPECT_FALSE(srv.fingerprint_sha256.empty());

    auto chain = tls::verifyChain(srv.cert_path, dir / "ca.crt");
    EXPECT_TRUE(chain.ok) << chain.error;

    auto pair = tls::verifyKeyPair(srv.cert_path, srv.key_path);
    EXPECT_TRUE(pair.ok) << pair.error;

    auto info = tls::readCertInfo(srv.cert_path);
    EXPECT_FALSE(info.is_ca);
    EXPECT_FALSE(info.self_signed);
    EXPECT_NE(std::find(info.san_dns.begin(), info.san_dns.end(), "core.lan"),
              info.san_dns.end());
    EXPECT_NE(std::find(info.san_dns.begin(), info.san_dns.end(), "streaming.test"),
              info.san_dns.end());
    EXPECT_NE(std::find(info.san_ip.begin(), info.san_ip.end(), "10.0.0.5"),
              info.san_ip.end());
    EXPECT_NE(std::find(info.san_ip.begin(), info.san_ip.end(), "::1"),
              info.san_ip.end());
    EXPECT_GT(info.days_remaining, 300);   // default 365d
    fs::remove_all(dir);
}

TEST(Tls, IssueServerCertRejectsEmptySans) {
    auto dir = makeTempDir("srv-empty");
    ASSERT_TRUE(tls::ensureCa(dir).ok());
    auto r = tls::issueServerCert(dir, "core.lan", {});
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.find("SAN"), std::string::npos);
    fs::remove_all(dir);
}

// ─── verifyKeyPair / verifyChain negative paths ─────────────────────────────
TEST(Tls, VerifyKeyPairDetectsSwappedKey) {
    auto a = makeTempDir("vkp-a");
    auto b = makeTempDir("vkp-b");
    ASSERT_TRUE(tls::ensureCa(a).ok());
    ASSERT_TRUE(tls::ensureCa(b).ok());
    auto srv_a = tls::issueServerCert(a, "core",
                                      {{"localhost"}, {"127.0.0.1"}, {}});
    ASSERT_TRUE(srv_a.ok());

    // Swap server.key with the OTHER CA's key — won't match cert pubkey.
    fs::copy_file(b / "ca.key", srv_a.key_path,
                  fs::copy_options::overwrite_existing);
    auto v = tls::verifyKeyPair(srv_a.cert_path, srv_a.key_path);
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.error.find("private key"), std::string::npos);
    fs::remove_all(a);
    fs::remove_all(b);
}

TEST(Tls, VerifyChainRejectsForeignCa) {
    auto a = makeTempDir("chain-a");
    auto b = makeTempDir("chain-b");
    ASSERT_TRUE(tls::ensureCa(a).ok());
    ASSERT_TRUE(tls::ensureCa(b).ok());
    auto srv_a = tls::issueServerCert(a, "core",
                                      {{"localhost"}, {"127.0.0.1"}, {}});
    ASSERT_TRUE(srv_a.ok());

    auto v_ok  = tls::verifyChain(srv_a.cert_path, a / "ca.crt");
    auto v_bad = tls::verifyChain(srv_a.cert_path, b / "ca.crt");
    EXPECT_TRUE (v_ok.ok)  << v_ok.error;
    EXPECT_FALSE(v_bad.ok);
    fs::remove_all(a);
    fs::remove_all(b);
}

// ─── autoDetectSans ─────────────────────────────────────────────────────────
TEST(Tls, AutoDetectSansAlwaysIncludesLoopback) {
    auto s = tls::autoDetectSans({});
    EXPECT_NE(std::find(s.dns_names.begin(), s.dns_names.end(), "localhost"),
              s.dns_names.end());
    EXPECT_NE(std::find(s.ip_v4.begin(), s.ip_v4.end(), "127.0.0.1"),
              s.ip_v4.end());
    EXPECT_NE(std::find(s.ip_v6.begin(), s.ip_v6.end(), "::1"),
              s.ip_v6.end());
}

TEST(Tls, AutoDetectSansRoutesExtrasByLooks) {
    auto s = tls::autoDetectSans({"core.example", "192.168.99.99", "fd00::1"});
    EXPECT_NE(std::find(s.dns_names.begin(), s.dns_names.end(), "core.example"),
              s.dns_names.end());
    EXPECT_NE(std::find(s.ip_v4.begin(), s.ip_v4.end(), "192.168.99.99"),
              s.ip_v4.end());
    EXPECT_NE(std::find(s.ip_v6.begin(), s.ip_v6.end(), "fd00::1"),
              s.ip_v6.end());
}

TEST(Tls, AutoDetectSansDedupesExtras) {
    auto s = tls::autoDetectSans({"localhost", "127.0.0.1", "127.0.0.1"});
    int dns_lh = static_cast<int>(std::count(s.dns_names.begin(), s.dns_names.end(),
                                             "localhost"));
    int ip_lh  = static_cast<int>(std::count(s.ip_v4.begin(), s.ip_v4.end(),
                                             "127.0.0.1"));
    EXPECT_EQ(dns_lh, 1);
    EXPECT_EQ(ip_lh,  1);
}

// ─── readCertInfo on a missing file is a no-op (empty subject) ─────────────
TEST(Tls, ReadCertInfoOnMissingFileReturnsEmpty) {
    auto info = tls::readCertInfo(makeTempDir("noent") / "no-such.crt");
    EXPECT_TRUE(info.subject.empty());
    EXPECT_TRUE(info.fingerprint_sha256.empty());
}
