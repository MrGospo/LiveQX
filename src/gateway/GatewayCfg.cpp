#include "gateway/GatewayCfg.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace liveqx::gateway {

const char* toString(GatewayMode m) noexcept {
    switch (m) {
        case GatewayMode::Passthrough: return "passthrough";
        case GatewayMode::Demux:       return "demux";
        case GatewayMode::Remux:       return "remux";
        case GatewayMode::Transcode:   return "transcode";
    }
    return "passthrough";
}

GatewayMode parseMode(const std::string& s) {
    if (s == "passthrough") return GatewayMode::Passthrough;
    if (s == "demux")       return GatewayMode::Demux;
    if (s == "remux")       return GatewayMode::Remux;
    if (s == "transcode")   return GatewayMode::Transcode;
    throw std::invalid_argument(
        "gateway cfg: unknown 'mode' '" + s + "' (expected passthrough|demux|remux|transcode)");
}

const char* toString(RemuxCfg::ServiceIdPolicy p) noexcept {
    switch (p) {
        case RemuxCfg::ServiceIdPolicy::AutoRenumber: return "auto_renumber";
        case RemuxCfg::ServiceIdPolicy::Reject:       return "reject";
    }
    return "auto_renumber";
}

RemuxCfg::ServiceIdPolicy parseServiceIdPolicy(const std::string& s) {
    if (s == "auto_renumber") return RemuxCfg::ServiceIdPolicy::AutoRenumber;
    if (s == "reject")        return RemuxCfg::ServiceIdPolicy::Reject;
    throw std::invalid_argument(
        "gateway remux: unknown 'service_id_policy' '" + s
        + "' (expected auto_renumber|reject)");
}

const char* toString(FecCfg::Mode m) noexcept {
    switch (m) {
        case FecCfg::Mode::OneD: return "1d";
        case FecCfg::Mode::TwoD: return "2d";
    }
    return "1d";
}

FecCfg::Mode parseFecMode(const std::string& s) {
    if (s == "1d") return FecCfg::Mode::OneD;
    if (s == "2d") return FecCfg::Mode::TwoD;
    throw std::invalid_argument(
        "gateway fec: unknown 'mode' '" + s + "' (expected 1d|2d)");
}

namespace {

void requireString(const nlohmann::json& j, const char* key, const char* ctx) {
    if (!j.contains(key) || !j[key].is_string()
            || j[key].get<std::string>().empty())
        throw std::invalid_argument(
            std::string(ctx) + ": '" + key + "' (non-empty string) required");
}

int requirePort(const nlohmann::json& j, const char* ctx) {
    if (!j.contains("port") || !j["port"].is_number_integer())
        throw std::invalid_argument(std::string(ctx) + ": 'port' (integer) required");
    const int p = j["port"].get<int>();
    if (p < 1 || p > 65535)
        throw std::invalid_argument(std::string(ctx) + ": 'port' out of range (1..65535)");
    return p;
}

// "interface" — IPv4 string OR NIC name. We disambiguate by looking at the
// first character: a digit means IPv4 (192.168.1.10), anything else is a
// NIC name (eth0, lo, wlp3s0). "interface_address" is always treated as IPv4.
void parseInterface(const nlohmann::json& j,
                    std::string& addr,
                    std::string& name,
                    const char* ctx) {
    if (j.contains("interface")) {
        if (!j["interface"].is_string())
            throw std::invalid_argument(std::string(ctx) + ": 'interface' must be a string");
        const auto v = j["interface"].get<std::string>();
        if (!v.empty()) {
            const bool looks_ipv4 =
                std::isdigit(static_cast<unsigned char>(v.front())) != 0;
            if (looks_ipv4) addr = v;
            else            name = v;
        }
    }
    if (j.contains("interface_address")) {
        if (!j["interface_address"].is_string())
            throw std::invalid_argument(
                std::string(ctx) + ": 'interface_address' must be a string");
        addr = j["interface_address"].get<std::string>();
    }
}

} // namespace

InputCfg parseInputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway input: JSON object expected");

    InputCfg in;
    requireString(j, "address", "gateway input");
    in.address = j["address"].get<std::string>();
    in.port    = requirePort(j, "gateway input");
    parseInterface(j, in.interface_addr, in.interface_name, "gateway input");

    if (j.contains("recv_buffer_kb")) {
        if (!j["recv_buffer_kb"].is_number_integer())
            throw std::invalid_argument("gateway input: 'recv_buffer_kb' must be integer");
        in.recv_buffer_kb = j["recv_buffer_kb"].get<int>();
        if (in.recv_buffer_kb < 16 || in.recv_buffer_kb > 65536)
            throw std::invalid_argument(
                "gateway input: 'recv_buffer_kb' out of range (16..65536)");
    }
    return in;
}

