// fix26 c13 — stress_runner_cli: standalone, blocking entry point that
// runs StressRunner::runOnce against a synthetic config, prints the
// resulting StressReport JSON to stdout, and exits 0/1 based on pass.
//
// Use cases:
//   - Operator wants to validate a stress JSON without a running daemon:
//       stress_runner_cli --config stress.json --validate-only
//   - CI smoke test fires a 1s synthetic run with channels=0 to confirm
//     the binary at least links + runs:
//       stress_runner_cli --config stress.json --report-out report.json
//   - Manual ad-hoc bench against a dedicated host (channels>0 + media
//     dirs set up out-of-band): same as above, longer duration_sec.
//
// We deliberately do NOT spin up the full daemon (no REST, no auth, no
// EventBus, no Watchdog). The CLI uses a bare ChannelManager; non-zero
// `channels` configs that need real media/outputs will fail to create
// channels — the runner notices via mgr_.create()'s return code and
// records crashes=N in the report. That's the correct outcome for an
// out-of-band CLI: the daemon is the path that actually sets up the
// rendering pipeline.
//
// Exit code: 0 if report.pass, 1 otherwise.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/ChannelManager.h"
#include "stress/StressConfig.h"
#include "stress/StressReport.h"
#include "stress/StressRunner.h"

using nlohmann::json;
using liveqx::stress::StressConfig;
using liveqx::stress::StressRunner;

namespace {

void printUsage() {
    std::cerr <<
        "Usage: stress_runner_cli --config <stress.json> [options]\n"
        "Options:\n"
        "  --config, -c <path>     stress config JSON (required)\n"
        "  --report-out <path>     write report JSON to file (default: stdout)\n"
        "  --validate-only         parse + validate the config and exit\n"
        "  --quiet                 suppress spdlog output (errors only)\n"
        "  --help, -h              show this help\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string config_path;
    std::string report_out;
    bool        validate_only = false;
    bool        quiet         = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--config" || a == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--report-out" && i + 1 < argc) {
            report_out = argv[++i];
        } else if (a == "--validate-only") {
            validate_only = true;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            printUsage();
            return 2;
        }
    }
    if (config_path.empty()) {
        std::cerr << "Error: --config is required\n";
        printUsage();
        return 2;
    }
    if (quiet) spdlog::set_level(spdlog::level::err);

    std::ifstream cf(config_path);
    if (!cf) {
        std::cerr << "Cannot open " << config_path << "\n";
        return 2;
    }
    json body;
    try { body = json::parse(cf); }
    catch (const std::exception& e) {
        std::cerr << "Config parse error: " << e.what() << "\n";
        return 2;
    }

    StressConfig cfg = StressConfig::fromJson(body);
    if (auto v = cfg.validate()) {
        std::cerr << "Invalid stress config: " << *v << "\n";
        return 2;
    }
    if (validate_only) {
        std::cout << "ok\n";
        return 0;
    }

    ChannelManager manager(nullptr);
    StressRunner   runner(manager);

    const auto rep = runner.runOnce(cfg, std::stop_token{});
    const auto out = rep.toJson().dump(2);

    if (!report_out.empty()) {
        std::ofstream of(report_out, std::ios::trunc);
        if (!of) {
            std::cerr << "Cannot open report output: " << report_out << "\n";
            return 2;
        }
        of << out << '\n';
    } else {
        std::cout << out << '\n';
    }
    return rep.pass ? 0 : 1;
}
