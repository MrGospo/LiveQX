#include "clips/PlaylistEntry.h"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

#include "clips/LiveInputFactory.h"

namespace liveqx::playlist {

namespace {

PlaylistEntry parseFileEntry(const nlohmann::json& j, double default_dur) {
    PlaylistEntry e;
    e.kind = EntryKind::File;
    e.file.path = j.value("path", std::string{});
    if (e.file.path.empty()) {
        throw std::invalid_argument(
            "file entry: 'path' is required");
    }
    e.file.display_duration = j.value("duration", default_dur);
    if (!(e.file.display_duration > 0.0)) {
        throw std::invalid_argument(
            "file entry: 'duration' must be > 0");
    }
    return e;
}

PlaylistEntry parseLiveEntry(const nlohmann::json& j) {
    PlaylistEntry e;
    e.kind = EntryKind::Live;

    e.live.id = j.value("id", std::string{});
    if (e.live.id.empty()) {
        throw std::invalid_argument(
            "live entry: 'id' is required and must be non-empty");
    }

    if (!j.contains("input") || !j["input"].is_object()) {
        throw std::invalid_argument(
            "live entry: 'input' object is required");
    }
    // Validates discriminator + raises UnsupportedTypeError on unknown
    // input.type so REST sees a 400 with the right reason.
    (void) liveqx::live_input::parseKind(j["input"]);
    e.live.input = j["input"];

    if (!j.contains("duration_sec")) {
        throw std::invalid_argument(
            "live entry: 'duration_sec' is required");
    }
    if (!j["duration_sec"].is_number()) {
        throw std::invalid_argument(
            "live entry: 'duration_sec' must be numeric");
    }
    const double dur_sec = j["duration_sec"].get<double>();
    if (!(dur_sec > 0.0)) {
        throw std::invalid_argument(
            "live entry: 'duration_sec' must be > 0");
    }
    e.live.live.duration_ns = static_cast<std::uint64_t>(dur_sec * 1e9);

    const double warm_sec = j.value("warm_up_sec", 5.0);
    if (warm_sec < 0.0) {
        throw std::invalid_argument(
            "live entry: 'warm_up_sec' must be >= 0");
    }
    e.live.live.warm_up_ns = static_cast<std::uint64_t>(warm_sec * 1e9);

    const double loss_ms = j.value("loss_threshold_ms", 2000.0);
    if (!(loss_ms > 0.0)) {
        throw std::invalid_argument(
            "live entry: 'loss_threshold_ms' must be > 0");
    }
    e.live.live.loss_threshold_ns = static_cast<std::uint64_t>(loss_ms * 1e6);

    e.live.live.id = e.live.id;

    e.live.fallback_on_loss = j.value("fallback_on_loss",
                                      std::string{"fallback_clip"});
    if (e.live.fallback_on_loss != "fallback_clip" &&
        e.live.fallback_on_loss != "freeze" &&
        e.live.fallback_on_loss != "black") {
        throw std::invalid_argument(
            "live entry: 'fallback_on_loss' must be one of "
            "fallback_clip | freeze | black");
    }

    if (j.contains("transition_in"))  e.live.transition_in_raw  = j["transition_in"];
    if (j.contains("transition_out")) e.live.transition_out_raw = j["transition_out"];

    return e;
}

} // namespace

EntryKind parseEntryKind(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        throw std::invalid_argument("playlist entry must be an object");
    }
    if (!entry.contains("type")) return EntryKind::File;          // legacy
    if (!entry["type"].is_string()) {
        throw std::invalid_argument(
            "playlist entry: 'type' must be a string");
    }
    const auto t = entry["type"].get<std::string>();
    if (t == "live") return EntryKind::Live;
    if (t == "video" || t == "image" || t == "file") return EntryKind::File;
    throw liveqx::live_input::UnsupportedTypeError(
        "playlist entry: unsupported type '" + t + "'");
}

PlaylistEntry parseEntry(const nlohmann::json& entry,
                         double default_photo_duration) {
    const auto kind = parseEntryKind(entry);
    return (kind == EntryKind::Live)
        ? parseLiveEntry(entry)
        : parseFileEntry(entry, default_photo_duration);
}

std::vector<PlaylistEntry> parsePlaylist(const nlohmann::json& arr,
                                         double default_photo_duration) {
    if (!arr.is_array()) {
        throw std::invalid_argument("playlist must be an array");
    }
    std::vector<PlaylistEntry> out;
    out.reserve(arr.size());
    std::set<std::string> seen_live_ids;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        try {
            auto e = parseEntry(arr[i], default_photo_duration);
            if (e.kind == EntryKind::Live) {
                if (!seen_live_ids.insert(e.live.id).second) {
                    throw std::invalid_argument(
                        "duplicate live id '" + e.live.id + "'");
                }
            }
            out.push_back(std::move(e));
        } catch (const std::exception& ex) {
            throw std::invalid_argument(
                "playlist[" + std::to_string(i) + "]: " + ex.what());
        }
    }
    return out;
}

} // namespace liveqx::playlist
