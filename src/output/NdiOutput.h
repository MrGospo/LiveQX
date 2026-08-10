#pragma once
//
// IOutput-совместимый driver для NDI 5.x. На c6 это ещё скелет:
//   • start()         — open libndi (через NdiLib singleton),
//                       создать NDIlib_send instance с именем из конфига.
//   • stop()          — destroy sender + drop refs.
//   • send(Packet)    — на c6 не делает ничего полезного: NDI принимает
//                       UNCOMPRESSED video frames, а Packet от encoder'а —
//                       уже H.264/MPEG-TS. Реальный raw-frame путь
//                       подключается в commit 7/15 (compositor pre-encoder
//                       hook). До того момента send() считает входящие
//                       пакеты, чтобы статистика была не пустой, и
//                       логирует one-shot warning при первом вызове.
//   • statusJson()    — добавляет ndi_loaded/ndi_path/ndi_name к стандарту.
//
// Если libndi не загружается — start() возвращает false; ChannelInstance
// тогда логирует "outputs[id=…]: ndi unavailable — skipped" и продолжает
// без NDI-выхода. Это сознательный выбор: lack of NDI library не должен
// валить весь канал.
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Frame.h"
#include "output/IOutput.h"
#include "output/NdiAbi.h"
#include "output/NdiOutputCfg.h"

class Encoder;

namespace liveqx::ndi {

class NdiLib;

class NdiOutput : public IOutput,
                  public std::enable_shared_from_this<NdiOutput> {
public:
    explicit NdiOutput(OutputCfg cfg);
    ~NdiOutput() override;

    bool        start() override;
    void        stop() override;
    void        send(const Packet& pkt) override;
    bool        isHealthy() const override;
    OutputStats getStats() const override;
    nlohmann::json statusJson() const override;

    void setLogger(std::shared_ptr<spdlog::logger> log) override { log_ = std::move(log); }
    void setChannelId(std::string id)                  override { channel_id_ = std::move(id); }

    // Wires NdiOutput to the channel's encoder via the pre-encode tap.
    // Must be called after start() returned true and the encoder exists.
    // Captures std::weak_ptr<NdiOutput> so a hot-removed driver becomes a
    // no-op tap rather than a UAF on the render thread.
    void attachEncoder(Encoder* enc, int fps);

private:
    void sendRawFrame(const Frame& f);

    OutputCfg                     cfg_;
    std::shared_ptr<spdlog::logger> log_;
    std::string                   channel_id_;

    std::shared_ptr<NdiLib>       lib_;
    abi::send_instance_t          sender_  = nullptr;

    mutable std::mutex            mu_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             warned_encoded_packet_{false};
    std::atomic<std::uint64_t>    packets_dropped_encoded_{0};
    std::atomic<std::uint64_t>    packets_received_{0};
    std::atomic<std::uint64_t>    bytes_received_{0};

    // Frame-send path (populated lazily on first sendRawFrame call).
    int                           fps_n_ = 25;
    std::vector<std::uint8_t>     bgra_scratch_;        // render thread only
    std::atomic<std::uint64_t>    frames_sent_{0};
    std::atomic<std::uint64_t>    frames_dropped_{0};
    std::atomic<bool>             warned_frame_size_mismatch_{false};
};

}  // namespace liveqx::ndi
