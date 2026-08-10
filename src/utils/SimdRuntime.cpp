#include "utils/SimdRuntime.h"

#include "utils/Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>

namespace liveqx::simd {

namespace {

std::atomic<Mode> g_mode{Mode::Scalar};

bool detectAvx2() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    // __builtin_cpu_supports must be preceded by __builtin_cpu_init on
    // older GCCs; both are no-ops past gcc-9 / clang-7.
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

bool avx2Available() noexcept {
    static const bool yes = detectAvx2();
    return yes;
}

Mode current() noexcept {
    return g_mode.load(std::memory_order_relaxed);
}

const char* modeName(Mode m) noexcept {
    switch (m) {
        case Mode::Scalar: return "scalar";
        case Mode::Avx2:   return "avx2";
    }
    return "unknown";
}

void initFromConfig(const std::string& cfg) noexcept {
    const std::string v = toLower(cfg.empty() ? "auto" : cfg);
    Mode chosen = Mode::Scalar;
    const bool have_avx2 = avx2Available();

    if (v == "scalar") {
        chosen = Mode::Scalar;
        LOG_INFO("SIMD: scalar (forced via config=scalar)");
    } else if (v == "avx2") {
        if (have_avx2) {
            chosen = Mode::Avx2;
            LOG_INFO("SIMD: avx2 (forced via config=avx2)");
        } else {
            chosen = Mode::Scalar;
            LOG_WARN("SIMD: config=avx2 requested but CPU lacks AVX2 — "
                     "falling back to scalar");
        }
    } else {
        // "auto" or unknown
        if (v != "auto") {
            LOG_WARN("SIMD: unknown config value '{}' — treating as 'auto'",
                     cfg);
        }
        if (have_avx2) {
            chosen = Mode::Avx2;
            LOG_INFO("SIMD: avx2 (CPU supports AVX2; config=auto)");
        } else {
            chosen = Mode::Scalar;
            LOG_INFO("SIMD: scalar (CPU lacks AVX2; config=auto fallback)");
        }
    }
    g_mode.store(chosen, std::memory_order_relaxed);
}

}  // namespace liveqx::simd
