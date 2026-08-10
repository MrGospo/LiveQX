#pragma once
//
// Runtime-loader libndi.so.5 — резолвит индивидуальные NDIlib_*
// символы через dlsym (вместо NDIlib_v5_load() pointer-table). Так
// проще: NewTek экспортирует и flat-API, а struct layout v5_load
// чувствителен к ABI-bump'ам.
//
// Жизненный цикл: один процесс-уровневый instance (lazy init).
// NdiOutput/NdiInput держат shared_ptr<NdiLib>, чтобы libndi
// оставалась загруженной пока есть хоть один потребитель. Реальный
// dlclose — на process exit (если делать раньше, NDI worker threads
// в библиотеке могут продолжить дёргать наши указатели).
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "output/NdiAbi.h"

namespace spdlog { class logger; }

namespace liveqx::ndi {

class NdiLib {
public:
    // Конкретный путь к libndi (например "/opt/ndi/lib/libndi.so.5"
    // когда оператор поставил SDK не в системный prefix). Пустая
    // строка → пробуем "libndi.so.5" → "libndi.so".
    static std::shared_ptr<NdiLib> getOrLoad(const std::string& override_path = "",
                                             std::shared_ptr<spdlog::logger> log = {});

    NdiLib();
    ~NdiLib();
    NdiLib(const NdiLib&)            = delete;
    NdiLib& operator=(const NdiLib&) = delete;

    // True ⇒ libndi successfully dlopen'ed + NDIlib_initialize() returned ok.
    bool ok() const noexcept { return ok_.load(std::memory_order_acquire); }

    // Last failure message (set when ok() is false).
    const std::string& lastError() const noexcept { return last_error_; }

    // ── Resolved symbols ─────────────────────────────────────────────
    using fn_initialize     = bool         (*)(void);
    using fn_destroy        = void         (*)(void);
    using fn_send_create    = abi::send_instance_t (*)(const abi::send_create_t*);
    using fn_send_destroy   = void         (*)(abi::send_instance_t);
    using fn_send_send_v2   = void         (*)(abi::send_instance_t,
                                               const abi::video_frame_v2_t*);
    using fn_version        = const char*  (*)(void);

    using fn_recv_create_v3   = abi::recv_instance_t (*)(const abi::recv_create_v3_t*);
    using fn_recv_destroy     = void                 (*)(abi::recv_instance_t);
    using fn_recv_capture_v2  = int                  (*)(abi::recv_instance_t,
                                                         abi::video_frame_v2_t*,
                                                         abi::audio_frame_v2_t*,
                                                         void*,
                                                         std::uint32_t);
    using fn_recv_free_video  = void                 (*)(abi::recv_instance_t,
                                                         const abi::video_frame_v2_t*);
    using fn_recv_free_audio  = void                 (*)(abi::recv_instance_t,
                                                         const abi::audio_frame_v2_t*);

    fn_initialize     initialize     = nullptr;
    fn_destroy        destroy        = nullptr;
    fn_send_create    send_create    = nullptr;
    fn_send_destroy   send_destroy   = nullptr;
    fn_send_send_v2   send_send_v2   = nullptr;
    fn_version        version_string = nullptr;

    fn_recv_create_v3 recv_create_v3 = nullptr;
    fn_recv_destroy   recv_destroy   = nullptr;
    fn_recv_capture_v2 recv_capture_v2 = nullptr;
    fn_recv_free_video recv_free_video = nullptr;
    fn_recv_free_audio recv_free_audio = nullptr;

private:
    bool tryLoad(const std::string& path, std::shared_ptr<spdlog::logger>& log);

    void*              handle_ = nullptr;
    std::atomic<bool>  ok_{false};
    std::string        last_error_;
    std::string        loaded_path_;
};

}  // namespace liveqx::ndi