OutputCfg parseOutputCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway output: JSON object expected");

    OutputCfg out;
    requireString(j, "address", "gateway output");
    out.address = j["address"].get<std::string>();
    out.port    = requirePort(j, "gateway output");
    parseInterface(j, out.interface_addr, out.interface_name, "gateway output");

    if (j.contains("id")) {
        if (!j["id"].is_string())
            throw std::invalid_argument("gateway output: 'id' must be a string");
        out.id = j["id"].get<std::string>();
    }
    if (j.contains("ttl")) {
        if (!j["ttl"].is_number_integer())
            throw std::invalid_argument("gateway output: 'ttl' must be integer");
        out.ttl = j["ttl"].get<int>();
        if (out.ttl < 1 || out.ttl > 255)
            throw std::invalid_argument("gateway output: 'ttl' out of range (1..255)");
    }
    if (j.contains("send_buffer_kb")) {
        if (!j["send_buffer_kb"].is_number_integer())
            throw std::invalid_argument("gateway output: 'send_buffer_kb' must be integer");
        out.send_buffer_kb = j["send_buffer_kb"].get<int>();
        if (out.send_buffer_kb < 16 || out.send_buffer_kb > 16384)
            throw std::invalid_argument(
                "gateway output: 'send_buffer_kb' out of range (16..16384)");
    }
    return out;
}

namespace {

DemuxRule parseDemuxRule(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway demux route: JSON object expected");
    if (!j.contains("service_id") || !j["service_id"].is_number_integer())
        throw std::invalid_argument("gateway demux route: 'service_id' (integer) required");
    const int sid = j["service_id"].get<int>();
    if (sid < 1 || sid > 0xFFFF)
        throw std::invalid_argument(
            "gateway demux route: 'service_id' out of range (1..65535)");

    DemuxRule r;
    r.service_id = static_cast<std::uint16_t>(sid);
    requireString(j, "output_id", "gateway demux route");
    r.output_id = j["output_id"].get<std::string>();

    if (j.contains("preserve_eit"))       r.preserve_eit       = j["preserve_eit"].get<bool>();
    if (j.contains("preserve_subtitles")) r.preserve_subtitles = j["preserve_subtitles"].get<bool>();
    if (j.contains("preserve_teletext"))  r.preserve_teletext  = j["preserve_teletext"].get<bool>();

    if (j.contains("pid_remap")) {
        if (!j["pid_remap"].is_array())
            throw std::invalid_argument("gateway demux route: 'pid_remap' must be array");
        for (const auto& jp : j["pid_remap"]) {
            if (!jp.is_object() || !jp.contains("from") || !jp.contains("to"))
                throw std::invalid_argument(
                    "gateway demux route: pid_remap entry needs {from, to}");
            const int from = jp["from"].get<int>();
            const int to   = jp["to"].get<int>();
            if (from < 0 || from > 0x1FFF || to < 0 || to > 0x1FFF)
                throw std::invalid_argument(
                    "gateway demux route: pid_remap PID out of range (0..0x1FFF)");
            r.pid_remap.emplace_back(static_cast<std::uint16_t>(from),
                                     static_cast<std::uint16_t>(to));
        }
    }
    return r;
}

DemuxCfg parseDemuxCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway demux: JSON object expected");
    DemuxCfg d;
    if (!j.contains("routes") || !j["routes"].is_array())
        throw std::invalid_argument("gateway demux: 'routes' (array) required");
    if (j["routes"].empty())
        throw std::invalid_argument("gateway demux: 'routes' must not be empty");
    d.routes.reserve(j["routes"].size());
    for (const auto& jr : j["routes"])
        d.routes.push_back(parseDemuxRule(jr));

    if (j.contains("emit_sdt"))       d.emit_sdt       = j["emit_sdt"].get<bool>();
    auto loadPeriod = [&](const char* key, std::uint16_t& out, std::uint16_t lo, std::uint16_t hi) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer())
            throw std::invalid_argument(std::string("gateway demux: '") + key + "' must be integer");
        const int v = j[key].get<int>();
        if (v < lo || v > hi)
            throw std::invalid_argument(
                std::string("gateway demux: '") + key + "' out of range");
        out = static_cast<std::uint16_t>(v);
    };
    loadPeriod("pat_period_ms", d.pat_period_ms, 20, 500);
    loadPeriod("pmt_period_ms", d.pmt_period_ms, 20, 500);
    loadPeriod("sdt_period_ms", d.sdt_period_ms, 100, 30000);
    return d;
}

