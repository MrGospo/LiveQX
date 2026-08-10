#include "stress/Scenario.h"

#include <filesystem>

#include <spdlog/spdlog.h>

#include "stress/ClipCorrupt.h"
#include "stress/NetworkBlackout.h"
#include "stress/RandomOutputFail.h"

namespace liveqx::stress {

std::unique_ptr<IScenario> makeScenario(const std::string& name,
                                         const nlohmann::json& options) {
    if (name == "random_output_fail") {
        return std::make_unique<RandomOutputFail>();
    }
    if (name == "clip_corrupt") {
        std::vector<std::filesystem::path> paths;
        if (options.is_object() && options.contains("paths")
            && options["paths"].is_array()) {
            for (const auto& p : options["paths"]) {
                if (p.is_string()) paths.emplace_back(p.get<std::string>());
            }
        }
        if (paths.empty()) {
            spdlog::warn("stress: clip_corrupt requires options.paths — skipping");
            return nullptr;
        }
        return std::make_unique<ClipCorrupt>(std::move(paths));
    }
    if (name == "network_blackout") {
        std::vector<std::string> ifaces;
        if (options.is_object() && options.contains("interfaces")
            && options["interfaces"].is_array()) {
            for (const auto& it : options["interfaces"]) {
                if (it.is_string()) ifaces.push_back(it.get<std::string>());
            }
        }
        if (ifaces.empty()) {
            spdlog::warn(
                "stress: network_blackout requires options.interfaces — skipping");
            return nullptr;
        }
        return std::make_unique<NetworkBlackout>(std::move(ifaces));
    }
    spdlog::warn("stress: unknown scenario '{}' — skipping", name);
    return nullptr;
}

}  // namespace liveqx::stress
