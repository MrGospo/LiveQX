#pragma once

// fix28 — runtime SIMD dispatch.
//
// On x86_64 the CPU may or may not support AVX2 (e.g. some older Xeon E5,
// virtualised cores with masked features, or ARM cross-builds). We runtime-
// detect the feature and let operators force a path via global config:
//
//   "simd": "auto"    — use AVX2 if the CPU supports it; else scalar.
//   "simd": "avx2"    — force AVX2; warns + falls back to scalar if absent.
//   "simd": "scalar"  — force scalar (used by CI to exercise the fallback).
//
// `current()` is a lock-free atomic read. Hot-path callers call it once per
// invocation — branch prediction makes the cost a single load + jcc.

#include <atomic>
#include <string>

namespace liveqx::simd {

enum class Mode : int {
    Scalar = 0,
    Avx2   = 1,
};

// True iff the host CPU advertises AVX2 (cpuid). Cached on first call.
bool avx2Available() noexcept;

// Returns the currently active mode (default Scalar until initFromConfig
// runs). Lock-free.
Mode current() noexcept;

// Apply a config string. Unknown values are treated as "auto" with a warn
// log. Safe to call once at startup; later calls just overwrite the mode
// (no thread fences — by design, the read side is racy-acceptable since
// we never flip mid-frame in production).
void initFromConfig(const std::string& cfg) noexcept;

// Stringify for logs.
const char* modeName(Mode m) noexcept;

}  // namespace liveqx::simd
