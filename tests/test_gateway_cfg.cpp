#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "gateway/GatewayCfg.h"

using nlohmann::json;
using namespace liveqx::gateway;

namespace {

// ─── Input ───────────────────────────────────────────────────────────────────

TEST(GatewayInputCfg, MinimalValid) {
    auto in = parseInputCfg(json{{"address", "239.1.2.3"}, {"port", 5000}});
    EXPECT_EQ(in.address, "239.1.2.3");
    EXPECT_EQ(in.port,    5000);
    EXPECT_EQ(in.recv_buffer_kb, 1024);
    EXPECT_TRUE(in.interface_addr.empty());
    EXPECT_TRUE(in.interface_name.empty());
}

TEST(GatewayInputCfg, InterfaceAsName) {
    auto in = parseInputCfg(json{
        {"address", "239.1.2.3"}, {"port", 5000}, {"interface", "eth0"}});
    EXPECT_EQ(in.interface_name, "eth0");
    EXPECT_TRUE(in.interface_addr.empty());
}

TEST(GatewayInputCfg, InterfaceAsIPv4) {
    auto in = parseInputCfg(json{
        {"address", "239.1.2.3"}, {"port", 5000}, {"interface", "192.168.1.10"}});
    EXPECT_EQ(in.interface_addr, "192.168.1.10");
    EXPECT_TRUE(in.interface_name.empty());
}

TEST(GatewayInputCfg, ExplicitAddrFieldOverridesInterface) {
    auto in = parseInputCfg(json{
        {"address",          "239.1.2.3"},
        {"port",             5000},
        {"interface",        "eth0"},
        {"interface_address","10.0.0.1"},
    });
    EXPECT_EQ(in.interface_name, "eth0");
    EXPECT_EQ(in.interface_addr, "10.0.0.1");
}

TEST(GatewayInputCfg, RecvBufferKb) {
    auto in = parseInputCfg(json{
        {"address","239.1.2.3"},{"port",5000},{"recv_buffer_kb",4096}});
    EXPECT_EQ(in.recv_buffer_kb, 4096);
}

TEST(GatewayInputCfg, RejectsMissingAddress) {
    EXPECT_THROW(parseInputCfg(json{{"port", 5000}}), std::invalid_argument);
}
TEST(GatewayInputCfg, RejectsEmptyAddress) {
    EXPECT_THROW(parseInputCfg(json{{"address",""},{"port",5000}}),
                 std::invalid_argument);
}
TEST(GatewayInputCfg, RejectsBadPort) {
    EXPECT_THROW(parseInputCfg(json{{"address","x"},{"port",0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"address","x"},{"port",70000}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"address","x"},{"port","abc"}}),
                 std::invalid_argument);
}
TEST(GatewayInputCfg, RejectsBadRecvBuffer) {
    EXPECT_THROW(parseInputCfg(json{{"address","x"},{"port",5000},
                                    {"recv_buffer_kb",4}}),
                 std::invalid_argument);
    EXPECT_THROW(parseInputCfg(json{{"address","x"},{"port",5000},
                                    {"recv_buffer_kb",1<<20}}),
                 std::invalid_argument);
}
TEST(GatewayInputCfg, RejectsNonObject) {
    EXPECT_THROW(parseInputCfg(json::array()), std::invalid_argument);
}

// ─── Output ──────────────────────────────────────────────────────────────────

TEST(GatewayOutputCfg, MinimalValid) {
    auto out = parseOutputCfg(json{{"address","239.5.5.5"},{"port",6000}});
    EXPECT_EQ(out.address, "239.5.5.5");
    EXPECT_EQ(out.port,    6000);
    EXPECT_EQ(out.ttl,     16);
    EXPECT_EQ(out.send_buffer_kb, 256);
    EXPECT_TRUE(out.id.empty());
}

