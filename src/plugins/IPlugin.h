#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace liveqx::plugins {

// ABI version exported by every plugin. Bumping this requires every
// installed plugin to be rebuilt against the new headers; PluginManager
// rejects plugins with a mismatching version on dlopen.
inline constexpr std::uint32_t kPluginAbiVersion = 1;

// Symbols every plugin shared library must export. Exported with C
// linkage so name mangling does not get in the way of dlsym().
//
//     extern "C" std::uint32_t liveqx_plugin_abi_version();
//     extern "C" IPlugin* liveqx_plugin_init(const PluginContext*);
//     extern "C" const char* liveqx_plugin_attribution(); // optional
//
// Init returns ownership of an IPlugin to the host. The host calls
// onShutdown() before it drops the pointer; the plugin then deletes
// itself in onShutdown() (typical) or in a static teardown hook.
inline constexpr const char* kSymAbiVersion  = "liveqx_plugin_abi_version";
inline constexpr const char* kSymInit        = "liveqx_plugin_init";
inline constexpr const char* kSymAttribution = "liveqx_plugin_attribution";

// Information the host hands to the plugin on init. Populated by
// PluginManager from local config. Held by-pointer for ABI stability —
// adding a field at the tail does not break already-compiled plugins
// as long as they never read past their own struct length.
struct PluginContext {
    // Host-provided logger; plugin may keep this pointer for the lifetime
    // of the plugin instance. May be nullptr in test fixtures.
    spdlog::logger* logger{nullptr};

    // Absolute path of the directory the plugin was extracted into.
    // Plugin may store private state files here (allow-listed by
    // operator policy).
    const char* plugin_dir{nullptr};
};

// What a plugin may register with the host. Each list contains
// driver-name strings — the host queries factories by name and lets the
// plugin construct the actual driver. The plugin owns the lifecycle of
// any drivers it produces.
//
// fix19 ships only a couple of categories. New categories (decoders,
// transitions, …) get appended without breaking ABI as long as the
// host treats unknown driver-types as inert.
struct PluginCapabilities {
    std::vector<std::string> output_drivers;  // e.g. {"ndi"}
    std::vector<std::string> input_drivers;   // e.g. {"ndi"}
};

// The interface the host calls on the loaded plugin object. Methods
// are called from the main thread during install/uninstall; drivers
// produced by the plugin are responsible for their own concurrency.
class IPlugin {
public:
    virtual ~IPlugin() = default;

    // Plugin-author identity for /api/plugins listing.
    virtual const char* name()    const = 0;
    virtual const char* version() const = 0;

    // Capabilities the plugin advertises. Returned by-value; the host
    // copies it into PluginManager registry and never reads the plugin
    // memory again.
    virtual PluginCapabilities capabilities() const = 0;

    // Optional license/attribution text exposed via /api/version. May
    // return empty when EULA acceptance is sufficient.
    virtual const char* attributionText() const { return ""; }

    // Called once before unload. Plugin should release any threads,
    // connections, dlopen'd libs it owns. After return the host calls
    // dlclose() on the plugin's .so (best-effort — see fix19 design
    // doc for the libndi pending-unload caveat).
    virtual void onShutdown() {}
};

}  // namespace liveqx::plugins
