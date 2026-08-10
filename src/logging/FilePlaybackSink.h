#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "IPlaybackSink.h"
#include "utils/SpscQueue.h"

namespace liveqx::logging {

// Append-only JSONL sink. One file per UTC day:
//   {channel_dir}/playback/file-YYYY-MM-DD.jsonl
//
// Rotation is driven by the *event's* started_at_ns, not by writer-thread
// wall-clock — so backfill or burst events at midnight land in the right
// file deterministically. There is no automatic retention; operators rotate
// with logrotate or a cron sweep.
//
// Threading:
//   - log() pushes onto a 4096-slot SPSC queue and is non-blocking. On
//     overflow the event is dropped and a counter is bumped (visible in
//     statusJson). This protects RenderLoop / dispatcher threads from any
//     filesystem stall.
//   - One writer-thread drains the queue, opens/closes day-files, flushes
//     after each line so query() sees consistent state.
//   - query() runs on the REST handler thread; it reads files directly
//     and tolerates a partial last line if writer crashed mid-write.
class FilePlaybackSink final : public IPlaybackSink {
public:
    FilePlaybackSink(int channel_id, std::filesystem::path channel_dir);
    ~FilePlaybackSink() override;

    FilePlaybackSink(const FilePlaybackSink&) = delete;
    FilePlaybackSink& operator=(const FilePlaybackSink&) = delete;

    void log(const PlaybackEvent& ev) override;
    nlohmann::json query(const QueryParams& params) override;
    nlohmann::json purge(const PurgeParams& params) override;
    nlohmann::json statusJson() const override;

private:
    static constexpr size_t kQueueSize = 4096;
    static std::string dayString(int64_t ns);  // UTC YYYY-MM-DD

    void writerLoop();
    void rotateIfNeeded(const std::string& day);

    int                                  channel_id_;
    std::filesystem::path                playback_dir_;
    SpscQueue<PlaybackEvent, kQueueSize> queue_;
    std::atomic<uint64_t>                dropped_{0};
    std::atomic<int64_t>                 last_write_ns_{0};
    std::atomic<bool>                    running_{true};

    // Owned by writer-thread only.
    std::ofstream  current_file_;
    std::string    current_day_;
    std::thread    writer_;
};

}  // namespace liveqx::logging
