#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

class LiveInputClip;

namespace liveqx::live_input {

// fix13 c2: discriminator over cfg["input"]["type"]. PlaylistEntry of
// type "live" carries an `input` sub-object whose `type` selects which
// concrete LiveInputClip to instantiate. Adding a new transport (RTSP in
// fix15) is a one-line change here plus a parseInputCfg+ctor on the
// concrete side; nothing in Timeline/LiveClip/REST cares about the type
// once the LiveInputClip is built.
enum class Kind { Multicast, Rtmp, Rtsp, Ndi };

// Distinct exception so REST validators can return 400 with a precise
// reason ("unknown live-input type") instead of a generic invalid_argument.
struct UnsupportedTypeError : public std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

inline std::string_view kindToString(Kind k) noexcept {
    switch (k) {
        case Kind::Multicast: return "multicast";
        case Kind::Rtmp:      return "rtmp";
        case Kind::Rtsp:      return "rtsp";
        case Kind::Ndi:       return "ndi";
    }
    return "unknown";
}

// Inspects ONLY the discriminator. Sub-cfg validation happens later in
// build() (or callers' explicit parseInputCfg) so a REST PATCH can answer
// "is this even a known type?" without parsing the full transport block.
inline Kind parseKind(const nlohmann::json& input_cfg) {
    if (!input_cfg.is_object())
        throw std::invalid_argument("live_input: cfg must be a JSON object");
    if (!input_cfg.contains("type") || !input_cfg["type"].is_string())
        throw std::invalid_argument("live_input: 'type' (string) required");
    const auto& t = input_cfg["type"].get_ref<const std::string&>();
    if (t == "multicast") return Kind::Multicast;
    if (t == "rtmp")      return Kind::Rtmp;
    if (t == "rtsp")      return Kind::Rtsp;
    if (t == "ndi")       return Kind::Ndi;
    throw UnsupportedTypeError("live_input: unsupported type '" + t + "'");
}

// Constructs a ready-but-unprepared LiveInputClip. Caller is expected to
// call setLogger / setChannelId / setNumaNode and finally prepare() (the
// state machine for that lives in LiveClip — see fix13 c3).
//
// Defined out-of-line in LiveInputFactory.cpp because MulticastInput /
// RtmpInput pull in FFmpeg via their Impl pimpls; keeping the discriminator
// header-only lets unit tests verify type dispatch without linking libav*.
std::unique_ptr<LiveInputClip> build(const nlohmann::json& input_cfg,
                                     int out_width,
                                     int out_height);

} // namespace liveqx::live_input
