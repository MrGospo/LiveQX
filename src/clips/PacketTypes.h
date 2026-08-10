#pragma once

struct AVPacket;

// Sentinel kind carried alongside packets through the demuxer queues.
// CROSSFADE_POINT  — seamless loop boundary (preload context handed off,
//                    no avformat seek needed; audio decoder performs crossfade).
// FLUSH            — fallback boundary when preload could not be opened
//                    (fmt_ctx was seeked back to start; decoder must flush
//                    its reference frames and fade-in audio).
enum class SentinelType { FLUSH, CROSSFADE_POINT };

struct PacketOrSentinel {
    AVPacket*    pkt           = nullptr;
    SentinelType sentinel_type = SentinelType::FLUSH;

    bool is_sentinel() const { return pkt == nullptr; }
};