PidRemapEntry parsePidRemapEntry(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway remux pid_remap: JSON object expected");
    auto requireU16 = [&](const char* key) -> int {
        if (!j.contains(key) || !j[key].is_number_integer())
            throw std::invalid_argument(
                std::string("gateway remux pid_remap: '") + key
                + "' (integer) required");
        return j[key].get<int>();
    };
    const int input_idx = requireU16("input_idx");
    const int src_pid   = requireU16("src_pid");
    const int dst_pid   = requireU16("dst_pid");
    if (input_idx < 0 || input_idx > 0xFFFF)
        throw std::invalid_argument(
            "gateway remux pid_remap: 'input_idx' out of range (0..65535)");
    if (src_pid < 0 || src_pid > 0x1FFF)
        throw std::invalid_argument(
            "gateway remux pid_remap: 'src_pid' out of range (0..0x1FFF)");
    if (dst_pid < 0 || dst_pid > 0x1FFF)
        throw std::invalid_argument(
            "gateway remux pid_remap: 'dst_pid' out of range (0..0x1FFF)");
    PidRemapEntry e;
    e.input_idx = static_cast<std::uint16_t>(input_idx);
    e.src_pid   = static_cast<std::uint16_t>(src_pid);
    e.dst_pid   = static_cast<std::uint16_t>(dst_pid);
    return e;
}

RemuxCfg parseRemuxCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway remux: JSON object expected");
    RemuxCfg r;
    if (j.contains("pid_remap")) {
        if (!j["pid_remap"].is_array())
            throw std::invalid_argument("gateway remux: 'pid_remap' must be array");
        for (const auto& jp : j["pid_remap"])
            r.pid_remap.push_back(parsePidRemapEntry(jp));
    }
    if (j.contains("target_bitrate_bps")) {
        if (!j["target_bitrate_bps"].is_number_integer())
            throw std::invalid_argument(
                "gateway remux: 'target_bitrate_bps' must be integer");
        const auto v = j["target_bitrate_bps"].get<long long>();
        if (v < 0 || v > 1'000'000'000LL)
            throw std::invalid_argument(
                "gateway remux: 'target_bitrate_bps' out of range (0..1e9)");
        r.target_bitrate_bps = static_cast<std::uint32_t>(v);
    }
    auto loadU16 = [&](const char* key, std::uint16_t& out, int lo, int hi) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer())
            throw std::invalid_argument(
                std::string("gateway remux: '") + key + "' must be integer");
        const int v = j[key].get<int>();
        if (v < lo || v > hi)
            throw std::invalid_argument(
                std::string("gateway remux: '") + key + "' out of range");
        out = static_cast<std::uint16_t>(v);
    };
    loadU16("transport_stream_id", r.transport_stream_id, 0,    0xFFFF);
    loadU16("original_network_id", r.original_network_id, 0,    0xFFFF);
    loadU16("pat_period_ms",       r.pat_period_ms,       20,   500);
    loadU16("pmt_period_ms",       r.pmt_period_ms,       20,   500);
    loadU16("sdt_period_ms",       r.sdt_period_ms,       100,  30000);
    if (j.contains("emit_sdt"))
        r.emit_sdt = j["emit_sdt"].get<bool>();
    if (j.contains("service_id_policy")) {
        if (!j["service_id_policy"].is_string())
            throw std::invalid_argument(
                "gateway remux: 'service_id_policy' must be string");
        r.service_id_policy =
            parseServiceIdPolicy(j["service_id_policy"].get<std::string>());
    }
    return r;
}

