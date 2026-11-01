#pragma once

/**
 * @file output.hpp
 * @brief Audio output management using miniaudio
 * @ingroup caudio_player
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

#include <caudio/fwd.hpp>
#include <caudio/types/record.hpp>
#include <cstdint>
#include <expected>
#include <memory>

namespace caudio::player {

/**
 * @brief Enumerate all available audio playback devices
 * @return DeviceList containing all playback devices
 *
 * Creates a miniaudio context, queries the system for playback devices,
 * and returns a list of DeviceInfo structures. The miniaudio context
 * is created and destroyed within this function call.
 *
 * Device IDs are assigned as sequential indices (0, 1, 2...) for
 * user-friendly CLI selection. The default device is marked with
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
CAUDIO_API caudio::types::DeviceList enumerateDevices();

/**
 * @class AudioOutput
 * @brief Lock-free audio output device with ring buffer integration
 * @ingroup caudio_player
 *
 * Manages a miniaudio playback device that consumes float32 samples from
 * a lock-free SPSC ring buffer. Designed for real-time audio playback
 * with minimal latency and no allocations in the audio callback.
 *
 * @par Thread safety
 * - create()/init()/shutdown(): Thread-safe, called from control thread
 * - start()/stop()/setVolume()/volume(): Thread-safe (atomic)
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
class CAUDIO_API AudioOutput {
  public:
    using Expected = std::expected<std::unique_ptr<AudioOutput>, caudio::utils::Error>;

    /**
     * @struct Config
     * @brief Audio output configuration parameters
     * @ingroup caudio_player
     */
    struct Config {
        /** @brief Output sample rate in hertz. */
        uint32_t sampleRate = 48000;
        /** @brief Number of output channels. */
        uint32_t channels = 2;
        /** @brief SPSC ring buffer consumed by the audio callback. */
        caudio::utils::SpscRing<float>* ring = nullptr;
        /** @brief Initial volume, clamped to the range [0, 1]. */
        float volume = 1.0f;
        /**
         * @brief Non-owning logger; null disables logging.
         * @par Ownership
         * The logger must outlive the AudioOutput.
         */
        caudio::utils::Logger* logger{nullptr};
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
     * @retval StatusCode::InvalidArg Invalid config (0 channels, 0 sample rate, null ring)
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
     *
     * Starts the miniaudio device if initialized. Uses compare_exchange
     * to ensure only one start transition occurs.
     *
     * @par Thread safety
     * Thread-safe; uses an atomic running-state transition.
     * @par Blocking
     * May block while the backend starts the audio device.
     */
    void start();

    /**
     * @brief Stop audio playback
     *
     * Stops the miniaudio device if running. Uses compare_exchange
     * to ensure only one stop transition occurs.
     *
     * @par Thread safety
     * Thread-safe; uses an atomic running-state transition.
     */
    void stop();

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
     * @return true on success, false on failure
     *
     * Configures and initializes the miniaudio device with float32 format.
     * This is normally called by create(); call it directly only on a
     * default-constructed or shut-down object.
     * Sets up the data callback for lock-free ring buffer consumption.
     * Device starts in stopped state; caller must call start() after preroll.
     *
     * @pre cfg.channels > 0 && cfg.channels <= 32 && cfg.sampleRate > 0 && cfg.ring != nullptr
     *
     * @retval true Device initialized successfully
     * @retval false Initialization failed (invalid config or miniaudio error)
     *
     * @par Thread safety
     * Thread-safe during setup; callers should not reconfigure a running device.
     * miniaudio Integration:
     * - ma_device_config_init(ma_device_type_playback)
     * - Format: ma_format_f32 (float32)
     */
    bool init(const Config& cfg);

  private:
    /// Opaque audio backend (miniaudio device) and mutable state, defined in output.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace caudio::player