TEST(GatewayOutputCfg, FullValid) {
    auto out = parseOutputCfg(json{
        {"id",             "out0"},
        {"address",        "239.5.5.5"},
        {"port",           6000},
        {"interface",      "192.168.1.10"},
        {"ttl",            32},
        {"send_buffer_kb", 1024},
    });
    EXPECT_EQ(out.id,              "out0");
    EXPECT_EQ(out.interface_addr,  "192.168.1.10");
    EXPECT_EQ(out.ttl,             32);
    EXPECT_EQ(out.send_buffer_kb,  1024);
}

TEST(GatewayOutputCfg, RejectsBadTtl) {
    EXPECT_THROW(parseOutputCfg(json{{"address","x"},{"port",6000},{"ttl",0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseOutputCfg(json{{"address","x"},{"port",6000},{"ttl",256}}),
                 std::invalid_argument);
}
TEST(GatewayOutputCfg, RejectsBadIdType) {
    EXPECT_THROW(parseOutputCfg(json{{"id",42},{"address","x"},{"port",6000}}),
                 std::invalid_argument);
}

// ─── Top-level ──────────────────────────────────────────────────────────────

TEST(GatewayCfg, FullValid) {
    auto cfg = parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000},{"interface","eth0"}}},
        {"outputs", json::array({
            json{{"id","out0"},{"address","239.5.5.5"},{"port",6000}},
            json{{"id","out1"},{"address","127.0.0.1"},{"port",6001}},
        })},
    });
    EXPECT_EQ(cfg.input.address, "239.1.2.3");
    EXPECT_EQ(cfg.input.interface_name, "eth0");
    ASSERT_EQ(cfg.outputs.size(), 2u);
    EXPECT_EQ(cfg.outputs[0].id, "out0");
    EXPECT_EQ(cfg.outputs[1].address, "127.0.0.1");
}

TEST(GatewayCfg, RejectsMissingInput) {
    EXPECT_THROW(parseGatewayCfg(json{{"outputs", json::array()}}),
                 std::invalid_argument);
}
TEST(GatewayCfg, RejectsMissingOutputs) {
    EXPECT_THROW(parseGatewayCfg(json{
                    {"input", {{"address","x"},{"port",5000}}}}),
                 std::invalid_argument);
}
TEST(GatewayCfg, RejectsEmptyOutputs) {
    EXPECT_THROW(parseGatewayCfg(json{
                    {"input",   {{"address","x"},{"port",5000}}},
                    {"outputs", json::array()}}),
                 std::invalid_argument);
}
TEST(GatewayCfg, RejectsOutputsNotArray) {
    EXPECT_THROW(parseGatewayCfg(json{
                    {"input",   {{"address","x"},{"port",5000}}},
                    {"outputs", "not an array"}}),
                 std::invalid_argument);
}

// ─── Round-trip ──────────────────────────────────────────────────────────────

TEST(GatewayCfg, ToJsonElidesDefaults) {
    InputCfg in;
    in.address = "239.1.2.3";
    in.port    = 5000;
    auto j = toJson(in);
    EXPECT_FALSE(j.contains("recv_buffer_kb"));   // default elided
    EXPECT_FALSE(j.contains("interface"));
    EXPECT_FALSE(j.contains("interface_address"));
}

TEST(GatewayCfg, ToJsonKeepsNonDefaults) {
    OutputCfg out;
    out.id              = "out0";
    out.address         = "239.5.5.5";
    out.port            = 6000;
    out.interface_addr  = "10.0.0.1";
    out.ttl             = 32;
    out.send_buffer_kb  = 1024;
    auto j = toJson(out);
    EXPECT_EQ(j["id"],                "out0");
    EXPECT_EQ(j["interface_address"], "10.0.0.1");
    EXPECT_EQ(j["ttl"],               32);
    EXPECT_EQ(j["send_buffer_kb"],    1024);
}