TranscodeCfg parseTranscodeCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway transcode: JSON object expected");
    TranscodeCfg t;

    auto loadInt = [&](const char* key, auto& out, long long lo, long long hi) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer())
            throw std::invalid_argument(
                std::string("gateway transcode: '") + key + "' must be integer");
        const long long v = j[key].get<long long>();
        if (v < lo || v > hi)
            throw std::invalid_argument(
                std::string("gateway transcode: '") + key + "' out of range");
        out = static_cast<std::remove_reference_t<decltype(out)>>(v);
    };

    // video
    loadInt("video_width",        t.video_width,        16,    7680);
    loadInt("video_height",       t.video_height,       16,    4320);
    loadInt("video_fps",          t.video_fps,          1,     120);
    loadInt("video_bitrate_bps",  t.video_bitrate_bps,  16'000, 200'000'000LL);
    loadInt("video_max_b_frames", t.video_max_b_frames, 0,     8);
    loadInt("video_gpu_index",    t.video_gpu_index,    0,     31);
    if (j.contains("video_preset")) {
        if (!j["video_preset"].is_string())
            throw std::invalid_argument(
                "gateway transcode: 'video_preset' must be string");
        t.video_preset = j["video_preset"].get<std::string>();
        if (t.video_preset.empty())
            throw std::invalid_argument(
                "gateway transcode: 'video_preset' must not be empty");
    }
    if (j.contains("video_encoder_mode")) {
        if (!j["video_encoder_mode"].is_string())
            throw std::invalid_argument(
                "gateway transcode: 'video_encoder_mode' must be string");
        const auto m = j["video_encoder_mode"].get<std::string>();
        // Same set fix29 EncoderFactory::pickVideoEncoder accepts. Empty is
        // treated as "auto" downstream — reject here to keep cfg explicit.
        static const std::unordered_set<std::string> kModes{
            "auto", "cpu", "x264", "nvenc", "qsv", "vaapi"};
        if (kModes.count(m) == 0)
            throw std::invalid_argument(
                "gateway transcode: 'video_encoder_mode' must be one of "
                "auto|cpu|x264|nvenc|qsv|vaapi");
        t.video_encoder_mode = m;
    }

    // audio
    loadInt("audio_bitrate_bps", t.audio_bitrate_bps, 8'000, 1'000'000);
    loadInt("audio_sample_rate", t.audio_sample_rate, 8000,  192000);
    loadInt("audio_channels",    t.audio_channels,    1,     8);

    // loss protection
    if (j.contains("freeze_on_video_loss"))
        t.freeze_on_video_loss = j["freeze_on_video_loss"].get<bool>();
    if (j.contains("silence_on_audio_loss"))
        t.silence_on_audio_loss = j["silence_on_audio_loss"].get<bool>();
    loadInt("loss_grace_ms",        t.loss_grace_ms,        0,     60000);
    loadInt("retry_backoff_ms_min", t.retry_backoff_ms_min, 50,    60000);
    loadInt("retry_backoff_ms_max", t.retry_backoff_ms_max, 50,    600000);
    if (t.retry_backoff_ms_max < t.retry_backoff_ms_min)
        throw std::invalid_argument(
            "gateway transcode: 'retry_backoff_ms_max' must be >= "
            "'retry_backoff_ms_min'");
    if (j.contains("fallback_logo_path")) {
        if (!j["fallback_logo_path"].is_string())
            throw std::invalid_argument(
                "gateway transcode: 'fallback_logo_path' must be string");
        t.fallback_logo_path = j["fallback_logo_path"].get<std::string>();
    }

    // PSI / output transport
    loadInt("transport_stream_id", t.transport_stream_id, 0, 0xFFFF);
    loadInt("original_network_id", t.original_network_id, 0, 0xFFFF);
    loadInt("service_id",          t.service_id,          1, 0xFFFF);
    loadInt("pmt_pid",             t.pmt_pid,             0x10, 0x1FFE);
    loadInt("video_pid",           t.video_pid,           0x10, 0x1FFE);
    loadInt("audio_pid",           t.audio_pid,           0x10, 0x1FFE);
    loadInt("pcr_pid",             t.pcr_pid,             0,    0x1FFE);
    loadInt("pat_period_ms",       t.pat_period_ms,       20,   500);
    loadInt("pmt_period_ms",       t.pmt_period_ms,       20,   500);
    loadInt("sdt_period_ms",       t.sdt_period_ms,       100,  30000);
    if (j.contains("emit_sdt"))
        t.emit_sdt = j["emit_sdt"].get<bool>();
    if (j.contains("service_name")) {
        if (!j["service_name"].is_string())
            throw std::invalid_argument(
                "gateway transcode: 'service_name' must be string");
        t.service_name = j["service_name"].get<std::string>();
    }
    if (j.contains("provider_name")) {
        if (!j["provider_name"].is_string())
            throw std::invalid_argument(
                "gateway transcode: 'provider_name' must be string");
        t.provider_name = j["provider_name"].get<std::string>();
    }
    if (j.contains("target_bitrate_bps")) {
        if (!j["target_bitrate_bps"].is_number_integer())
            throw std::invalid_argument(
                "gateway transcode: 'target_bitrate_bps' must be integer");
        const auto v = j["target_bitrate_bps"].get<long long>();
        if (v < 0 || v > 1'000'000'000LL)
            throw std::invalid_argument(
                "gateway transcode: 'target_bitrate_bps' out of range (0..1e9)");
        t.target_bitrate_bps = static_cast<std::uint32_t>(v);
    }

    // PID uniqueness — pmt/video/audio share the same PID space and must not
    // collide. pcr_pid==0 (use video_pid) is valid; otherwise must be unique
    // OR equal to video_pid / audio_pid (broadcast convention permits PCR
    // ride-along on an ES PID).
    {
        std::unordered_set<std::uint16_t> pids;
        pids.insert(t.pmt_pid);
        if (!pids.insert(t.video_pid).second)
            throw std::invalid_argument(
                "gateway transcode: 'video_pid' collides with 'pmt_pid'");
        if (!pids.insert(t.audio_pid).second)
            throw std::invalid_argument(
                "gateway transcode: 'audio_pid' collides with another PID");
        if (t.pcr_pid != 0
                && t.pcr_pid != t.video_pid
                && t.pcr_pid != t.audio_pid
                && pids.count(t.pcr_pid) > 0)
            throw std::invalid_argument(
                "gateway transcode: 'pcr_pid' collides with 'pmt_pid'");
    }

    return t;
}

} // namespace

FecCfg parseFecCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway fec: JSON object expected");
    FecCfg f;
    if (j.contains("enabled")) {
        if (!j["enabled"].is_boolean())
            throw std::invalid_argument("gateway fec: 'enabled' must be boolean");
        f.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("mode")) {
        if (!j["mode"].is_string())
            throw std::invalid_argument("gateway fec: 'mode' must be string");
        f.mode = parseFecMode(j["mode"].get<std::string>());
    }
    auto loadU8 = [&](const char* key, std::uint8_t& out, int lo, int hi) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer())
            throw std::invalid_argument(
                std::string("gateway fec: '") + key + "' must be integer");
        const int v = j[key].get<int>();
        if (v < lo || v > hi)
            throw std::invalid_argument(
                std::string("gateway fec: '") + key + "' out of range");
        out = static_cast<std::uint8_t>(v);
    };
    auto loadU16 = [&](const char* key, std::uint16_t& out, int lo, int hi) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer())
            throw std::invalid_argument(
                std::string("gateway fec: '") + key + "' must be integer");
        const int v = j[key].get<int>();
        if (v < lo || v > hi)
            throw std::invalid_argument(
                std::string("gateway fec: '") + key + "' out of range");
        out = static_cast<std::uint16_t>(v);
    };
    loadU8 ("L",                  f.L,                  1,    20);
    loadU8 ("D",                  f.D,                  4,    20);
    loadU8 ("payload_type",       f.payload_type,       0,    127);
    loadU8 ("ts_per_rtp",         f.ts_per_rtp,         1,    7);
    loadU16("column_port_offset", f.column_port_offset, 1,    255);
    loadU16("row_port_offset",    f.row_port_offset,    1,    255);
    if (j.contains("ssrc")) {
        if (!j["ssrc"].is_number_integer())
            throw std::invalid_argument("gateway fec: 'ssrc' must be integer");
        const auto v = j["ssrc"].get<long long>();
        if (v < 0 || v > 0xFFFFFFFFLL)
            throw std::invalid_argument(
                "gateway fec: 'ssrc' out of range (0..0xFFFFFFFF)");
        f.ssrc = static_cast<std::uint32_t>(v);
    }
    // Cross-validation only meaningful when enabled — disabled FEC is the
    // pass-through path and we don't want to reject persisted configs that
    // happen to carry stale defaults.
    if (f.enabled) {
        const int product = static_cast<int>(f.L) * static_cast<int>(f.D);
        if (product > 100)
            throw std::invalid_argument(
                "gateway fec: L*D must be <= 100 (Pro-MPEG COP3 r2)");
        if (f.column_port_offset == f.row_port_offset)
            throw std::invalid_argument(
                "gateway fec: 'column_port_offset' must differ from 'row_port_offset'");
    }
    return f;
}

