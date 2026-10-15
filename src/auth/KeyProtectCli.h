#pragma once
//
// Recovery-CLI commands that manage passphrase protection of the master key:
//   --protect-master-key       bare key file  -> wrapped file (LQXKEY1)
//   --unprotect-master-key     wrapped file   -> bare key file
//   --change-master-passphrase wrapped file   -> wrapped under another passphrase
//
// The data key K never changes, so nothing in the databases is touched. All
// three refuse to run while the service is running, when the key is not a
// plain file the tool may rewrite (master_key_store other than "file", or
// LIVEQX_MASTER_KEY set), and write the new file atomically after checking
// that it opens again to the same K. No backup copy of a bare key is left
// behind: it would defeat the point.
//
// Return values are process exit codes (0 = done).

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include "auth/KeyWrap.h"

namespace liveqx::auth {

// Supplies a passphrase for a prompt; nullopt when none can be obtained.
using PassphraseSource = std::function<std::optional<std::string>(std::string_view prompt)>;

// One line per call from `in` (trailing CR/LF removed). For --passphrase-stdin
// and tests.
PassphraseSource makeStreamPassphraseSource(std::istream& in);

// Prompts on the controlling terminal with echo off; nullopt without one.
PassphraseSource makeTerminalPassphraseSource();

inline constexpr std::size_t kMinPassphraseLength = 12;

// Asks twice for a NEW passphrase and applies the length rule. nullopt with
// `err` filled if none is available, the two differ or it is too short.
// The caller should wipe the returned string when done.
std::optional<std::string> promptNewPassphrase(const PassphraseSource& passphrase,
                                               std::string& err);

int protectMasterKey(const std::filesystem::path& key_file,
                     const std::filesystem::path& state_dir,
                     const std::string& key_store,
                     const PassphraseSource& passphrase,
                     const keywrap::KdfParams& kdf = keywrap::KdfParams::moderate());

int unprotectMasterKey(const std::filesystem::path& key_file,
                       const std::filesystem::path& state_dir,
                       const std::string& key_store,
                       const PassphraseSource& passphrase);

int changeMasterPassphrase(const std::filesystem::path& key_file,
                           const std::filesystem::path& state_dir,
                           const std::string& key_store,
                           const PassphraseSource& passphrase,
                           const keywrap::KdfParams& kdf = keywrap::KdfParams::moderate());

}  // namespace liveqx::auth
