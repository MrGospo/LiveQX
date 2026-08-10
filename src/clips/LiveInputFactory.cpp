#include "clips/LiveInputFactory.h"

#include "clips/LiveInputClip.h"
#include "clips/MulticastInput.h"
#include "clips/MulticastInputCfg.h"
#include "clips/NdiInput.h"
#include "clips/NdiInputCfg.h"
#include "clips/RtmpInput.h"
#include "clips/RtmpInputCfg.h"
#include "clips/RtspInput.h"
#include "clips/RtspInputCfg.h"

namespace liveqx::live_input {

std::unique_ptr<LiveInputClip> build(const nlohmann::json& input_cfg,
                                     int out_width,
                                     int out_height) {
    const Kind kind = parseKind(input_cfg);
    switch (kind) {
        case Kind::Multicast: {
            auto cfg = liveqx::multicast::parseInputCfg(input_cfg);
            return std::make_unique<MulticastInput>(std::move(cfg),
                                                    out_width, out_height);
        }
        case Kind::Rtmp: {
            auto cfg = liveqx::rtmp::parseInputCfg(input_cfg);
            return std::make_unique<RtmpInput>(std::move(cfg),
                                               out_width, out_height);
        }
        case Kind::Rtsp: {
            auto cfg = liveqx::rtsp::parseInputCfg(input_cfg);
            return std::make_unique<RtspInput>(std::move(cfg),
                                               out_width, out_height);
        }
        case Kind::Ndi: {
            auto cfg = liveqx::ndi::parseInputCfg(input_cfg);
            return std::make_unique<liveqx::ndi::NdiInput>(
                std::move(cfg), out_width, out_height);
        }
    }
    throw UnsupportedTypeError("live_input: kind dispatch fell through");
}

} // namespace liveqx::live_input