TEST(GatewayCfg, RoundTrip) {
    const auto orig = parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000},
                     {"interface","eth0"},{"recv_buffer_kb",2048}}},
        {"outputs", json::array({
            json{{"id","out0"},{"address","239.5.5.5"},{"port",6000},{"ttl",64}},
            json{{"id","out1"},{"address","127.0.0.1"},{"port",6001},
                 {"interface_address","10.0.0.1"},{"send_buffer_kb",2048}},
        })},
    });
    const auto j2     = toJson(orig);
    const auto reparsed = parseGatewayCfg(j2);

    EXPECT_EQ(reparsed.input.address,        orig.input.address);
    EXPECT_EQ(reparsed.input.port,           orig.input.port);
    EXPECT_EQ(reparsed.input.interface_name, orig.input.interface_name);
    EXPECT_EQ(reparsed.input.recv_buffer_kb, orig.input.recv_buffer_kb);
    ASSERT_EQ(reparsed.outputs.size(),       orig.outputs.size());
    for (size_t i = 0; i < orig.outputs.size(); ++i) {
        EXPECT_EQ(reparsed.outputs[i].id,             orig.outputs[i].id);
        EXPECT_EQ(reparsed.outputs[i].address,        orig.outputs[i].address);
        EXPECT_EQ(reparsed.outputs[i].port,           orig.outputs[i].port);
        EXPECT_EQ(reparsed.outputs[i].interface_addr, orig.outputs[i].interface_addr);
        EXPECT_EQ(reparsed.outputs[i].ttl,            orig.outputs[i].ttl);
        EXPECT_EQ(reparsed.outputs[i].send_buffer_kb, orig.outputs[i].send_buffer_kb);
    }
}

// ─── fix-A3: multi-input + RemuxCfg ──────────────────────────────────────────

namespace {

json baseRemuxJson() {
    return json{
        {"mode", "remux"},
        {"inputs", json::array({
            json{{"address", "239.1.0.1"}, {"port", 5000}},
            json{{"address", "239.1.0.2"}, {"port", 5000}},
        })},
        {"outputs", json::array({
            json{{"id", "mpts"}, {"address", "239.2.0.1"}, {"port", 5001}},
        })},
        {"remux", json::object()},
    };
}

} // anonymous

TEST(GatewayCfg, RemuxAcceptsInputsArray) {
    auto cfg = parseGatewayCfg(baseRemuxJson());
    EXPECT_EQ(cfg.mode, GatewayMode::Remux);
    EXPECT_EQ(cfg.input.address, "239.1.0.1");
    ASSERT_EQ(cfg.extra_inputs.size(), 1u);
    EXPECT_EQ(cfg.extra_inputs[0].address, "239.1.0.2");
    EXPECT_EQ(cfg.outputs.size(), 1u);
}

TEST(GatewayCfg, InputAndInputsMutuallyExclusive) {
    json j = baseRemuxJson();
    j["input"] = j["inputs"][0];
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, RemuxRejectsSingleInput) {
    json j = baseRemuxJson();
    j["inputs"] = json::array({json{{"address", "239.1.0.1"}, {"port", 5000}}});
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, RemuxRejectsMultipleOutputs) {
    json j = baseRemuxJson();
    j["outputs"].push_back(json{{"id", "extra"}, {"address", "239.2.0.2"}, {"port", 5002}});
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, DemuxRejectsMultipleInputs) {
    json j = baseRemuxJson();
    j["mode"] = "demux";
    j.erase("remux");
    j["demux"] = json{
        {"routes", json::array({
            json{{"service_id", 1}, {"output_id", "mpts"}},
        })},
    };
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, RemuxPidRemapValidates) {
    json j = baseRemuxJson();
    j["remux"]["pid_remap"] = json::array({
        json{{"input_idx", 0}, {"src_pid", 256}, {"dst_pid", 256}},
        json{{"input_idx", 1}, {"src_pid", 256}, {"dst_pid", 512}},
    });
    auto cfg = parseGatewayCfg(j);
    ASSERT_EQ(cfg.remux.pid_remap.size(), 2u);
    EXPECT_EQ(cfg.remux.pid_remap[1].dst_pid, 512);
}

