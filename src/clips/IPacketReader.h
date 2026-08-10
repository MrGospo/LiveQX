#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

// Dependency injection seam for VideoClip's packet reader.
// Production code uses the default implementation (av_read_frame).
// Tests inject mocks to simulate I/O errors after seamless handoff —
// since the live AVFormatContext switches mid-stream, tests need to
// observe the switch (onContextSwitch) and fail reads against the new ctx.
struct IPacketReader {
    virtual int read(AVFormatContext* ctx, AVPacket* pkt) {
        return av_read_frame(ctx, pkt);
    }
    virtual void onContextSwitch(AVFormatContext* /*new_ctx*/) {}
    virtual ~IPacketReader() = default;
};
