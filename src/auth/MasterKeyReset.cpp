// fix35 (ROADMAP_1 A3.11–A3.15) — destructive master-key reset.
// See MasterKeyReset.h for the contract; the body below is the
// implementation. Order of operations matters and is documented inline.

#include "auth/MasterKeyReset.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>
#include <sqlite3.h>

#include "auth/MasterKey.h"
#include "utils/Log.h"
#include "utils/Paths.h"
#include "utils/RuntimeLock.h"

namespace liveqx::auth {

namespace fs = std::filesystem;

namespace {

std::int64_t unixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Service detection — try to acquire the shared single-instance
// lockfile that the running daemon holds for its entire lifetime
// (see src/utils/RuntimeLock.{h,cpp}, wired in main.cpp before
// Log::init). flock(LOCK_EX|LOCK_NB) is the POSIX-mandated primitive
// for this pattern; it's released automatically when the holder's
// fd goes away (clean shutdown, kill -9, kernel panic). No PID-file
// races, no stale state to clean up.
//
// We deliberately DO NOT probe via SQLite's BEGIN EXCLUSIVE — the
// running daemon opens auth.db in default rollback-journal mode,
// which only takes the file lock for the duration of an active
// statement, not the whole uptime. That made the SQLite probe a false
// negative whenever the server happened to be idle between writes.
//
// Returns:
//   "ok"      — lockfile is free; either the daemon is stopped, or
//               there is no daemon configured to use this state dir.
//   "running" — flock returned EWOULDBLOCK; daemon is alive.
//   "error:<msg>" — couldn't open/create the lockfile (treat as fatal).
std::string probeServiceState(const fs::path& state_dir) {
    auto lock = liveqx::util::RuntimeLock::tryAcquire(
        state_dir / "liveqx.lock");
    if (lock.held()) {
        // Hand the lock back immediately — we just wanted to know
        // whether anyone else was holding it.
        lock.release();
        return "ok";
    }
    // RuntimeLock encodes EWOULDBLOCK as "another instance is running";
    // anything else is an open/permission failure on the lockfile path.
    if (lock.error().find("another instance") != std::string::npos) {
        return "running";
    }
    return "error:" + lock.error();
}

bool wipeEncryptedSecrets(const fs::path& auth_db,
                          std::vector<std::string>& wiped_out,
                          std::string& err_out) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(auth_db.string().c_str(), &db,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        err_out = std::string{"open: "} + (db ? sqlite3_errmsg(db) : "");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 5000);

    auto run = [&](const char* sql, const char* label,
                   bool count_changes) -> bool {
        char* e = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
            err_out = std::string{label} + ": " + (e ? e : "");
            if (e) sqlite3_free(e);
            return false;
        }
        if (count_changes && sqlite3_changes(db) > 0) {
            wiped_out.emplace_back(label);
        }
        return true;
    };

    // The three column-sets that the running server writes encrypted
    // ciphertext into. Everything else (Argon2 hashes, JWT IDs, audit
    // log, channel perms) stays untouched.
    bool ok = true;
    ok = ok && run("UPDATE ldap_config SET bind_password_encrypted=NULL",
                   "ldap_config.bind_password_encrypted", true);
    ok = ok && run("UPDATE smtp_config SET password_encrypted=NULL",
                   "smtp_config.password_encrypted", true);
    // jwt_secret is a single-row table; deleting it makes AuthService
    // regenerate a fresh secret on next start (which it already does
    // when no row is present — see "Generated fresh jwt_secret" log).
    ok = ok && run("DELETE FROM jwt_secret",
                   "jwt_secret.secret_encrypted", true);

    sqlite3_close(db);
    return ok;
}

