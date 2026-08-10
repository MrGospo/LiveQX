#pragma once
//
// fix35 (ROADMAP_1 A3.11–A3.15) — destructive reset of the master key.
//
// `rotateMasterKey()` (in AuthCli.cpp) re-encrypts all encrypted blobs
// in auth.db with a freshly-generated key — non-destructive. This file
// implements the OPPOSITE: an out-of-band recovery procedure for the
// case where the operator has *lost* the master key and the encrypted
// secrets are unrecoverable. We:
//
//   1. Refuse without --confirm-reset (caller prints the warning).
//   2. Detect a running service via `BEGIN EXCLUSIVE` on auth.db. SQLite
//      returns SQLITE_BUSY/LOCKED if another process holds a write lock.
//      Stopping the service before reset is mandatory: the running
//      process holds the old key in memory and would silently keep
//      writing fresh ciphertext after the reset, leaving the DB in a
//      mixed state.
//   3. Back up the existing master.key as `master.key.<unix>.bak` so the
//      operator can still attempt manual decryption if they recover the
//      key elsewhere.
//   4. NULL out the encrypted columns we manage:
//        ldap_config.bind_password_encrypted
//        smtp_config.password_encrypted
//      and DELETE FROM jwt_secret (so the next start regenerates it).
//      Argon2id password hashes are not encrypted and stay intact —
//      operators do NOT lose user accounts.
//   5. Generate a fresh 32-byte key via libsodium and write it atomically
//      with mode 0600 (tmp file + rename, like AuthCli.rotateMasterKey).
//   6. Append an audit-log entry to logDir()/audit.log so an after-the-fact
//      auditor can see when the reset happened.
//
// Reset is intended for use AFTER `systemctl stop liveqx`. It is
// not a /api/* endpoint — destructive operations of this magnitude must
// require shell access on the host, not a stolen JWT.

#include <filesystem>
#include <string>
#include <vector>

namespace liveqx::paths { class Paths; }

namespace liveqx::auth {

enum class ResetResult {
    Success,
    NoConfirmation,    // caller did not pass confirmed=true
    ServiceRunning,    // auth.db locked by another process
    DbOpenFailed,      // cannot open auth.db at all
    BackupFailed,      // cp master.key → master.key.<ts>.bak failed
    WipeFailed,        // UPDATE/DELETE on encrypted columns failed
    GenerateFailed,    // cannot write the new master.key file
};

struct ResetReport {
    ResetResult              result{ResetResult::Success};

    // Populated on Success. Path of the saved-aside old key. Empty when
    // there was no pre-existing master.key (fresh install where the
    // operator triggers reset on a never-started DB).
    std::filesystem::path    backup_path;

    // Human-readable list of touched DB columns/tables. Empty list is
    // valid — a never-started DB has no rows in ldap_config / smtp_config /
    // jwt_secret yet, so nothing was wiped.
    std::vector<std::string> wiped;

    // Detail string for non-Success results. Caller surfaces this on
    // stderr so the operator gets a usable error.
    std::string              error;
};

// One-shot reset. Service must be stopped (we'll detect a running one
// via BEGIN EXCLUSIVE and refuse). Returns NoConfirmation when
// confirmed=false so the CLI layer can print the warning text.
ResetReport resetMasterKey(const liveqx::paths::Paths& paths,
                           bool confirmed);

}  // namespace liveqx::auth