GatewayCfg parseGatewayCfg(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::invalid_argument("gateway cfg: JSON object expected");

    const bool has_input  = j.contains("input");
    const bool has_inputs = j.contains("inputs");
    if (!has_input && !has_inputs)
        throw std::invalid_argument(
            "gateway cfg: 'input' (object) or 'inputs' (array) required");
    if (has_input && has_inputs)
        throw std::invalid_argument(
            "gateway cfg: 'input' and 'inputs' are mutually exclusive");

    if (!j.contains("outputs") || !j["outputs"].is_array())
        throw std::invalid_argument("gateway cfg: 'outputs' (array) required");
    if (j["outputs"].empty())
        throw std::invalid_argument("gateway cfg: 'outputs' must not be empty");

    GatewayCfg cfg;
    if (j.contains("mode")) {
        if (!j["mode"].is_string())
            throw std::invalid_argument("gateway cfg: 'mode' must be string");
        cfg.mode = parseMode(j["mode"].get<std::string>());
    }

    if (has_input) {
        cfg.input = parseInputCfg(j["input"]);
    } else {
        if (!j["inputs"].is_array() || j["inputs"].empty())
            throw std::invalid_argument(
                "gateway cfg: 'inputs' must be a non-empty array");
        cfg.input = parseInputCfg(j["inputs"][0]);
        cfg.extra_inputs.reserve(j["inputs"].size() - 1);
        for (std::size_t i = 1; i < j["inputs"].size(); ++i)
            cfg.extra_inputs.push_back(parseInputCfg(j["inputs"][i]));
    }

    cfg.outputs.reserve(j["outputs"].size());
    for (const auto& jo : j["outputs"])
        cfg.outputs.push_back(parseOutputCfg(jo));

    if (cfg.mode == GatewayMode::Demux) {
        if (!cfg.extra_inputs.empty())
            throw std::invalid_argument(
                "gateway cfg: demux mode accepts a single input only");
        if (!j.contains("demux"))
            throw std::invalid_argument("gateway cfg: 'demux' required when mode=='demux'");
        cfg.demux = parseDemuxCfg(j["demux"]);

        // Cross-validate: every route.output_id must reference an existing
        // OutputCfg.id; service_ids must be unique.
        std::unordered_set<std::string> known_outputs;
        for (const auto& o : cfg.outputs) if (!o.id.empty()) known_outputs.insert(o.id);
        std::unordered_set<std::uint16_t> seen_sids;
        for (const auto& r : cfg.demux.routes) {
            if (known_outputs.count(r.output_id) == 0)
                throw std::invalid_argument(
                    "gateway demux route: output_id '" + r.output_id + "' not in outputs");
            if (!seen_sids.insert(r.service_id).second)
                throw std::invalid_argument(
                    "gateway demux: duplicate service_id in routes");
        }
    } else if (j.contains("demux")) {
        // Allow demux block on a passthrough cfg only as a no-op (operator
        // may toggle mode later) — but reject obviously-bad demux structure
        // here so REST returns 400 immediately.
        cfg.demux = parseDemuxCfg(j["demux"]);
    }

    if (cfg.mode == GatewayMode::Remux) {
        if (cfg.extra_inputs.empty())
            throw std::invalid_argument(
                "gateway cfg: remux mode requires 'inputs' array with ≥2 entries");
        if (cfg.outputs.size() != 1)
            throw std::invalid_argument(
                "gateway cfg: remux mode emits exactly one MPTS output");
        if (!j.contains("remux"))
            throw std::invalid_argument("gateway cfg: 'remux' required when mode=='remux'");
        cfg.remux = parseRemuxCfg(j["remux"]);

        // Cross-validate: every pid_remap.input_idx must be in range
        // [0, 1+extra_inputs.size()); every (input_idx, src_pid) pair must
        // be unique; every dst_pid must be unique across the entire MPTS
        // output (downstream PID space).
        const std::size_t n_inputs = 1 + cfg.extra_inputs.size();
        std::unordered_set<std::uint32_t> seen_src;   // (input_idx<<16)|pid
        std::unordered_set<std::uint16_t> seen_dst;
        for (const auto& e : cfg.remux.pid_remap) {
            if (e.input_idx >= n_inputs)
                throw std::invalid_argument(
                    "gateway remux pid_remap: input_idx out of range");
            const std::uint32_t key = (std::uint32_t(e.input_idx) << 16) | e.src_pid;
            if (!seen_src.insert(key).second)
                throw std::invalid_argument(
                    "gateway remux pid_remap: duplicate (input_idx, src_pid)");
            if (!seen_dst.insert(e.dst_pid).second)
                throw std::invalid_argument(
                    "gateway remux pid_remap: duplicate dst_pid in output PID space");
        }
    } else if (j.contains("remux")) {
        cfg.remux = parseRemuxCfg(j["remux"]);
    }

    if (cfg.mode == GatewayMode::Transcode) {
        if (!cfg.extra_inputs.empty())
            throw std::invalid_argument(
                "gateway cfg: transcode mode accepts a single input only");
        if (cfg.outputs.size() != 1)
            throw std::invalid_argument(
                "gateway cfg: transcode mode emits exactly one SPTS output");
        if (!j.contains("transcode"))
            throw std::invalid_argument(
                "gateway cfg: 'transcode' required when mode=='transcode'");
        cfg.transcode = parseTranscodeCfg(j["transcode"]);
    } else if (j.contains("transcode")) {
        cfg.transcode = parseTranscodeCfg(j["transcode"]);
    }

    if (j.contains("fec")) {
        cfg.fec = parseFecCfg(j["fec"]);
        // When FEC is on, every output's (port + max_offset) must fit in
        // the legal UDP port range. Validate now so REST returns 400 instead
        // of failing at socket bind.
        if (cfg.fec.enabled) {
            const int max_off = std::max<int>(
                cfg.fec.column_port_offset,
                cfg.fec.mode == FecCfg::Mode::TwoD ? cfg.fec.row_port_offset : 0);
            for (const auto& o : cfg.outputs) {
                if (o.port + max_off > 65535)
                    throw std::invalid_argument(
                        "gateway fec: output port + FEC offset exceeds 65535");
            }
        }
    }

    return cfg;
}