// Atomic write of `bytes` to `path` with mode 0600. Pattern lifted from
// AuthCli.rotateMasterKey(): write to <path>.new (O_CREAT|O_EXCL),
// fsync, rename onto target.
bool atomicWrite0600(const fs::path& path,
                     const std::array<std::uint8_t, MasterKey::kKeyBytes>& bytes,
                     std::string& err_out) {
    const fs::path tmp = path.string() + ".new";

    // Clean up dangling .new from a prior crashed reset.
    std::error_code ec;
    fs::remove(tmp, ec);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        err_out = std::string{"open("} + tmp.string() + "): " + std::strerror(errno);
        return false;
    }
    const auto written = ::write(fd, bytes.data(), bytes.size());
    if (written != static_cast<ssize_t>(bytes.size())) {
        err_out = "short write to " + tmp.string();
        ::close(fd);
        fs::remove(tmp, ec);
        return false;
    }
    if (::fsync(fd) != 0) {
        LOG_WARN("MasterKeyReset: fsync failed on {}: {}",
                 tmp.string(), std::strerror(errno));
    }
    ::close(fd);
    ::chmod(tmp.c_str(), 0600);

    fs::rename(tmp, path, ec);
    if (ec) {
        err_out = "rename(" + tmp.string() + " -> " + path.string() + "): "
                  + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void appendAuditLine(const fs::path& log_dir,
                     const ResetReport& report) {
    std::error_code ec;
    fs::create_directories(log_dir, ec);
    const fs::path audit_path = log_dir / "audit.log";

    // ISO-8601 UTC timestamp ("2026-05-09T03:24:11Z") — same shape we
    // use elsewhere in the audit subsystem so log aggregators can parse
    // both streams identically.
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_POSIX_VERSION)
    gmtime_r(&now, &utc);
#else
    utc = *std::gmtime(&now);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

    std::string wiped_csv;
    for (std::size_t i = 0; i < report.wiped.size(); ++i) {
        if (i) wiped_csv += ',';
        wiped_csv += report.wiped[i];
    }
    if (wiped_csv.empty()) wiped_csv = "(none)";

    std::ofstream f(audit_path, std::ios::app);
    if (!f) {
        LOG_WARN("MasterKeyReset: cannot open audit log {} for append",
                 audit_path.string());
        return;
    }
    f << ts << " event=master_key_reset uid=" << ::getuid()
      << " backup=" << (report.backup_path.empty() ? std::string{"(none)"}
                                                   : report.backup_path.string())
      << " wiped=" << wiped_csv << "\n";
}

}  // namespace

ResetReport resetMasterKey(const liveqx::paths::Paths& paths,
                           bool confirmed) {
    ResetReport report;

    // (1) Confirmation gate. CLI layer is in charge of printing the
    // warning text — we just refuse silently here.
    if (!confirmed) {
        report.result = ResetResult::NoConfirmation;
        return report;
    }

    if (sodium_init() < 0) {
        report.result = ResetResult::GenerateFailed;
        report.error  = "sodium_init failed";
        return report;
    }

    const fs::path auth_db    = paths.authDb();
    const fs::path master_key = paths.masterKey();

    // (2) Service detection via the liveqx.lock file. Stopping
    // the server is mandatory — the live process holds the OLD master
    // key in memory and would silently re-encrypt rows after our wipe
    // if it stayed up.
    const auto state = probeServiceState(paths.stateDir());
    if (state == "running") {
        report.result = ResetResult::ServiceRunning;
        report.error  = "LiveQX is running. Stop the service "
                        "before resetting the master key.";
        return report;
    }
    if (state.rfind("error:", 0) == 0) {
        report.result = ResetResult::DbOpenFailed;
        report.error  = state;
        return report;
    }
    // If auth.db does NOT exist yet, that's fine — fresh install where
    // the operator wants to start from a clean key. We skip the wipe
    // step in that branch (no rows to wipe).

    // (3) Backup the existing master.key, if there is one. We use a
    // unix-second suffix; the .bak files don't need ns precision since
    // the reset is a manual operation and ns collisions would mean
    // two operators racing — already a pathology.
    std::error_code ec;
    if (fs::exists(master_key, ec)) {
        const std::string ts = std::to_string(unixSec());
        report.backup_path = master_key.string() + "." + ts + ".bak";
        fs::copy_file(master_key, report.backup_path,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            report.result = ResetResult::BackupFailed;
            report.error  = "copy_file(" + master_key.string() + " -> "
                            + report.backup_path.string() + "): "
                            + ec.message();
            return report;
        }
        // Best-effort lock-down of the backup — same posture as the
        // live key, so an operator's `ls -l state/` doesn't surface a
        // world-readable archive copy.
        ::chmod(report.backup_path.c_str(), 0600);
    }

    // (4) Wipe encrypted secrets. Only attempted when auth.db actually
    // exists; on a fresh install there's nothing to wipe.
    if (fs::exists(auth_db)) {
        if (!wipeEncryptedSecrets(auth_db, report.wiped, report.error)) {
            report.result = ResetResult::WipeFailed;
            return report;
        }
    }

    // (5) Generate the new key and write it atomically.
    std::array<std::uint8_t, MasterKey::kKeyBytes> fresh{};
    randombytes_buf(fresh.data(), fresh.size());

    // Make sure the parent directory exists; on a fresh install the
    // operator may have wiped state/ entirely before running reset.
    if (master_key.has_parent_path()) {
        fs::create_directories(master_key.parent_path(), ec);
    }

    if (!atomicWrite0600(master_key, fresh, report.error)) {
        sodium_memzero(fresh.data(), fresh.size());
        report.result = ResetResult::GenerateFailed;
        return report;
    }
    sodium_memzero(fresh.data(), fresh.size());

    // (6) Audit log. Failure to append is logged but not fatal — the
    // reset itself is already done; degrading audit visibility is
    // strictly less bad than telling the operator their reset failed.
    report.result = ResetResult::Success;
    appendAuditLine(paths.logDir(), report);

    return report;
}

}  // namespace liveqx::auth
