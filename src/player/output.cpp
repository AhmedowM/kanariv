#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <atomic>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ring_fill.hpp"

namespace kanariv::player {

/// miniaudio playback device and mutable control flags, hidden from the header.
struct AudioOutput::Impl {
    AudioOutput::Config cfg;
    ma_device device{};
    std::atomic<float> volume{1.0f};
    std::atomic<bool> running{false};
    std::atomic<bool> initialized{false};

    /// Audio-thread entry point (backend trampoline).
    static void dataCallback(ma_device* pDevice, void* pOutput, const void* pInput,
                             ma_uint32 frameCount);
};

kanariv::types::DeviceList enumerateDevices() {
    kanariv::types::DeviceList list;
    ma_context context;
    ma_context_config ctxConfig = ma_context_config_init();
    ma_result res = ma_context_init(nullptr, 0, &ctxConfig, &context);
    if (res != MA_SUCCESS) {
        return list;
    }

    ma_device_info* pPlaybackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* pCaptureInfos = nullptr;
    ma_uint32 captureCount = 0;

    res = ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos,
                                 &captureCount);
    if (res == MA_SUCCESS && pPlaybackInfos && playbackCount > 0) {
        list.devices.reserve(playbackCount);
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            const auto& info = pPlaybackInfos[i];
            kanariv::types::DeviceInfo di;
            di.name = info.name;
            di.isDefault = info.isDefault;
            // Use simple index-based ID for user-friendly CLI
            di.id = std::to_string(i);
            list.devices.push_back(std::move(di));
        }
    }

    ma_context_uninit(&context);
    return list;
}

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    shutdown();
}

AudioOutput::Expected AudioOutput::create(const Config& cfg) {
    auto out = std::make_unique<AudioOutput>();
    if (!out->init(cfg)) {
        return std::unexpected(kanariv::utils::Error{
            kanariv::utils::StatusCode::Device, std::string_view("miniaudio device init failed")});
    }
    if (cfg.logger)
        cfg.logger->debug("audio output opened rate={} channels={}", cfg.sampleRate, cfg.channels);
    return out;
}

void AudioOutput::setVolume(float vol) {
    impl_->volume.store(kanariv::utils::clampVolume(vol), std::memory_order_relaxed);
}

float AudioOutput::volume() const noexcept {
    return impl_->volume.load(std::memory_order_relaxed);
}

void AudioOutput::start() {
    if (!impl_->initialized.load(std::memory_order_acquire))
        return;
    bool expected = false;
    if (impl_->running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ma_device_start(&impl_->device);
    }
}

void AudioOutput::stop() {
    bool expected = true;
    if (impl_->running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        ma_device_stop(&impl_->device);
    }
}

void AudioOutput::shutdown() noexcept {
    stop();
    if (impl_->initialized.exchange(false, std::memory_order_acq_rel)) {
        ma_device_uninit(&impl_->device);
    }
}

bool AudioOutput::init(const Config& cfg) {
    if (cfg.channels == 0 || cfg.channels > 32 || cfg.sampleRate == 0)
        return false;
    impl_->cfg = cfg;
    float v = kanariv::utils::clampVolume(cfg.volume);
    impl_->volume.store(v, std::memory_order_relaxed);

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = cfg.channels;
    deviceConfig.sampleRate = cfg.sampleRate;
    deviceConfig.dataCallback = &Impl::dataCallback;
    deviceConfig.pUserData = impl_.get();

    ma_result res = ma_device_init(nullptr, &deviceConfig, &impl_->device);
    if (res != MA_SUCCESS) {
        return false;
    }
    impl_->initialized.store(true, std::memory_order_release);

    // @pre caller must start() after preroll, cap/2 frames
    impl_->running.store(false, std::memory_order_release);

    return true;
}

void AudioOutput::Impl::dataCallback(ma_device* pDevice, void* pOutput, const void* pInput,
                                     ma_uint32 frameCount) {
    (void)pInput;
    auto* self = static_cast<Impl*>(pDevice->pUserData);
    if (!self)
        return;

    float* output = static_cast<float*>(pOutput);
    ma_uint32 channels = pDevice->playback.channels;
    ma_uint32 totalSamples = frameCount * channels;
    float vol = self->volume.load(std::memory_order_relaxed);
    // lock-free, no allocation
    detail::fillFromRing(std::span<float>(output, totalSamples), self->cfg.ring, channels, vol);
}

} // namespace kanariv::player