nlohmann::json toJson(const InputCfg& in) {
    nlohmann::json j{
        {"address", in.address},
        {"port",    in.port},
    };
    if (!in.interface_addr.empty()) j["interface_address"] = in.interface_addr;
    if (!in.interface_name.empty()) j["interface"]         = in.interface_name;
    if (in.recv_buffer_kb != 1024)  j["recv_buffer_kb"]    = in.recv_buffer_kb;
    return j;
}

nlohmann::json toJson(const OutputCfg& out) {
    nlohmann::json j{
        {"address", out.address},
        {"port",    out.port},
    };
    if (!out.id.empty())             j["id"]                = out.id;
    if (!out.interface_addr.empty()) j["interface_address"] = out.interface_addr;
    if (!out.interface_name.empty()) j["interface"]         = out.interface_name;
    if (out.ttl != 16)               j["ttl"]               = out.ttl;
    if (out.send_buffer_kb != 256)   j["send_buffer_kb"]    = out.send_buffer_kb;
    return j;
}

nlohmann::json toJson(const DemuxRule& r) {
    nlohmann::json j{
        {"service_id", r.service_id},
        {"output_id",  r.output_id},
    };
    if (!r.preserve_eit)       j["preserve_eit"]       = false;
    if (!r.preserve_subtitles) j["preserve_subtitles"] = false;
    if (!r.preserve_teletext)  j["preserve_teletext"]  = false;
    if (!r.pid_remap.empty()) {
        auto arr = nlohmann::json::array();
        for (auto [from, to] : r.pid_remap) arr.push_back({{"from", from}, {"to", to}});
        j["pid_remap"] = arr;
    }
    return j;
}

