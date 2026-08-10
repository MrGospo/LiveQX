#pragma once
//
// NdiInput config — переводит JSON-фрагмент input["..."] в типизированный
// конфиг.
//
// fix19 c9 — multicast↔NDI bridge: реализуется не как отдельный
// gateway.kind=ndi, а композицией обычного Channel с
// input.type=ndi + output.type=multicast (или наоборот: input=multicast +
// output=ndi). Поскольку Channel уже несёт Compositor + Encoder + REST
// surface, отдельный gateway-class дублировал бы абстракцию без выгоды;
// design doc fix/fix19 пункт 26 это явно фиксирует.
//
// Поля минимальные на c8:
//   {
//     "type":               "ndi",
//     "source_name":        "MACHINE-A (Camera 1)",  // обязательно: точное
//                                                    // NDI-имя источника
//                                                    // (видно в discovery)
//     "url":                "192.168.1.10:5961",     // optional: явный
//                                                    // p_url_address для
//                                                    // обхода discovery
//     "groups":             "Studio,Public",         // optional
//     "lib_path":           "/opt/ndi/...so.5",      // optional override
//     "low_bandwidth":      false,                   // default false
//                                                    // (true → preview-rate)
//     "recv_name":          "LiveQX",                // local recv label
//     "reconnect_on_silence_sec": 10                 // 0 disables stall flag
//   }
//

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::ndi {

struct InputCfg {
    std::string source_name;                      // mandatory: NDI source name
    std::string url_address;                      // optional override
    std::string groups;
    std::string lib_path_override;
    std::string recv_name = "LiveQX";
    bool        low_bandwidth = false;
    int         reconnect_on_silence_sec = 10;    // 0 disables stall flag
};

inline InputCfg parseInputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("ndi input: JSON object expected");

    InputCfg c;

    if (!j.contains("source_name") || !j["source_name"].is_string()
            || j["source_name"].get<std::string>().empty())
        throw std::invalid_argument("ndi input: 'source_name' (non-empty string) required");
    c.source_name = j["source_name"].get<std::string>();

    if (j.contains("url")) {
        if (!j["url"].is_string())
            throw std::invalid_argument("ndi input: 'url' must be a string");
        c.url_address = j["url"].get<std::string>();
    }
    if (j.contains("groups")) {
        if (!j["groups"].is_string())
            throw std::invalid_argument("ndi input: 'groups' must be a string");
        c.groups = j["groups"].get<std::string>();
    }
    if (j.contains("lib_path")) {
        if (!j["lib_path"].is_string())
            throw std::invalid_argument("ndi input: 'lib_path' must be a string");
        c.lib_path_override = j["lib_path"].get<std::string>();
    }
    if (j.contains("recv_name")) {
        if (!j["recv_name"].is_string())
            throw std::invalid_argument("ndi input: 'recv_name' must be a string");
        const auto& s = j["recv_name"].get_ref<const std::string&>();
        if (!s.empty()) c.recv_name = s;
    }
    c.low_bandwidth = j.value("low_bandwidth", c.low_bandwidth);

    if (j.contains("reconnect_on_silence_sec")) {
        if (!j["reconnect_on_silence_sec"].is_number_integer())
            throw std::invalid_argument("ndi input: 'reconnect_on_silence_sec' must be integer");
        c.reconnect_on_silence_sec = j["reconnect_on_silence_sec"].get<int>();
        if (c.reconnect_on_silence_sec < 0 || c.reconnect_on_silence_sec > 3600)
            throw std::invalid_argument(
                "ndi input: 'reconnect_on_silence_sec' out of range (0..3600)");
    }

    return c;
}

}  // namespace liveqx::ndi
