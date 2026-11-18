#pragma once

/**
 * @file output.hpp
 * @brief Audio output management using miniaudio
 * @ingroup kanariv_player
 *
 * This header provides audio device enumeration and playback functionality
 * using the miniaudio library. It includes:
 * - DeviceInfo/DeviceList: Audio device metadata structures
 * - enumerateDevices(): System audio device enumeration via miniaudio context
 * - AudioOutput: Lock-free audio playback with ring buffer integration
 *
 * @par Thread safety
 * - enumerateDevices(): Thread-safe (creates/destroys miniaudio context internally)
 * - AudioOutput::create(): Thread-safe
 * - AudioOutput methods: Thread-safe for control (start/stop/setVolume),
 *   the audio callback runs on the audio thread (real-time priority)
 *
 * miniaudio Integration:
 * - Uses ma_context for device enumeration
 * - Uses ma_device for playback with float32 format
 * - Data callback runs on miniaudio's internal audio thread
 * - Lock-free ring buffer communication between decode and audio threads
 *
 * Error Codes:
 * - Device: miniaudio device initialization/start/stop failed
 * - InvalidArg: Invalid configuration (sample rate, channels, null ring)
 * - NoMem: Allocation failure
 */

#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kanariv::player {

/**
 * @brief Enumerate all available audio playback devices
 * @return DeviceList containing all playback devices
 *
 * Creates a miniaudio context, queries the system for playback devices,
 * and returns a list of DeviceInfo structures. The miniaudio context
 * is created and destroyed within this function call.
 *
 * Device IDs are opaque backend-qualified strings (`<backend>:<hex>`) derived
 * from the backend's own device identifier, so they survive re-enumeration
 * when the backend exposes a stable id; they are not guaranteed stable across
 * backend or system changes. The default device is marked with
 * isDefault = true.
 *
 * @par Thread safety
 * Thread-safe. Each call creates and destroys its own
 * miniaudio context. Can be called concurrently.
 *
 * Error Handling: On miniaudio context initialization failure, returns
 * an empty DeviceList. No exceptions thrown.
 *
 * miniaudio Integration:
 * - Uses ma_context_init() with default config
 * - Queries devices via ma_context_get_devices()
 * - Cleans up with ma_context_uninit()
 * - Only returns playback devices (capture devices ignored)
 */
KANARIV_API kanariv::types::DeviceList enumerateDevices();

/**
 * @brief Interprets a legacy numeric device id.
 * @param id Candidate device id from an old config or command.
 * @return The enumeration index when the whole string is decimal digits that
 *         fit a uint32 (configs written before ids became opaque stored the
 *         index), nullopt for every other spelling.
 */
KANARIV_API std::optional<std::uint32_t> legacyDeviceIndex(std::string_view id);

/**
 * @class AudioOutput
 * @brief Lock-free audio output device with ring buffer integration
 * @ingroup kanariv_player
 *
 * Manages a miniaudio playback device that consumes float32 samples from
 * a lock-free SPSC ring buffer. Designed for real-time audio playback
 * with minimal latency and no allocations in the audio callback.
 *
 * @par Thread safety
 * - create()/init()/shutdown(): Thread-safe, called from control thread
 * - start()/stop(): serialized by an internal mutex; the audio callback never
 *   takes that mutex, so start/stop may block on the backend but never recurse
 *   into the real-time callback.
 * - setVolume()/volume(): Thread-safe (atomic)
 * - audio callback: runs on the audio thread (real-time), lock-free
 *
 * Audio Pipeline:
 * 1. Decode thread writes float samples to SPSC ring buffer
 * 2. the backend invokes the audio callback on the audio thread
 * 3. The callback reads from the ring, applies volume and zero-fills any
 *    remaining buffer space (silence on underrun)
 *
 * Volume Control:
 * - Volume is applied in the audio callback via atomic load (lock-free)
 * - Range: 0.0 to 1.0, clamped with utils::clampVolume()
 * - Changes take effect on next audio callback
 *
 * Lifecycle:
 *   create() (which calls init()) -> start() -> (playback) -> stop() -> shutdown()
 *   A default-constructed object may call init() explicitly.
 *   Destructor calls shutdown() automatically.
 */
class KANARIV_API AudioOutput {
  public:
    using Expected = kanariv::utils::Expected<std::unique_ptr<AudioOutput>>;
    /** @brief Result of control operations that produce no value. */
    using ExpectedVoid = kanariv::utils::Expected<void>;

