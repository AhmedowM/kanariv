#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <atomic>
#include <expected>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/status_format.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "device_id.hpp"
#include "ring_fill.hpp"

namespace kanariv::player {

namespace {

/// Backend tag stored in the opaque device id. Kept short and stable: it is
/// persisted in config.json, so renaming one would orphan saved selections.
std::string_view backendTag(ma_backend backend) {
    switch (backend) {
    case ma_backend_wasapi:
        return "wasapi";
    case ma_backend_dsound:
        return "dsound";
    case ma_backend_winmm:
        return "winmm";
    case ma_backend_coreaudio:
        return "coreaudio";
    case ma_backend_sndio:
        return "sndio";
    case ma_backend_audio4:
        return "audio4";
    case ma_backend_oss:
        return "oss";
    case ma_backend_pulseaudio:
        return "pulse";
    case ma_backend_alsa:
        return "alsa";
    case ma_backend_jack:
        return "jack";
    case ma_backend_aaudio:
        return "aaudio";
    case ma_backend_opensl:
        return "opensl";
    case ma_backend_webaudio:
        return "webaudio";
    case ma_backend_null:
        return "null";
    case ma_backend_custom:
        break;
    }
    return "custom";
}

std::optional<ma_backend> backendFromTag(std::string_view tag) {
    // ma_backend_custom is deliberately absent: it needs caller-supplied
    // callbacks, so a persisted custom tag can never be reopened from config.
    if (tag == "wasapi")
        return ma_backend_wasapi;
    if (tag == "dsound")
        return ma_backend_dsound;
    if (tag == "winmm")
        return ma_backend_winmm;
    if (tag == "coreaudio")
        return ma_backend_coreaudio;
    if (tag == "sndio")
        return ma_backend_sndio;
    if (tag == "audio4")
        return ma_backend_audio4;
    if (tag == "oss")
        return ma_backend_oss;
    if (tag == "pulse")
        return ma_backend_pulseaudio;
    if (tag == "alsa")
        return ma_backend_alsa;
    if (tag == "jack")
        return ma_backend_jack;
    if (tag == "aaudio")
        return ma_backend_aaudio;
    if (tag == "opensl")
        return ma_backend_opensl;
    if (tag == "webaudio")
        return ma_backend_webaudio;
    if (tag == "null")
        return ma_backend_null;
    return std::nullopt;
}

std::size_t stringBytes(const char* s, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && s[n] != '\0')
        ++n;
    return n;
}

/// Bytes of the union member the backend fills. Only these take part in the
/// encoded id, so zero padding and pointer members never enter a persisted
/// value.
std::size_t deviceIdBytes(ma_backend backend, const ma_device_id& id) {
    switch (backend) {
    case ma_backend_wasapi: {
        std::size_t n = 0;
        while (n < 64 && id.wasapi[n] != 0)
            ++n;
        return n * sizeof(ma_wchar_win32);
    }
    case ma_backend_dsound:
        return 16;
    case ma_backend_winmm:
        return sizeof(id.winmm);
    case ma_backend_coreaudio:
        return stringBytes(id.coreaudio, 256);
    case ma_backend_sndio:
        return stringBytes(id.sndio, 256);
    case ma_backend_audio4:
        return stringBytes(id.audio4, 256);
    case ma_backend_oss:
        return stringBytes(id.oss, 64);
    case ma_backend_pulseaudio:
        return stringBytes(id.pulse, 256);
    case ma_backend_alsa:
        return stringBytes(id.alsa, 256);
    case ma_backend_jack:
        return sizeof(id.jack);
    case ma_backend_aaudio:
        return sizeof(id.aaudio);
    case ma_backend_opensl:
        return sizeof(id.opensl);
    case ma_backend_webaudio:
        return stringBytes(id.webaudio, 32);
    case ma_backend_null:
        return sizeof(id.nullbackend);
    case ma_backend_custom:
        break;
    }
    return sizeof(ma_device_id);
}

std::string encodeDeviceId(ma_backend backend, const ma_device_id& id) {
    const std::size_t bytes = deviceIdBytes(backend, id);
    return detail::formatDeviceId(
        backendTag(backend),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&id), bytes));
}

struct ResolvedDevice {
    ma_backend backend;
    ma_device_id id;
    bool matchedById{false};
};

/// Scans one context's playback devices: encoded id first, then display name,
/// then a legacy enumeration index.
std::optional<ResolvedDevice> findDevice(ma_context& context, std::string_view key,
                                         std::string_view name,
                                         std::optional<std::uint32_t> legacyIndex) {
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    ma_device_info* captureInfos = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&context, &infos, &count, &captureInfos, &captureCount) !=
            MA_SUCCESS ||
        !infos)
        return std::nullopt;
    for (ma_uint32 i = 0; i < count; ++i) {
        if (encodeDeviceId(context.backend, infos[i].id) == key)
            return ResolvedDevice{context.backend, infos[i].id, true};
    }
    if (!name.empty()) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (name == infos[i].name)
                return ResolvedDevice{context.backend, infos[i].id, false};
        }
    }
    if (legacyIndex && *legacyIndex < count)
        return ResolvedDevice{context.backend, infos[*legacyIndex].id, false};
    return std::nullopt;
}

