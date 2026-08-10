#include "gateway/IGateway.h"

#include <stdexcept>
#include <utility>

#include "gateway/DemuxGateway.h"
#include "gateway/Gateway.h"
#include "gateway/RemuxGateway.h"
#include "gateway/TranscodeGateway.h"

namespace liveqx::gateway {

std::unique_ptr<IGateway>
makeGateway(int id, std::string name, GatewayCfg cfg) {
    switch (cfg.mode) {
        case GatewayMode::Passthrough:
            return std::make_unique<Gateway>(id, std::move(name), std::move(cfg));
        case GatewayMode::Demux:
            return std::make_unique<DemuxGateway>(id, std::move(name), std::move(cfg));
        case GatewayMode::Remux:
            return std::make_unique<RemuxGateway>(id, std::move(name), std::move(cfg));
        case GatewayMode::Transcode:
            return std::make_unique<TranscodeGateway>(id, std::move(name), std::move(cfg));
    }
    throw std::invalid_argument("gateway: unknown mode");
}

} // namespace liveqx::gateway
