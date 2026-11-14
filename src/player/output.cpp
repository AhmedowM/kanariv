#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <atomic>
#include <expected>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
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
    /// Optional dedicated context when the silent null backend is requested;
    /// owned contexts must be paired with ma_context_uninit().
    ma_context nullContext{};
    bool ownsNullContext{false};
    std::atomic<float> volume{1.0f};
    std::atomic<bool> running{false};
    std::atomic<bool> initialized{false};
    /// Serializes init/start/stop/shutdown transitions. The real-time data
    /// callback never takes this mutex.
    std::mutex stateMutex;

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
    if (auto initResult = out->init(cfg); !initResult) {
        return std::unexpected(initResult.error());
    }
    if (cfg.logger)
        cfg.logger->debug("audio output opened rate={} channels={}{}", cfg.sampleRate, cfg.channels,
                          cfg.nullBackend ? " backend=null" : "");
    return out;
}

void AudioOutput::setVolume(float vol) {
    impl_->volume.store(kanariv::utils::clampVolume(vol), std::memory_order_relaxed);
}

float AudioOutput::volume() const noexcept {
    return impl_->volume.load(std::memory_order_relaxed);
}

AudioOutput::ExpectedVoid AudioOutput::start() {
    std::lock_guard<std::mutex> lk(impl_->stateMutex);
    if (!impl_->initialized.load(std::memory_order_acquire)) {
        return std::unexpected(kanariv::utils::Error{
            kanariv::utils::StatusCode::State, std::string_view("audio output not initialized")});
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return {};
    }
    // Backend first: the flag is committed only after the device actually
    // starts, so a failure cannot leave a stale "playing" state (P3).
    const ma_result res = ma_device_start(&impl_->device);
    if (res != MA_SUCCESS) {
        impl_->running.store(false, std::memory_order_release);
        const std::string msg =
            std::string("ma_device_start failed: ") + ma_result_description(res);
        if (impl_->cfg.logger)
            impl_->cfg.logger->warn("audio output start failed: {}", msg);
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::Device, std::string_view(msg)});
    }
    impl_->running.store(true, std::memory_order_release);
    return {};
}

AudioOutput::ExpectedVoid AudioOutput::stop() {
    std::lock_guard<std::mutex> lk(impl_->stateMutex);
    if (!impl_->running.load(std::memory_order_acquire)) {
        return {};
    }
    const ma_result res = ma_device_stop(&impl_->device);
    // Always clear the logical running state: callers must not observe a
    // "playing" device after a stop request, even if the backend complained.
    impl_->running.store(false, std::memory_order_release);
    if (res != MA_SUCCESS) {
        const std::string msg = std::string("ma_device_stop failed: ") + ma_result_description(res);
        if (impl_->cfg.logger)
            impl_->cfg.logger->warn("audio output stop failed: {}", msg);
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::Device, std::string_view(msg)});
    }
    return {};
}

void AudioOutput::shutdown() noexcept {
    std::lock_guard<std::mutex> lk(impl_->stateMutex);
    if (impl_->running.load(std::memory_order_acquire)) {
        (void)ma_device_stop(&impl_->device);
        impl_->running.store(false, std::memory_order_release);
    }
    if (impl_->initialized.exchange(false, std::memory_order_acq_rel)) {
        ma_device_uninit(&impl_->device);
    }
    if (impl_->ownsNullContext) {
        ma_context_uninit(&impl_->nullContext);
        impl_->ownsNullContext = false;
    }
}

AudioOutput::ExpectedVoid AudioOutput::init(const Config& cfg) {
    std::lock_guard<std::mutex> lk(impl_->stateMutex);
    // Validate the documented preconditions instead of silently accepting a
    // config the callback cannot serve (P4).
    if (impl_->initialized.load(std::memory_order_acquire)) {
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                  std::string_view("audio output already initialized")});
    }
    if (cfg.ring == nullptr) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("ring must not be null")});
    }
    if (cfg.sampleRate == 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("sampleRate must be > 0")});
    }
    if (cfg.channels == 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("channels must be > 0")});
    }
    if (cfg.channels > 32) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                                     std::string_view("channels must be <= 32")});
    }

    impl_->cfg = cfg;
    impl_->volume.store(kanariv::utils::clampVolume(cfg.volume), std::memory_order_relaxed);

    // Optional silent null backend for headless tests: initialize a private
    // context that only advertises ma_backend_null.
    ma_context* pContext = nullptr;
    if (cfg.nullBackend) {
        const ma_backend backends[] = {ma_backend_null};
        ma_context_config ctxConfig = ma_context_config_init();
        const ma_result ctxRes = ma_context_init(backends, 1, &ctxConfig, &impl_->nullContext);
        if (ctxRes != MA_SUCCESS) {
            const std::string msg =
                std::string("ma_context_init(null) failed: ") + ma_result_description(ctxRes);
            if (cfg.logger)
                cfg.logger->warn("audio output init failed: {}", msg);
            return std::unexpected(
                kanariv::utils::Error{kanariv::utils::StatusCode::Device, std::string_view(msg)});
        }
        impl_->ownsNullContext = true;
        pContext = &impl_->nullContext;
    }

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = cfg.channels;
    deviceConfig.sampleRate = cfg.sampleRate;
    deviceConfig.dataCallback = &Impl::dataCallback;
    deviceConfig.pUserData = impl_.get();

    const ma_result res = ma_device_init(pContext, &deviceConfig, &impl_->device);
    if (res != MA_SUCCESS) {
        if (impl_->ownsNullContext) {
            ma_context_uninit(&impl_->nullContext);
            impl_->ownsNullContext = false;
        }
        const std::string msg = std::string("ma_device_init failed: ") + ma_result_description(res);
        if (cfg.logger)
            cfg.logger->warn("audio output init failed: {}", msg);
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::Device, std::string_view(msg)});
    }
    impl_->initialized.store(true, std::memory_order_release);

    // @pre caller must start() after preroll, cap/2 frames
    impl_->running.store(false, std::memory_order_release);

    return {};
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
