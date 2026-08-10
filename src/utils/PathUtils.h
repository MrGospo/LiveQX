#pragma once
#include <filesystem>
#include <string>

namespace PathUtils {

// Replace filesystem-unsafe characters with underscores while keeping
// UTF-8 (Cyrillic, etc.) intact. Strips path separators, control chars,
// leading dots/spaces and trailing whitespace. Used for deriving safe
// filenames from user-provided channel names.
std::string sanitizeForPath(const std::string& s);

// Atomic file write: dump `payload` to `target` via target.tmp + fsync +
// rename + dir-fsync, so that a `kill -9` mid-write either leaves the old
// `target` intact or replaces it wholesale. Cross-FS rename (EXDEV) falls
// back to copy+remove which is non-atomic but still preserves the old
// target until the copy succeeds. Throws std::runtime_error on any
// unrecoverable I/O failure.
//
// Used by ChannelInstance::persistConfig and GatewayManager — both rely on
// the on-disk config.json being durably consistent across crash recovery.
void atomicWriteFile(const std::filesystem::path& target,
                     const std::string& payload);

}  // namespace PathUtils