/// Resolves a configured device reference. A tagged id is looked up on its own
/// backend first (the default context may have picked another backend); names
/// and legacy indices use the default backend list.
std::optional<ResolvedDevice> resolveConfiguredDevice(std::string_view key,
                                                      std::string_view name) {
    if (key.empty() || key == "auto")
        return std::nullopt;
    if (const auto tag = detail::deviceIdBackend(key)) {
        if (const auto backend = backendFromTag(*tag); backend && ma_is_backend_enabled(*backend)) {
            ma_context context{};
            ma_context_config contextConfig = ma_context_config_init();
            const ma_backend backends[] = {*backend};
            if (ma_context_init(backends, 1, &contextConfig, &context) == MA_SUCCESS) {
                auto found = findDevice(context, key, name, std::nullopt);
                ma_context_uninit(&context);
                if (found)
                    return found;
            }
        }
    }
    ma_context context{};
    ma_context_config contextConfig = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &contextConfig, &context) != MA_SUCCESS)
        return std::nullopt;
    auto found = findDevice(context, key, name, legacyDeviceIndex(key));
    ma_context_uninit(&context);
    return found;
}

} // namespace

/// miniaudio playback device and mutable control flags, hidden from the header.
struct AudioOutput::Impl {
    AudioOutput::Config cfg;
    ma_device device{};
    /// Dedicated context for a configured backend or the silent null backend;
    /// owned contexts must be paired with ma_context_uninit().
    ma_context deviceContext{};
    bool ownsDeviceContext{false};
    /// Device handed to ma_device_init when a configured device resolved.
    ma_device_id selectedDeviceId{};
    bool hasSelectedDevice{false};
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

std::optional<std::uint32_t> legacyDeviceIndex(std::string_view id) {
    if (id.empty() || id.size() > 10)
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : id) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > 0xffffffffULL)
            return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

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
            di.id = encodeDeviceId(context.backend, info.id);
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
    impl_->hasSelectedDevice = false;
    if (impl_->ownsDeviceContext) {
        ma_context_uninit(&impl_->deviceContext);
        impl_->ownsDeviceContext = false;
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
    // context that only advertises ma_backend_null. Otherwise resolve the
    // configured device (id, then name, then legacy index) on an owned context
    // bound to its backend; a reference that no longer resolves falls back to
    // the default device with a warning instead of failing playback.
    ma_context* pContext = nullptr;
    if (cfg.nullBackend) {
        const ma_backend backends[] = {ma_backend_null};
        ma_context_config ctxConfig = ma_context_config_init();
        const ma_result ctxRes = ma_context_init(backends, 1, &ctxConfig, &impl_->deviceContext);
        if (ctxRes != MA_SUCCESS) {
            const std::string msg =
                std::string("ma_context_init(null) failed: ") + ma_result_description(ctxRes);
            if (cfg.logger)
                cfg.logger->warn("audio output init failed: {}", msg);
            return std::unexpected(
                kanariv::utils::Error{kanariv::utils::StatusCode::Device, std::string_view(msg)});
        }
        impl_->ownsDeviceContext = true;
        pContext = &impl_->deviceContext;
    } else if (const auto resolved = resolveConfiguredDevice(cfg.deviceId, cfg.deviceName)) {
        const ma_backend backends[] = {resolved->backend};
        ma_context_config ctxConfig = ma_context_config_init();
        const ma_result ctxRes =
            ma_context_init(backends, 1, &ctxConfig, &impl_->deviceContext);
        if (ctxRes == MA_SUCCESS) {
            impl_->ownsDeviceContext = true;
            impl_->selectedDeviceId = resolved->id;
            impl_->hasSelectedDevice = true;
            pContext = &impl_->deviceContext;
            if (cfg.logger && !resolved->matchedById)
                cfg.logger->debug("audio device '{}' matched by name", cfg.deviceName);
        } else if (cfg.logger) {
            cfg.logger->warn("audio device '{}' context unavailable: {}; using the default device",
                             cfg.deviceId, ma_result_description(ctxRes));
        }
    } else if (!cfg.deviceId.empty() && cfg.deviceId != "auto" && cfg.logger) {
        cfg.logger->warn("audio device '{}' is not available; using the default device",
                         cfg.deviceId);
    }

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = cfg.channels;
    deviceConfig.sampleRate = cfg.sampleRate;
    deviceConfig.dataCallback = &Impl::dataCallback;
    deviceConfig.pUserData = impl_.get();
    if (impl_->hasSelectedDevice)
        deviceConfig.playback.pDeviceID = &impl_->selectedDeviceId;

    const ma_result res = ma_device_init(pContext, &deviceConfig, &impl_->device);
    if (res != MA_SUCCESS) {
        impl_->hasSelectedDevice = false;
        if (impl_->ownsDeviceContext) {
            ma_context_uninit(&impl_->deviceContext);
            impl_->ownsDeviceContext = false;
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
