#include "output/NdiLib.h"

#include <mutex>
#include <utility>

#include <dlfcn.h>

#include <spdlog/spdlog.h>

namespace liveqx::ndi {

namespace {

std::mutex                  g_singleton_mu;
std::weak_ptr<NdiLib>       g_singleton;

template <typename Fn>
bool resolve(void* h, const char* name, Fn& out_fn, std::string& err) {
    void* sym = ::dlsym(h, name);
    if (!sym) {
        err = std::string("missing libndi symbol: ") + name;
        return false;
    }
    out_fn = reinterpret_cast<Fn>(sym);
    return true;
}

}  // namespace

std::shared_ptr<NdiLib>
NdiLib::getOrLoad(const std::string& override_path,
                  std::shared_ptr<spdlog::logger> log) {
    std::lock_guard lk(g_singleton_mu);
    if (auto existing = g_singleton.lock())
        return existing;

    auto inst = std::make_shared<NdiLib>();

    auto& logger = log;
    if (!logger) logger = spdlog::default_logger();

    // Search order:
    //   1. explicit override_path (config or env)
    //   2. libndi.so.5  (SONAME on Linux distros that ship NDI SDK)
    //   3. libndi.so    (dev symlink some installs leave behind)
    if (!override_path.empty() && inst->tryLoad(override_path, logger)) {
        g_singleton = inst;
        return inst;
    }
    if (inst->tryLoad("libndi.so.5", logger)) { g_singleton = inst; return inst; }
    if (inst->tryLoad("libndi.so",   logger)) { g_singleton = inst; return inst; }

    // All attempts failed — keep the instance (caller checks ok()).
    g_singleton = inst;
    return inst;
}

NdiLib::NdiLib() = default;

NdiLib::~NdiLib() {
    // We do NOT dlclose libndi here. NDI 5.x spawns worker threads inside
    // initialize() that the SDK only tears down via process exit; calling
    // dlclose mid-process is documented as undefined and historically has
    // produced segfaults on Linux. Leak the handle — the OS reclaims it
    // when the process dies. See operator notes in docs/PLUGINS.md (c13).
    if (destroy && ok()) {
        try { destroy(); } catch (...) {}
    }
    handle_ = nullptr;
}

bool NdiLib::tryLoad(const std::string& path,
                     std::shared_ptr<spdlog::logger>& log) {
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* err = ::dlerror();
        last_error_ = std::string("dlopen('") + path + "') failed: "
                    + (err ? err : "?");
        return false;
    }

    // Reset dlerror() so failed resolve() actually surfaces missing symbol.
    (void)::dlerror();

    if (!resolve(h, "NDIlib_initialize",         initialize,      last_error_) ||
        !resolve(h, "NDIlib_destroy",            destroy,         last_error_) ||
        !resolve(h, "NDIlib_send_create",        send_create,     last_error_) ||
        !resolve(h, "NDIlib_send_destroy",       send_destroy,    last_error_) ||
        !resolve(h, "NDIlib_send_send_video_v2", send_send_v2,    last_error_) ||
        !resolve(h, "NDIlib_recv_create_v3",     recv_create_v3,  last_error_) ||
        !resolve(h, "NDIlib_recv_destroy",       recv_destroy,    last_error_) ||
        !resolve(h, "NDIlib_recv_capture_v2",    recv_capture_v2, last_error_) ||
        !resolve(h, "NDIlib_recv_free_video_v2", recv_free_video, last_error_) ||
        !resolve(h, "NDIlib_recv_free_audio_v2", recv_free_audio, last_error_)) {
        ::dlclose(h);
        return false;
    }
    // version_string is best-effort.
    void* v = ::dlsym(h, "NDIlib_version");
    if (v) version_string = reinterpret_cast<fn_version>(v);

    if (!initialize()) {
        last_error_ = "NDIlib_initialize() returned false (CPU lacks SSSE3? "
                      "license file missing?)";
        ::dlclose(h);
        return false;
    }

    handle_      = h;
    loaded_path_ = path;
    ok_.store(true, std::memory_order_release);

    log->info("ndi: libndi loaded from '{}' (version='{}')",
              path,
              version_string ? version_string() : "?");
    return true;
}

}  // namespace liveqx::ndi
