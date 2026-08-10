#include "output/RtspOutput.h"

struct RtspOutput::Impl {
    // TODO: AVFormatContext* for RTSP
};

RtspOutput::RtspOutput(const std::string& url)
    : impl_(std::make_unique<Impl>()), url_(url) {}

RtspOutput::~RtspOutput() = default;

void RtspOutput::send(const Packet& pkt) {
    // TODO: av_write_frame via RTSP
    ++stats_.packets_sent;
    stats_.bytes_sent += pkt.data.size();
}

bool RtspOutput::isHealthy() const { return true; }

OutputStats RtspOutput::getStats() const { return stats_; }
