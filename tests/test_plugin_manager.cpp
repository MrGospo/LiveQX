// fix19 c11 — PluginManager validation + install/uninstall pipeline tests.
//
// We exercise the public surface only: REST handlers and main.cpp see the
// same status codes the tests assert on, so any regression in the
// gatekeeping (size cap, ELF magic, allow-list, force/i_understand,
// already-installed, uninstall-of-missing) shows up here without needing a
// real plugin .so. Two tests build a minimal but valid ET_DYN ELF64 header
// to drive the codepath past validation into dlopen — which then fails
// because the blob is not a real shared object — exercising the
// InitFailed / BadAbi branches' rollback behaviour (no leaked records,
// no leftover files in the plugin root).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "plugins/PluginManager.h"

using liveqx::plugins::InstallOptions;
using liveqx::plugins::InstallStatus;
using liveqx::plugins::PluginManager;
using liveqx::plugins::UninstallStatus;

namespace {

namespace fs = std::filesystem;

fs::path makeTmpDir(const char* tag) {
    auto p = fs::temp_directory_path()
           / ("liveqx_pm_" + std::string(tag) + "_" +
              std::to_string(::getpid()) + "_" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

// Minimal ET_DYN ELF64 LSB header (64 bytes). Just enough to satisfy
// PluginManager's isAcceptableElf() static check. dlopen() on this blob
// fails — that's intentional: it lets us drive the codepath all the way
// to the dlopen rollback branch without bundling a real .so.
std::vector<std::uint8_t> minimalDynElf() {
    std::vector<std::uint8_t> b(64, 0);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;   // ELFCLASS64
    b[5] = 1;   // ELFDATA2LSB
    b[6] = 1;   // EI_VERSION
    // e_type = ET_DYN (3) at offset 16
    b[16] = 0x03; b[17] = 0x00;
    // e_machine: x86_64 (0x3E) or aarch64 (0xB7) — pick the host's.
#if defined(__x86_64__)
    b[18] = 0x3E; b[19] = 0x00;
#elif defined(__aarch64__)
    b[18] = 0xB7; b[19] = 0x00;
#else
    b[18] = 0x3E; b[19] = 0x00;  // fall back to x86_64; non-host CI will skip
#endif
    return b;
}

void writeAllowList(const fs::path& dir,
                    const std::string& plugin_name,
                    const std::string& sha_hex) {
    fs::create_directories(dir);
    std::ofstream f(dir / (plugin_name + ".json"));
    f << nlohmann::json{{"sha256", sha_hex}}.dump(2);
}

}  // namespace

TEST(PluginManagerValidate, EmptyRegistryHasNoListings) {
    auto root = makeTmpDir("empty");
    PluginManager pm(root, /*log=*/{});
    EXPECT_EQ(pm.list().size(), 0u);
    EXPECT_EQ(pm.attributions().size(), 0u);
    EXPECT_FALSE(pm.hasOutputDriver("ndi"));
    EXPECT_FALSE(pm.hasInputDriver("ndi"));
    EXPECT_FALSE(pm.get("anything").has_value());
    fs::remove_all(root);
}

TEST(PluginManagerValidate, RejectsEmptyBlob) {
    auto root = makeTmpDir("empty_blob");
    PluginManager pm(root, /*log=*/{});
    auto rc = pm.install("p", {}, InstallOptions{});
    EXPECT_EQ(rc, InstallStatus::InvalidUpload);
    EXPECT_EQ(pm.list().size(), 0u);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, RejectsOversizedBlob) {
    auto root = makeTmpDir("oversize");
    PluginManager pm(root, /*log=*/{});
    // Just over the 32 MiB cap — kMaxPluginBytes is the contract.
    std::vector<std::uint8_t> blob(PluginManager::kMaxPluginBytes + 1, 0x7F);
    auto rc = pm.install("big", blob, InstallOptions{});
    EXPECT_EQ(rc, InstallStatus::InvalidUpload);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, RejectsNonElfBlob) {
    auto root = makeTmpDir("nonelf");
    PluginManager pm(root, /*log=*/{});
    // 100 random-ish bytes that don't start with ELF magic.
    std::vector<std::uint8_t> blob(128, 0xAB);
    auto rc = pm.install("plain", blob, InstallOptions{});
    EXPECT_EQ(rc, InstallStatus::InvalidUpload);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, RejectsInvalidPluginName) {
    auto root = makeTmpDir("badname");
    PluginManager pm(root, /*log=*/{});
    auto blob = minimalDynElf();

    EXPECT_EQ(pm.install("",            blob, InstallOptions{}),
              InstallStatus::InvalidUpload);
    EXPECT_EQ(pm.install("with space",  blob, InstallOptions{}),
              InstallStatus::InvalidUpload);
    EXPECT_EQ(pm.install("../escape",   blob, InstallOptions{}),
              InstallStatus::InvalidUpload);
    EXPECT_EQ(pm.install("with/slash",  blob, InstallOptions{}),
              InstallStatus::InvalidUpload);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, MissingAllowListRejectedWithoutForce) {
    auto root = makeTmpDir("noallow");
    PluginManager pm(root, /*log=*/{});
    pm.setAllowListDir(makeTmpDir("noallow_list"));   // exists but empty
    auto blob = minimalDynElf();
    auto rc = pm.install("absent", blob, InstallOptions{/*force=*/false});
    EXPECT_EQ(rc, InstallStatus::NotInAllowList);
    EXPECT_EQ(pm.list().size(), 0u);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, AllowListShaMismatchRejectedWithoutForce) {
    auto root      = makeTmpDir("badhash_root");
    auto allow_dir = makeTmpDir("badhash_allow");
    PluginManager pm(root, /*log=*/{});
    pm.setAllowListDir(allow_dir);

    // Allow-list says the plugin must be a different (zero) sha — hash
    // mismatch must be reported with BadHash, not NotInAllowList.
    writeAllowList(allow_dir, "mism",
                   std::string(64, '0'));   // 64-char hex sentinel

    auto blob = minimalDynElf();
    auto rc = pm.install("mism", blob, InstallOptions{/*force=*/false});
    EXPECT_EQ(rc, InstallStatus::BadHash);
    EXPECT_EQ(pm.list().size(), 0u);

    fs::remove_all(root);
    fs::remove_all(allow_dir);
}

TEST(PluginManagerValidate, ForceWithoutIUnderstandStillBlocked) {
    // The contract is `effective_force = force && i_understand`. Setting
    // only `force` is treated as a half-armed gun and must not bypass.
    auto root = makeTmpDir("halfforce");
    PluginManager pm(root, /*log=*/{});
    auto blob = minimalDynElf();
    auto rc = pm.install("half", blob,
                         InstallOptions{/*force=*/true,
                                        /*i_understand=*/false});
    EXPECT_EQ(rc, InstallStatus::NotInAllowList);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, ForcedDlopenOfFakeElfRollsBack) {
    // Operator armed force+i_understand on a malformed shared object.
    // Validation passes (size + ELF header are valid) but dlopen()
    // rejects the body — manager must report InitFailed *and* leave no
    // record / no leftover files behind.
    auto root = makeTmpDir("forced_root");
    PluginManager pm(root, /*log=*/{});
    auto blob = minimalDynElf();

    auto rc = pm.install("fake", blob,
                         InstallOptions{/*force=*/true,
                                        /*i_understand=*/true});
    // Either InitFailed (dlopen failed) or BadAbi (dlopen succeeded but
    // ABI symbol missing) is acceptable — the body is intentionally
    // not a real plugin. What matters is *no* successful install.
    EXPECT_TRUE(rc == InstallStatus::InitFailed ||
                rc == InstallStatus::BadAbi);
    EXPECT_EQ(pm.list().size(), 0u);

    // Rollback contract: nothing left under <root>/<name>/.
    EXPECT_FALSE(fs::exists(root / "fake"));

    fs::remove_all(root);
}

TEST(PluginManagerValidate, UninstallMissingReturnsNotInstalled) {
    auto root = makeTmpDir("uninst_missing");
    PluginManager pm(root, /*log=*/{});
    EXPECT_EQ(pm.uninstall("nope"), UninstallStatus::NotInstalled);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, AcceptEulaOnMissingReturnsFalse) {
    auto root = makeTmpDir("eula_missing");
    PluginManager pm(root, /*log=*/{});
    EXPECT_FALSE(pm.acceptEula("nope"));
    fs::remove_all(root);
}

TEST(PluginManagerValidate, ScanAndLoadOnEmptyTreeReturnsZero) {
    auto root = makeTmpDir("scan_empty");
    PluginManager pm(root, /*log=*/{});
    EXPECT_EQ(pm.scanAndLoad(), 0u);
    fs::remove_all(root);
}

TEST(PluginManagerValidate, ScanAndLoadSkipsIncompleteDirs) {
    // A subdir that's not a valid name, plus a valid-named subdir
    // missing its .so — both should be silently skipped, not aborted.
    auto root = makeTmpDir("scan_bad");
    fs::create_directories(root / "with space");          // bad name
    fs::create_directories(root / "noso");
    std::ofstream(root / "noso" / "manifest.json")
        << R"({"name":"noso","version":"1.0"})";
    PluginManager pm(root, /*log=*/{});
    EXPECT_EQ(pm.scanAndLoad(), 0u);
    EXPECT_EQ(pm.list().size(), 0u);
    fs::remove_all(root);
}
