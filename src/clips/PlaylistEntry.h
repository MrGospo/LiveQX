#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "clips/LiveClip.h"
#include "clips/PlaylistItem.h"

namespace liveqx::playlist {

// fix13 c4 — playlist-entry parser. Recognizes the new
// `"type": "live"` discriminator alongside the existing file entries
// (whose `type`, if present, is one of "video" | "image" | "file";
// legacy entries without `type` keep working as file entries).
//
// The parser is deliberately decoupled from ChannelInstance — the
// integration into ChannelInstance::buildClipFromItem lands in c5+/c9
// once Timeline's prepare-callback can drive the warm-up window. Here
// we just normalize the JSON shape and surface field-level errors so
// REST handlers (PUT/PATCH /playlist) can return a precise 400.

enum class EntryKind { File, Live };

struct LiveEntryCfg {
    std::string         id;
    nlohmann::json      input;            // raw — fed to LiveInputFactory::build()
    LiveClip::Cfg       live;             // duration_ns, warm_up_ns, loss_threshold_ns, id
    std::string         fallback_on_loss; // "fallback_clip" | "freeze" | "black"
    nlohmann::json      transition_in_raw;
    nlohmann::json      transition_out_raw;
};

struct PlaylistEntry {
    EntryKind     kind = EntryKind::File;
    PlaylistItem  file;   // valid iff kind == File
    LiveEntryCfg  live;   // valid iff kind == Live
};

// Inspect just the discriminator. Throws std::invalid_argument on a
// malformed entry envelope; UnsupportedTypeError (refines
// std::invalid_argument) for an unknown top-level `type` string so
// REST can map it to "400 unsupported type" specifically.
EntryKind parseEntryKind(const nlohmann::json& entry);

// Parse a single entry. default_photo_duration is used as the file
// entry's `duration` fallback when the field is absent.
PlaylistEntry parseEntry(const nlohmann::json& entry,
                         double default_photo_duration = 10.0);

// Parse the full playlist array. Throws on the first invalid entry,
// prefixing the message with `playlist[N]: ...`. Detects duplicate
// live `id` values (each must be unique within a playlist — REST
// /live-status reaches into the entry by id).
std::vector<PlaylistEntry> parsePlaylist(const nlohmann::json& arr,
                                         double default_photo_duration = 10.0);

} // namespace liveqx::playlist