TEST(GatewayCfg, RemuxRejectsDuplicateDstPid) {
    json j = baseRemuxJson();
    j["remux"]["pid_remap"] = json::array({
        json{{"input_idx", 0}, {"src_pid", 256}, {"dst_pid", 256}},
        json{{"input_idx", 1}, {"src_pid", 257}, {"dst_pid", 256}},
    });
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, RemuxRejectsInputIdxOutOfRange) {
    json j = baseRemuxJson();
    j["remux"]["pid_remap"] = json::array({
        json{{"input_idx", 5}, {"src_pid", 256}, {"dst_pid", 256}},
    });
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, RemuxRoundTrip) {
    json j = baseRemuxJson();
    j["remux"] = json{
        {"target_bitrate_bps",  10'000'000},
        {"transport_stream_id", 42},
        {"original_network_id", 7},
        {"emit_sdt",            false},
        {"service_id_policy",   "reject"},
        {"pid_remap", json::array({
            json{{"input_idx", 1}, {"src_pid", 256}, {"dst_pid", 512}},
        })},
    };
    auto cfg = parseGatewayCfg(j);
    auto reparsed = parseGatewayCfg(toJson(cfg));
    EXPECT_EQ(reparsed.remux.target_bitrate_bps,   10'000'000u);
    EXPECT_EQ(reparsed.remux.transport_stream_id,  42);
    EXPECT_EQ(reparsed.remux.original_network_id,  7);
    EXPECT_FALSE(reparsed.remux.emit_sdt);
    EXPECT_EQ(reparsed.remux.service_id_policy,
              RemuxCfg::ServiceIdPolicy::Reject);
    ASSERT_EQ(reparsed.remux.pid_remap.size(), 1u);
    EXPECT_EQ(reparsed.remux.pid_remap[0].input_idx, 1);
    EXPECT_EQ(reparsed.remux.pid_remap[0].src_pid,   256);
    EXPECT_EQ(reparsed.remux.pid_remap[0].dst_pid,   512);
    EXPECT_EQ(reparsed.extra_inputs.size(), 1u);
}

// ─── fix-A6: TranscodeCfg ───────────────────────────────────────────────────

namespace {

json baseTranscodeJson() {
    return json{
        {"mode", "transcode"},
        {"input", json{{"address", "239.1.0.1"}, {"port", 5000}}},
        {"outputs", json::array({
            json{{"id", "spts"}, {"address", "239.2.0.1"}, {"port", 5001}},
        })},
        {"transcode", json::object()},
    };
}

} // anonymous

