#pragma once

#include "IPlaybackSink.h"

namespace liveqx::logging {

// Sink for channels with playback_log.sink == "none" (or absent). All
// operations are O(1) no-ops; statusJson() reports sink_type="none" so
// monitoring can distinguish a disabled channel from a misconfigured one.
class NullPlaybackSink final : public IPlaybackSink {
public:
    void log(const PlaybackEvent&) override {}

    nlohmann::json query(const QueryParams&) override {
        return nlohmann::json{
            {"events", nlohmann::json::array()},
            {"next_after_ns", nullptr},
        };
    }

    nlohmann::json purge(const PurgeParams&) override {
        return nlohmann::json{{"deleted_rows", 0}, {"removed_files", 0}};
    }

    nlohmann::json statusJson() const override {
        return nlohmann::json{
            {"sink_type", "none"},
            {"queue_depth", 0},
            {"dropped_count", 0},
            {"last_write_ns", nullptr},
        };
    }
};

}  // namespace liveqx::logging