    /**
     * @struct Config
     * @brief Audio output configuration parameters
     * @ingroup kanariv_player
     */
    struct Config {
        /** @brief Output sample rate in hertz. */
        uint32_t sampleRate = 48000;
        /** @brief Number of output channels. */
        uint32_t channels = 2;
        /** @brief SPSC ring buffer consumed by the audio callback. */
        kanariv::utils::SpscRing<float>* ring = nullptr;
        /** @brief Initial volume, clamped to the range [0, 1]. */
        float volume = 1.0f;
        /**
         * @brief Non-owning logger; null disables logging.
         * @par Ownership
         * The logger must outlive the AudioOutput.
         */
        kanariv::utils::Logger* logger{nullptr};
        /**
         * @brief Use miniaudio's silent null backend instead of a real device.
         *
         * The null backend initializes always and drives the data callback at
         * the configured sample rate, but writes nowhere. Intended for
         * headless tests and simulations: it never beeps. Only the f32 format
         * is supported (the device format is fixed).
         */
        bool nullBackend = false;
        /**
         * @brief Configured playback device: an opaque id from
         * enumerateDevices(), a device name, or a legacy numeric index.
         *
         * Empty and "auto" select the default device. A reference that no
         * longer resolves logs a warning and falls back to the default device,
         * so a stale config never blocks playback.
         */
        std::string deviceId{};
        /** @brief Display name paired with deviceId.
         *
         * Tried when the id is not enumerated anymore (for example after a
         * backend change) before falling back to the default device.
         */
        std::string deviceName{};
    };

    /** @brief Default-constructs an uninitialized output; use create(). */
    AudioOutput();

    /** @brief Shuts down the device. */
    ~AudioOutput();

    /**
     * @brief Create and initialize an AudioOutput instance
     * @param cfg Configuration for the audio output
     * @return Expected containing AudioOutput on success, Error on failure
     *
     * Factory method that creates an AudioOutput and calls init() to initialize
     * the miniaudio device. Returns an error if device initialization fails.
     *
     * @retval StatusCode::Device miniaudio device init failed
     * @retval StatusCode::InvalidArg Invalid config (0 channels, 0 sample rate, null ring,
     *         already initialized)
     * @retval StatusCode::Unsupported Channel count above 32
     *
     * @par Thread safety
     * Thread-safe; can be called from any thread.
     */
    static Expected create(const Config& cfg);

    /**
     * @brief Set playback volume
     * @param vol Volume level (0.0 = mute, 1.0 = full)
     *
     * Volume is clamped to [0.0, 1.0] and applied atomically.
     * Takes effect on the next audio callback.
     *
     * @par Thread safety
     * Thread-safe; stores the volume atomically.
     */
    void setVolume(float vol);

    /**
     * @brief Get current playback volume
     * @return Current volume level (0.0 to 1.0)
     *
     * @par Thread safety
     * Thread-safe; loads the volume atomically.
     */
    float volume() const noexcept;

    /**
     * @brief Start audio playback
     * @return void on success, Error on failure
     *
     * Starts the miniaudio device. The backend is started first and the
     * internal running flag is committed only on success, so a failed start
     * never leaves the output looking active.
     *
     * @retval StatusCode::State Output is not initialized
     * @retval StatusCode::Device ma_device_start() failed
     *
     * @par Thread safety
     * Thread-safe; serialized against start()/stop()/shutdown() by an
     * internal mutex (never taken by the audio callback).
     * @par Blocking
     * May block while the backend starts the audio device.
     */
    ExpectedVoid start();

    /**
     * @brief Stop audio playback
     * @return void on success, Error on failure
     *
     * Stops the miniaudio device and clears the running flag even when the
     * backend reports an error, so callers never observe a stale "playing"
     * state.
     *
     * @retval StatusCode::Device ma_device_stop() failed
     *
     * @par Thread safety
     * Thread-safe; serialized against start()/stop()/shutdown() by an
     * internal mutex (never taken by the audio callback).
     */
    ExpectedVoid stop();

    /**
     * @brief Shutdown and release audio device resources
     *
     * Stops the device if running, then uninitializes the miniaudio device.
     * Safe to call multiple times. Called automatically by destructor.
     *
     * @par Thread safety
     * Thread-safe; idempotently exchanges the initialized flag.
     */
    void shutdown() noexcept;

    /**
     * @brief Initialize the miniaudio playback device
     * @param cfg Device configuration
     * @return void on success, Error on failure
     *
     * Validates the documented preconditions, then configures and initializes
     * the miniaudio device with float32 format. This is normally called by
     * create(); call it directly only on a default-constructed or shut-down
     * object (calling it twice is rejected instead of leaking the device).
     * Sets up the data callback for lock-free ring buffer consumption.
     * Device starts in stopped state; caller must call start() after preroll.
     *
     * The configured deviceId is resolved against the current enumeration
     * (then deviceName, then a legacy numeric index). A reference that does
     * not resolve is logged as a warning and the default device is opened.
     *
     * @pre cfg.ring != nullptr, cfg.sampleRate > 0, 1 <= cfg.channels <= 32,
     * object not already initialized, and the fixed f32 format (only supported
     * format).
     *
     * @retval StatusCode::InvalidArg Null ring, zero rate/channels, or already
     *         initialized.
     * @retval StatusCode::Unsupported Channel count above 32.
     * @retval StatusCode::Device miniaudio device initialization failed.
     *
     * @par Thread safety
     * Thread-safe during setup; callers should not reconfigure a running device.
     * miniaudio Integration:
     * - ma_device_config_init(ma_device_type_playback)
     * - Format: ma_format_f32 (float32)
     */
    ExpectedVoid init(const Config& cfg);

  private:
    /// Opaque audio backend (miniaudio device) and mutable state, defined in output.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::player