TEST(GatewayCfg, TranscodeAcceptsMinimal) {
    auto cfg = parseGatewayCfg(baseTranscodeJson());
    EXPECT_EQ(cfg.mode, GatewayMode::Transcode);
    EXPECT_EQ(cfg.input.address, "239.1.0.1");
    EXPECT_EQ(cfg.outputs.size(), 1u);
    // Defaults
    EXPECT_EQ(cfg.transcode.video_width,        1280);
    EXPECT_EQ(cfg.transcode.video_height,       720);
    EXPECT_EQ(cfg.transcode.video_fps,          25);
    EXPECT_EQ(cfg.transcode.video_bitrate_bps,  4'000'000u);
    EXPECT_EQ(cfg.transcode.video_encoder_mode, "auto");
    EXPECT_EQ(cfg.transcode.audio_bitrate_bps,  128'000u);
    EXPECT_TRUE(cfg.transcode.freeze_on_video_loss);
    EXPECT_TRUE(cfg.transcode.silence_on_audio_loss);
    EXPECT_EQ(cfg.transcode.pmt_pid,    0x100);
    EXPECT_EQ(cfg.transcode.video_pid,  0x101);
    EXPECT_EQ(cfg.transcode.audio_pid,  0x102);
}

TEST(GatewayCfg, TranscodeRejectsMultipleInputs) {
    json j = baseTranscodeJson();
    j.erase("input");
    j["inputs"] = json::array({
        json{{"address", "239.1.0.1"}, {"port", 5000}},
        json{{"address", "239.1.0.2"}, {"port", 5000}},
    });
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeRejectsMultipleOutputs) {
    json j = baseTranscodeJson();
    j["outputs"].push_back(json{{"address", "239.2.0.2"}, {"port", 5002}});
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeRejectsMissingBlock) {
    json j = baseTranscodeJson();
    j.erase("transcode");
    // Mode=transcode without "transcode" object: legal for round-trip of
    // empty defaults? No — parser requires explicit block to make intent
    // clear. RemuxCfg uses the same pattern.
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeRejectsBadEncoderMode) {
    json j = baseTranscodeJson();
    j["transcode"]["video_encoder_mode"] = "magic";
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeAcceptsAllEncoderModes) {
    for (const auto m : {"auto", "cpu", "x264", "nvenc", "qsv", "vaapi"}) {
        json j = baseTranscodeJson();
        j["transcode"]["video_encoder_mode"] = m;
        auto cfg = parseGatewayCfg(j);
        EXPECT_EQ(cfg.transcode.video_encoder_mode, m);
    }
}

TEST(GatewayCfg, TranscodeRejectsBadVideoDims) {
    json j = baseTranscodeJson();
    j["transcode"]["video_width"] = 0;
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
    j = baseTranscodeJson();
    j["transcode"]["video_fps"] = 200;
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
    j = baseTranscodeJson();
    j["transcode"]["video_bitrate_bps"] = 1000;          // < 16'000
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeRejectsPidCollision) {
    json j = baseTranscodeJson();
    j["transcode"]["video_pid"] = 0x102;     // collides with audio_pid default
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
    j = baseTranscodeJson();
    j["transcode"]["pmt_pid"]   = 0x101;     // collides with video_pid default
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeBackoffOrder) {
    json j = baseTranscodeJson();
    j["transcode"]["retry_backoff_ms_min"] = 1000;
    j["transcode"]["retry_backoff_ms_max"] = 500;
    EXPECT_THROW(parseGatewayCfg(j), std::invalid_argument);
}

TEST(GatewayCfg, TranscodeRoundTrip) {
    json j = baseTranscodeJson();
    j["transcode"] = json{
        {"video_width",        1920},
        {"video_height",       1080},
        {"video_fps",          50},
        {"video_bitrate_bps",  6'000'000},
        {"video_max_b_frames", 2},
        {"video_preset",       "fast"},
        {"video_encoder_mode", "nvenc"},
        {"video_gpu_index",    1},
        {"audio_bitrate_bps",  192'000},
        {"audio_sample_rate",  48000},
        {"audio_channels",     2},
        {"freeze_on_video_loss",  false},
        {"silence_on_audio_loss", false},
        {"loss_grace_ms",         5000},
        {"retry_backoff_ms_min",  500},
        {"retry_backoff_ms_max",  10000},
        {"fallback_logo_path",    "/var/lib/streaming-core/logo.png"},
        {"transport_stream_id",   42},
        {"original_network_id",   7},
        {"service_id",            101},
        {"service_name",          "Channel One"},
        {"provider_name",         "MyTV"},
        {"pmt_pid",               0x200},
        {"video_pid",             0x201},
        {"audio_pid",             0x202},
        {"pcr_pid",               0x201},   // ride-along on video_pid
        {"pat_period_ms",         200},
        {"pmt_period_ms",         200},
        {"sdt_period_ms",         3000},
        {"emit_sdt",              false},
        {"target_bitrate_bps",    7'500'000},
    };
    auto cfg      = parseGatewayCfg(j);
    auto reparsed = parseGatewayCfg(toJson(cfg));
    EXPECT_EQ(reparsed.transcode.video_width,        1920);
    EXPECT_EQ(reparsed.transcode.video_height,       1080);
    EXPECT_EQ(reparsed.transcode.video_fps,          50);
    EXPECT_EQ(reparsed.transcode.video_bitrate_bps,  6'000'000u);
    EXPECT_EQ(reparsed.transcode.video_max_b_frames, 2);
    EXPECT_EQ(reparsed.transcode.video_preset,       "fast");
    EXPECT_EQ(reparsed.transcode.video_encoder_mode, "nvenc");
    EXPECT_EQ(reparsed.transcode.video_gpu_index,    1);
    EXPECT_EQ(reparsed.transcode.audio_bitrate_bps,  192'000u);
    EXPECT_FALSE(reparsed.transcode.freeze_on_video_loss);
    EXPECT_FALSE(reparsed.transcode.silence_on_audio_loss);
    EXPECT_EQ(reparsed.transcode.loss_grace_ms,         5000u);
    EXPECT_EQ(reparsed.transcode.retry_backoff_ms_min,  500u);
    EXPECT_EQ(reparsed.transcode.retry_backoff_ms_max,  10000u);
    EXPECT_EQ(reparsed.transcode.fallback_logo_path,
              "/var/lib/streaming-core/logo.png");
    EXPECT_EQ(reparsed.transcode.transport_stream_id, 42);
    EXPECT_EQ(reparsed.transcode.original_network_id, 7);
    EXPECT_EQ(reparsed.transcode.service_id,          101);
    EXPECT_EQ(reparsed.transcode.service_name,        "Channel One");
    EXPECT_EQ(reparsed.transcode.provider_name,       "MyTV");
    EXPECT_EQ(reparsed.transcode.pmt_pid,             0x200);
    EXPECT_EQ(reparsed.transcode.video_pid,           0x201);
    EXPECT_EQ(reparsed.transcode.audio_pid,           0x202);
    EXPECT_EQ(reparsed.transcode.pcr_pid,             0x201);
    EXPECT_EQ(reparsed.transcode.pat_period_ms,       200);
    EXPECT_EQ(reparsed.transcode.target_bitrate_bps,  7'500'000u);
    EXPECT_FALSE(reparsed.transcode.emit_sdt);
}

TEST(GatewayCfg, PassthroughRoundTripStillUsesInput) {
    auto cfg = parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000}}},
        {"outputs", json::array({json{{"address","127.0.0.1"},{"port",6000}}})},
    });
    auto j = toJson(cfg);
    EXPECT_TRUE(j.contains("input"));
    EXPECT_FALSE(j.contains("inputs"));
}