nlohmann::json toJson(const DemuxCfg& d) {
    nlohmann::json j{{"routes", nlohmann::json::array()}};
    for (const auto& r : d.routes) j["routes"].push_back(toJson(r));
    if (!d.emit_sdt)         j["emit_sdt"]      = false;
    if (d.pat_period_ms != 100)  j["pat_period_ms"] = d.pat_period_ms;
    if (d.pmt_period_ms != 100)  j["pmt_period_ms"] = d.pmt_period_ms;
    if (d.sdt_period_ms != 2000) j["sdt_period_ms"] = d.sdt_period_ms;
    return j;
}

nlohmann::json toJson(const PidRemapEntry& e) {
    return nlohmann::json{
        {"input_idx", e.input_idx},
        {"src_pid",   e.src_pid},
        {"dst_pid",   e.dst_pid},
    };
}

nlohmann::json toJson(const RemuxCfg& r) {
    nlohmann::json j = nlohmann::json::object();
    if (!r.pid_remap.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& e : r.pid_remap) arr.push_back(toJson(e));
        j["pid_remap"] = arr;
    }
    if (r.target_bitrate_bps != 0)   j["target_bitrate_bps"]  = r.target_bitrate_bps;
    if (r.transport_stream_id != 1)  j["transport_stream_id"] = r.transport_stream_id;
    if (r.original_network_id != 1)  j["original_network_id"] = r.original_network_id;
    if (r.pat_period_ms != 100)      j["pat_period_ms"]       = r.pat_period_ms;
    if (r.pmt_period_ms != 100)      j["pmt_period_ms"]       = r.pmt_period_ms;
    if (r.sdt_period_ms != 2000)     j["sdt_period_ms"]       = r.sdt_period_ms;
    if (!r.emit_sdt)                 j["emit_sdt"]            = false;
    if (r.service_id_policy != RemuxCfg::ServiceIdPolicy::AutoRenumber)
        j["service_id_policy"] = toString(r.service_id_policy);
    return j;
}

