#pragma once
//
// NdiOutput config — переводит JSON-фрагмент output[] в типизированный
// конфиг. Поля минимальные на c6:
//   {
//     "type":         "ndi",
//     "ndi_name":     "Camera 1",         // обязательно: видно в NDI
//                                          //   discovery как "<HOST> (<NAME>)"
//     "groups":       "Studio,Public",    // optional, comma-separated
//     "lib_path":     "/opt/ndi/...so.5", // optional override
//     "clock_video":  true,               // default true (NDI clocks fps)
//     "clock_audio":  false               // default false
//   }
//

#include <nlohmann/json.hpp>
#include <string>

namespace liveqx::ndi {

struct OutputCfg {
    std::string ndi_name     = "LiveQX";
    std::string groups;
    std::string lib_path_override;
    bool        clock_video  = true;
    bool        clock_audio  = false;
};

inline OutputCfg parseOutputCfg(const nlohmann::json& j) {
    OutputCfg c;
    if (j.contains("ndi_name") && j["ndi_name"].is_string())
        c.ndi_name = j["ndi_name"].get<std::string>();
    if (j.contains("groups") && j["groups"].is_string())
        c.groups = j["groups"].get<std::string>();
    if (j.contains("lib_path") && j["lib_path"].is_string())
        c.lib_path_override = j["lib_path"].get<std::string>();
    c.clock_video = j.value("clock_video", c.clock_video);
    c.clock_audio = j.value("clock_audio", c.clock_audio);
    return c;
}

}  // namespace liveqx::ndi