// ─── FEC overlay (fix40 A7) ──────────────────────────────────────────────────

TEST(GatewayFecCfg, DefaultsAreDisabled) {
    auto f = parseFecCfg(json::object());
    EXPECT_FALSE(f.enabled);
    EXPECT_EQ(f.mode, FecCfg::Mode::OneD);
    EXPECT_EQ(f.L, 8);
    EXPECT_EQ(f.D, 8);
    EXPECT_EQ(f.payload_type, 33);
    EXPECT_EQ(f.ts_per_rtp, 7);
    EXPECT_EQ(f.column_port_offset, 2);
    EXPECT_EQ(f.row_port_offset, 4);
}

TEST(GatewayFecCfg, EnabledTwoD) {
    auto f = parseFecCfg(json{
        {"enabled", true}, {"mode", "2d"}, {"L", 10}, {"D", 10},
        {"ssrc", 0x12345678u}, {"payload_type", 96}, {"ts_per_rtp", 5},
        {"column_port_offset", 6}, {"row_port_offset", 8},
    });
    EXPECT_TRUE(f.enabled);
    EXPECT_EQ(f.mode, FecCfg::Mode::TwoD);
    EXPECT_EQ(f.L, 10);
    EXPECT_EQ(f.D, 10);
    EXPECT_EQ(f.ssrc, 0x12345678u);
    EXPECT_EQ(f.payload_type, 96);
    EXPECT_EQ(f.ts_per_rtp, 5);
    EXPECT_EQ(f.column_port_offset, 6);
    EXPECT_EQ(f.row_port_offset, 8);
}