nlohmann::json toJson(const TranscodeCfg& t) {
    nlohmann::json j = nlohmann::json::object();
    // video — emit only non-defaults
    if (t.video_width        != 1280)        j["video_width"]        = t.video_width;
    if (t.video_height       != 720)         j["video_height"]       = t.video_height;
    if (t.video_fps          != 25)          j["video_fps"]          = t.video_fps;
    if (t.video_bitrate_bps  != 4'000'000u)  j["video_bitrate_bps"]  = t.video_bitrate_bps;
    if (t.video_max_b_frames != 0)           j["video_max_b_frames"] = t.video_max_b_frames;
    if (t.video_preset       != "medium")    j["video_preset"]       = t.video_preset;
    if (t.video_encoder_mode != "auto")      j["video_encoder_mode"] = t.video_encoder_mode;
    if (t.video_gpu_index    != 0)           j["video_gpu_index"]    = t.video_gpu_index;
    // audio
    if (t.audio_bitrate_bps  != 128'000u)    j["audio_bitrate_bps"]  = t.audio_bitrate_bps;
    if (t.audio_sample_rate  != 48000)       j["audio_sample_rate"]  = t.audio_sample_rate;
    if (t.audio_channels     != 2)           j["audio_channels"]     = t.audio_channels;
    // loss protection
    if (!t.freeze_on_video_loss)             j["freeze_on_video_loss"]  = false;
    if (!t.silence_on_audio_loss)            j["silence_on_audio_loss"] = false;
    if (t.loss_grace_ms        != 2000u)     j["loss_grace_ms"]        = t.loss_grace_ms;
    if (t.retry_backoff_ms_min != 200u)      j["retry_backoff_ms_min"] = t.retry_backoff_ms_min;
    if (t.retry_backoff_ms_max != 5000u)     j["retry_backoff_ms_max"] = t.retry_backoff_ms_max;
    if (!t.fallback_logo_path.empty())       j["fallback_logo_path"]   = t.fallback_logo_path;
    // PSI / output
    if (t.transport_stream_id != 1)          j["transport_stream_id"] = t.transport_stream_id;
    if (t.original_network_id != 1)          j["original_network_id"] = t.original_network_id;
    if (t.service_id          != 1)          j["service_id"]          = t.service_id;
    if (t.service_name        != "Transcoded")        j["service_name"]  = t.service_name;
    if (t.provider_name       != "streaming-core")    j["provider_name"] = t.provider_name;
    if (t.pmt_pid             != 0x100)      j["pmt_pid"]             = t.pmt_pid;
    if (t.video_pid           != 0x101)      j["video_pid"]           = t.video_pid;
    if (t.audio_pid           != 0x102)      j["audio_pid"]           = t.audio_pid;
    if (t.pcr_pid             != 0)          j["pcr_pid"]             = t.pcr_pid;
    if (t.pat_period_ms       != 100)        j["pat_period_ms"]       = t.pat_period_ms;
    if (t.pmt_period_ms       != 100)        j["pmt_period_ms"]       = t.pmt_period_ms;
    if (t.sdt_period_ms       != 2000)       j["sdt_period_ms"]       = t.sdt_period_ms;
    if (!t.emit_sdt)                         j["emit_sdt"]            = false;
    if (t.target_bitrate_bps  != 0)          j["target_bitrate_bps"]  = t.target_bitrate_bps;
    return j;
}

nlohmann::json toJson(const FecCfg& f) {
    nlohmann::json j = nlohmann::json::object();
    if (f.enabled)                      j["enabled"]            = true;
    if (f.mode != FecCfg::Mode::OneD)   j["mode"]               = toString(f.mode);
    if (f.L != 8)                       j["L"]                  = f.L;
    if (f.D != 8)                       j["D"]                  = f.D;
    if (f.ssrc != 0)                    j["ssrc"]               = f.ssrc;
    if (f.payload_type != 33)           j["payload_type"]       = f.payload_type;
    if (f.ts_per_rtp != 7)              j["ts_per_rtp"]         = f.ts_per_rtp;
    if (f.column_port_offset != 2)      j["column_port_offset"] = f.column_port_offset;
    if (f.row_port_offset != 4)         j["row_port_offset"]    = f.row_port_offset;
    return j;
}

nlohmann::json toJson(const GatewayCfg& cfg) {
    nlohmann::json j{{"outputs", nlohmann::json::array()}};
    if (cfg.extra_inputs.empty()) {
        j["input"] = toJson(cfg.input);
    } else {
        auto arr = nlohmann::json::array();
        arr.push_back(toJson(cfg.input));
        for (const auto& in : cfg.extra_inputs) arr.push_back(toJson(in));
        j["inputs"] = std::move(arr);
    }
    if (cfg.mode != GatewayMode::Passthrough) j["mode"] = toString(cfg.mode);
    for (const auto& o : cfg.outputs) j["outputs"].push_back(toJson(o));
    if (cfg.mode == GatewayMode::Demux)     j["demux"]     = toJson(cfg.demux);
    if (cfg.mode == GatewayMode::Remux)     j["remux"]     = toJson(cfg.remux);
    if (cfg.mode == GatewayMode::Transcode) j["transcode"] = toJson(cfg.transcode);
    // FEC is orthogonal — emit only when enabled or carries non-defaults.
    auto fec_j = toJson(cfg.fec);
    if (!fec_j.empty()) j["fec"] = std::move(fec_j);
    return j;
}

} // namespace liveqx::gateway
