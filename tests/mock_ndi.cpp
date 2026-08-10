// fix19 c12 — mock libndi.so used by test_ndi_mock.cpp.
//
// Built as a SHARED library so NdiLib::tryLoad() can dlopen() it through
// the same code path it uses for the real NewTek SDK. We export every
// symbol NdiLib resolves (initialize/destroy/send_*/recv_*/version) plus
// a small mock_ndi_* getter API the test reads back to verify what the
// production code actually called.
//
// Lifetime: the mock is process-singleton just like real libndi —
// resetMockState() lets a test that wants a clean slate between
// assertions zero the counters explicitly.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "output/NdiAbi.h"

namespace {

// Sender state ---------------------------------------------------------------
std::atomic<int>          g_sender_alive{0};
std::atomic<std::uint64_t> g_send_video_count{0};
std::atomic<int>          g_last_xres{0};
std::atomic<int>          g_last_yres{0};
std::atomic<int>          g_last_fourcc{0};
std::atomic<int>          g_last_stride{0};

// Receiver state -------------------------------------------------------------
std::atomic<int>          g_recv_alive{0};
std::atomic<std::uint64_t> g_recv_capture_calls{0};

// A tiny phantom send/recv handle. Returning the same address every time
// is fine — NdiLib treats it as opaque.
char g_sender_phantom = 0;
char g_recv_phantom   = 0;

}  // namespace

extern "C" {

// ── NDIlib_* surface ────────────────────────────────────────────────────────
bool NDIlib_initialize() { return true; }
void NDIlib_destroy()    {}

const char* NDIlib_version() { return "mock-ndi/1.0"; }

void* NDIlib_send_create(const void* /*p_create*/) {
    g_sender_alive.fetch_add(1, std::memory_order_relaxed);
    return &g_sender_phantom;
}

void NDIlib_send_destroy(void* p_inst) {
    if (p_inst == &g_sender_phantom)
        g_sender_alive.fetch_sub(1, std::memory_order_relaxed);
}

void NDIlib_send_send_video_v2(void* p_inst, const void* p_video) {
    if (p_inst != &g_sender_phantom || !p_video) return;
    const auto* v =
        static_cast<const liveqx::ndi::abi::video_frame_v2_t*>(p_video);
    g_last_xres.store(v->xres,                std::memory_order_relaxed);
    g_last_yres.store(v->yres,                std::memory_order_relaxed);
    g_last_fourcc.store(v->FourCC,            std::memory_order_relaxed);
    g_last_stride.store(v->line_stride_in_bytes,
                        std::memory_order_relaxed);
    g_send_video_count.fetch_add(1, std::memory_order_relaxed);
}

void* NDIlib_recv_create_v3(const void* /*p_create*/) {
    g_recv_alive.fetch_add(1, std::memory_order_relaxed);
    return &g_recv_phantom;
}

void NDIlib_recv_destroy(void* p_inst) {
    if (p_inst == &g_recv_phantom)
        g_recv_alive.fetch_sub(1, std::memory_order_relaxed);
}

int NDIlib_recv_capture_v2(void* /*p_inst*/,
                           void* /*p_video*/,
                           void* /*p_audio*/,
                           void* /*p_metadata*/,
                           std::uint32_t /*timeout_ms*/) {
    g_recv_capture_calls.fetch_add(1, std::memory_order_relaxed);
    return liveqx::ndi::abi::frame_type_none;
}

void NDIlib_recv_free_video_v2(void*, const void*) {}
void NDIlib_recv_free_audio_v2(void*, const void*) {}

// ── Test inspection API — *not* part of NDI; symbols are namespaced
//    with mock_ndi_ to avoid colliding with anything libndi could ship.
std::uint64_t mock_ndi_send_count()  { return g_send_video_count.load(std::memory_order_relaxed); }
int           mock_ndi_last_xres()   { return g_last_xres.load(std::memory_order_relaxed); }
int           mock_ndi_last_yres()   { return g_last_yres.load(std::memory_order_relaxed); }
int           mock_ndi_last_fourcc() { return g_last_fourcc.load(std::memory_order_relaxed); }
int           mock_ndi_last_stride() { return g_last_stride.load(std::memory_order_relaxed); }
int           mock_ndi_sender_alive(){ return g_sender_alive.load(std::memory_order_relaxed); }
int           mock_ndi_recv_alive()  { return g_recv_alive.load(std::memory_order_relaxed); }
std::uint64_t mock_ndi_recv_capture_calls() {
    return g_recv_capture_calls.load(std::memory_order_relaxed);
}

void mock_ndi_reset() {
    g_send_video_count.store(0, std::memory_order_relaxed);
    g_last_xres.store(0, std::memory_order_relaxed);
    g_last_yres.store(0, std::memory_order_relaxed);
    g_last_fourcc.store(0, std::memory_order_relaxed);
    g_last_stride.store(0, std::memory_order_relaxed);
    g_recv_capture_calls.store(0, std::memory_order_relaxed);
}

}  // extern "C"