TEST(GatewayFecCfg, RejectsUnknownMode) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"mode","3d"}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, RejectsLOutOfRange) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"L",0}}),
                 std::invalid_argument);
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"L",21}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, RejectsDOutOfRange) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"D",3}}),
                 std::invalid_argument);
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"D",21}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, RejectsLDProductOver100WhenEnabled) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"L",20},{"D",6}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, AllowsLargeProductWhenDisabled) {
    // Disabled FEC with stale large dimensions should not be rejected —
    // operator may persist the config, edit later, then enable.
    auto f = parseFecCfg(json{{"enabled",false},{"L",20},{"D",20}});
    EXPECT_FALSE(f.enabled);
}

TEST(GatewayFecCfg, RejectsCollidingPortOffsets) {
    EXPECT_THROW(parseFecCfg(json{
        {"enabled", true},
        {"column_port_offset", 4}, {"row_port_offset", 4},
    }), std::invalid_argument);
}

TEST(GatewayFecCfg, RejectsPayloadTypeOutOfRange) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"payload_type",128}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, RejectsTsPerRtpOutOfRange) {
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"ts_per_rtp",8}}),
                 std::invalid_argument);
    EXPECT_THROW(parseFecCfg(json{{"enabled",true},{"ts_per_rtp",0}}),
                 std::invalid_argument);
}

TEST(GatewayFecCfg, ToJsonElidesDefaults) {
    FecCfg f;
    auto j = toJson(f);
    EXPECT_TRUE(j.empty());
}

TEST(GatewayFecCfg, ToJsonRoundTrip) {
    auto orig = parseFecCfg(json{
        {"enabled", true}, {"mode", "2d"}, {"L", 5}, {"D", 4},
        {"ssrc", 0xDEADBEEFu}, {"payload_type", 96}, {"ts_per_rtp", 4},
        {"column_port_offset", 10}, {"row_port_offset", 12},
    });
    auto j = toJson(orig);
    auto rt = parseFecCfg(j);
    EXPECT_TRUE(rt.enabled);
    EXPECT_EQ(rt.mode, FecCfg::Mode::TwoD);
    EXPECT_EQ(rt.L, 5);
    EXPECT_EQ(rt.D, 4);
    EXPECT_EQ(rt.ssrc, 0xDEADBEEFu);
    EXPECT_EQ(rt.payload_type, 96);
    EXPECT_EQ(rt.ts_per_rtp, 4);
    EXPECT_EQ(rt.column_port_offset, 10);
    EXPECT_EQ(rt.row_port_offset, 12);
}

TEST(GatewayFecCfg, GatewayCfgIntegrationWithFec) {
    auto cfg = parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000}}},
        {"outputs", json::array({json{{"address","239.1.2.4"},{"port",6000}}})},
        {"fec",     {{"enabled", true}, {"mode", "2d"}, {"L", 8}, {"D", 8}}},
    });
    EXPECT_TRUE(cfg.fec.enabled);
    EXPECT_EQ(cfg.fec.mode, FecCfg::Mode::TwoD);
    auto j = toJson(cfg);
    EXPECT_TRUE(j.contains("fec"));
    EXPECT_TRUE(j["fec"]["enabled"].get<bool>());
}

TEST(GatewayFecCfg, RejectsPortOverflowWithFec) {
    // 2d mode → max offset = max(column=2, row=4) = 4. 65534 + 4 = 65538.
    EXPECT_THROW(parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000}}},
        {"outputs", json::array({json{{"address","239.1.2.4"},{"port",65534}}})},
        {"fec",     {{"enabled", true}, {"mode", "2d"}}},
    }), std::invalid_argument);
}

TEST(GatewayFecCfg, AllowsPortOverflowWhenDisabled) {
    auto cfg = parseGatewayCfg(json{
        {"input",   {{"address","239.1.2.3"},{"port",5000}}},
        {"outputs", json::array({json{{"address","239.1.2.4"},{"port",65534}}})},
        {"fec",     {{"enabled", false}, {"mode", "2d"}}},
    });
    EXPECT_FALSE(cfg.fec.enabled);
}

} // namespace
