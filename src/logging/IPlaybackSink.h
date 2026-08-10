#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::logging {

struct PlaybackEvent {
    int         channel_id;
    std::string clip_path;
    std::string clip_type;        // "image" | "video"
    int64_t     started_at_ns;    // wall clock (system_clock)
    int64_t     ended_at_ns;
    double      played_sec;
    std::string transition_type;  // "hardcut" | "crossfade" | "dissolve" | "wipe"
    std::string status;           // "completed" | "skipped_user" | "removed" | "error"
    std::string error_reason;     // empty when status != "error"
};

class IPlaybackSink {
public:
    struct QueryParams {
        int                    channel_id  = 0;
        std::optional<int64_t> from_ns;
        std::optional<int64_t> to_ns;
        std::optional<int64_t> after_ns;
        int                    limit  = 100;  // hard cap 1000 inside impl
        int                    offset = 0;
    };

    struct PurgeParams {
        int                    channel_id  = 0;
        // null → -∞ / +∞. Открытый диапазон с обеих сторон = удалить всё
        // содержимое канала. Семантика интервала закрытая с обеих сторон
        // (started_at_ns ∈ [from, to]) — совпадает с query().
        std::optional<int64_t> from_ns;
        std::optional<int64_t> to_ns;
    };

    virtual ~IPlaybackSink() = default;

    // Non-blocking. Pushes to internal queue, drops on overflow,
    // increments dropped-counter exposed via statusJson().
    virtual void log(const PlaybackEvent& ev) = 0;

    // Synchronous. Implementations are thread-safe relative to log().
    virtual nlohmann::json query(const QueryParams& params) = 0;

    // Удалить записи воспроизведения в указанном диапазоне started_at_ns.
    // Реализация может удалять файлы партиций целиком, если все их записи
    // попадают в диапазон. Возвращает {"deleted_rows":N, "removed_files":M}.
    // Не блокирует writer-поток sink'а (log() остаётся неблокирующим).
    virtual nlohmann::json purge(const PurgeParams& params) = 0;

    // queue_depth, dropped_count, last_write_ns, sink_type, ...
    virtual nlohmann::json statusJson() const = 0;
};

}  // namespace liveqx::logging
