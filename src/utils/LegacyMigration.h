#pragma once
#include <cstddef>
#include <filesystem>

namespace LegacyMigration {

// fix7: one-shot migration from the pre-fix7 layout
//   global config.json with "channels": [...]
//   logs/ch{id}-{name}.log*
//   cache/{leaf}/...
// to the per-channel layout
//   channels/ch{id}-{name}/{config.json, logs/, cache/}
//
// Triggers only when:
//   1. main_cfg_path contains a non-empty "channels" array, AND
//   2. channels_root is missing OR has no sub-directories.
// Otherwise no-op (return 0).
//
// On full success, the "channels" key is erased from main_cfg_path
// (atomic rewrite). On partial failure the on-disk folders are kept
// and the main config is left untouched so a follow-up restart can
// retry. Returns the number of channels successfully migrated.
//
// Throws std::runtime_error only on unrecoverable I/O against the
// main config file itself (e.g. unreadable). Per-channel migration
// failures are logged and counted as not-migrated, but never throw.
std::size_t run(const std::filesystem::path& main_cfg_path,
                const std::filesystem::path& channels_root);

}  // namespace LegacyMigration
