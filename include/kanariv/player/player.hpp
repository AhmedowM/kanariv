#pragma once

/**
 * @file player.hpp
 * @brief Core audio player implementation with gapless playback support
 * @ingroup kanariv_player
 *
 * This header provides the Player class, the central component for audio playback.
 * It coordinates decoding, audio output, and playback state management.
 *
 * Architecture:
 * - Decode thread: Runs Decoder::decode() to fill SPSC ring buffer
 * - Audio thread (miniaudio): Consumes from ring buffer via AudioOutput::dataCallback
 * - Control thread: Application calls play/pause/stop/seek/volume
 *
 * @par Thread safety
 * - Public methods: thread-safe. Two mutexes guard the playback graph:
 *   `controlMutex_` serializes control operations for their whole duration and
 *   `graphMutex_` guards the decoder, ring, and audio output. Lock order is
 *   `controlMutex_` -> `graphMutex_`; the decode thread takes only
 *   `graphMutex_`, and it is held for at most one decode chunk (a reader that
 *   blocks can delay control calls, never the audio callback).
 *   AudioOutput::stop() joins the miniaudio callback before the ring is reset
 *   or the graph replaced, so the audio callback never takes a Player mutex.
 * - decodeLoop(): Runs on dedicated jthread, single-threaded
 * - AudioOutput::dataCallback(): Runs on miniaudio audio thread (real-time)
 * - `state()` is the only observation point for asynchronous transitions
 *   (end of stream, decode failure); it is published after the subordinate
 *   flags, so a reader never sees `Playing` with stale bookkeeping.
 *
 * Playback State Machine:
 *   Stopped <-> Playing <-> Paused
 *     ^         |          |
 *     |         v          v
 *     +-----> Error <------+
 *
 * Gapless Playback:
 * - Ring buffer prerolls with cap/2 samples before play() returns
 * - Seeking resets ring buffer and updates position atomically
 * - Decode thread maintains steady fill rate; audio thread consumes
 * - No allocations in hot path (decode loop or audio callback)
 *
 * FFmpeg Integration:
 * - Decoder::open probes format and creates the decoder
 * - Decoder uses custom AVIO callbacks for Reader abstraction
 * - SwrContext resamples to float32 interleaved for AudioOutput
 * - Seeking uses stream time_base for accurate container positioning
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace kanariv::player {

/**
 * @enum State
 * @brief Playback state enumeration
 * @ingroup kanariv_player
 *
 * Represents the current state of the Player. Transitions:
 * - Stopped <-> Playing (play/stop)
 * - Playing <-> Paused (pause/resume)
 * - Any -> Error (on decoder/output failure)
 */
enum class State : uint8_t { Stopped = 0, Playing = 1, Paused = 2, Error = 3 };

/**
 * @struct PlayerOptions
 * @brief Player configuration options
 * @ingroup kanariv_player
 */
struct PlayerOptions {
    /** @brief Output sample rate in hertz. */
    uint32_t sampleRate = 48000;
    /** @brief Number of interleaved output channels. */
    uint32_t channels = 2;
    /**
     * @brief Non-owning logger used by the decoder and output.
     * @par Ownership
     * May be null to disable logging. The logger must outlive the Player and
     * is never called from the destructor.
     */
    kanariv::utils::Logger* logger{nullptr};
    /**
     * @brief Use miniaudio's silent null backend instead of a real device.
     *
     * The null backend runs the data callback at the configured rate without
     * touching hardware, so playback can be exercised headless (tests, CI).
     * Only float32 interleaved output is supported.
     */
    bool nullOutput{false};
};

/**
 * @class Player
 * @brief Core audio player with gapless playback and multi-format support
 * @ingroup kanariv_player
 *
 * High-level playback controller coordinating:
 * - Reader abstraction (FileReader, MemoryReader)
 * - Decoder registry (FFmpeg for all common formats)
 * - Lock-free SPSC ring buffer for decode/audio thread communication
 * - miniaudio-based AudioOutput for device playback
 *
 * Thread Model:
 * - Control thread: Application calls open/play/pause/stop/seek/volume
 * - Decode thread: Dedicated jthread running decodeLoop()
 * - Audio thread: miniaudio callback (real-time priority)
 *
 * State Transitions (atomic, lock-free where possible):
 *   create() -> init() -> open() -> play() -> pause()/resume() -> stop()
 *                                    \-> seek() (any state)
 *
 * Gapless Implementation:
 * - Preroll fills ring buffer to cap/2 before play() returns
 * - Seek resets ring, updates posBase atomically, restarts decode
 * - Position tracking uses posBase + elapsed time (monotonic clock)
 *
 * Error Handling:
 * - Errors are kept for lastError() and the state is set to Error
 * - All public methods return Expected<void> for explicit error handling
 * - Clean end of stream sets Stopped (and play() rewinds); a decoder or
 *   device failure sets Error and is never reported as a normal end
 */
class KANARIV_API Player {
  public:
    /** @brief Expected result used by operations that return no value. */
    using ExpectedVoid = std::expected<void, kanariv::utils::Error>;
    /** @brief Expected result returned by the factory. */
    using ExpectedPlayer = std::expected<std::unique_ptr<Player>, kanariv::utils::Error>;

    /**
     * @brief Create a new Player instance
     * @param opts Configuration options (sample rate, channels)
     * @return Expected containing unique_ptr<Player> on success, Error on failure
     *
     * Factory method that constructs a Player and initializes the decode thread.
     * The player starts in Stopped state; call open() before play().
     *
     * @retval StatusCode::Device Decode thread creation failed
     *
     * @par Thread safety
     * Thread-safe; can be called from any thread.
     */
    static ExpectedPlayer create(const PlayerOptions& opts = {});

    /**
     * @brief Destructor - stops playback and joins decode thread
     *
     * Stops any active playback, signals the decode thread to stop,
     * and waits for it to join. Safe to call multiple times.
     *
     * @par Thread safety
     * Thread-safe; stops playback and joins the decode thread.
     */
    ~Player();

    /**
     * @brief Open an audio file for playback
     * @param path Path to the audio file
     * @return void on success, Error on failure
     *
     * Opens the file using FileReader, probes the format via Decoder::open,
     * creates the decoder, ring buffer, and audio output. Performs preroll
     * to fill the ring buffer to cap/2 before returning.
     *
     * Supported formats (via FFmpeg): OGG/Vorbis, FLAC, MP3, WAV, M4A/AAC, Opus, WMA
     *
     * @retval StatusCode::InvalidArg Empty path
     * @retval StatusCode::NotFound File not found or cannot be opened
     * @retval StatusCode::Unsupported No decoder recognized the format
     * @retval StatusCode::Io FFmpeg initialization failed
     * @retval StatusCode::Device Audio output device creation failed
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex, which also spans the graph
     * replacement so no other control call sees a half-built graph.
     */
    ExpectedVoid open(std::string_view path);

    /**
     * @brief Open a reader for playback (advanced usage)
     * @param reader Unique pointer to a Reader implementation
     * @return void on success, Error on failure
     *
     * Opens playback from a custom Reader (FileReader, MemoryReader, or custom).
     * Used for non-file sources like network streams or embedded audio data.
     * Stops any existing playback before opening the new reader.
     *
     * @retval StatusCode::InvalidArg Null reader
     * @retval StatusCode::Unsupported No decoder recognized the format
     * @retval StatusCode::Io FFmpeg initialization failed
     * @retval StatusCode::Device Audio output device creation failed
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex. The old graph is only
     * discarded after the new decoder, ring, and output are fully built.
     * @par Ownership
     * The player takes ownership of `reader` after successful or failed
     * opening and releases it when the player is destroyed or reopened.
     */
    ExpectedVoid openReader(std::unique_ptr<media::Reader> reader);

    /**
     * @brief Start or resume playback
     * @return void on success, Error on failure
     *
     * Transitions state: Stopped -> Playing or Paused -> Playing.
     * Starts the audio device and signals the decode thread to begin filling
     * the ring buffer. Preroll was already performed during open().
     * If playback previously ended at end of stream, it rewinds to the start.
     *
     * @retval StatusCode::State Not opened, already playing, or in Error
     * @retval StatusCode::Device Audio device failed to start; state becomes Error
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex.
     */
    ExpectedVoid play();

    /**
     * @brief Pause playback
     * @return void on success, Error on failure
     *
     * Transitions state: Playing -> Paused.
     * Stops the audio device and updates position tracking with elapsed time.
     * Decode thread will stop filling the ring buffer.
     *
     * @retval StatusCode::State Not currently playing
     * @retval StatusCode::Device Audio device failed to stop; state becomes Error
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex.
     */
    ExpectedVoid pause();

    /**
     * @brief Resume playback from paused state
     * @return void on success, Error on failure
     *
     * Transitions state: Paused -> Playing.
     * Restarts the audio device and signals the decode thread to continue.
     * Position tracking resumes from the paused position.
     *
     * @retval StatusCode::State Not currently paused
     * @retval StatusCode::Device Audio device failed to start; state becomes Error
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex.
     */
    ExpectedVoid resume();

    /**
     * @brief Stop playback and reset to initial state
     * @return void on success, Error on failure
     *
     * Transitions state: Any -> Stopped.
     * Stops audio device, resets ring buffer, clears position tracking,
     * and closes the open gate to stop the decode thread.
     * Does not close the decoder/reader; call open() again to play.
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex.
     */
    ExpectedVoid stop();

    /**
     * @brief Seek to a specific time position
     * @param seconds Target time in seconds from stream start (must be >= 0, finite)
     * @return void on success, Error on failure
     *
     * Seeks the decoder to the specified position, resets the ring buffer,
     * and updates position tracking atomically. If playing, restarts the
     * decode thread from the new position. If paused, updates position only.
     *
     * @retval StatusCode::State Not opened
     * @retval StatusCode::InvalidArg seconds is negative, NaN, or infinity
     * @retval StatusCode::Io Decoder seek failed
     * @retval StatusCode::Internal Decoder not initialized
     * @retval StatusCode::Device Audio device failed to stop or restart;
     *         state becomes Error
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex. The device is stopped
     * (joining the audio callback) before the decoder is repositioned and the
     * ring buffer is reset.
     * @par Blocking
     * May wait for an in-flight decode chunk and device state changes.
     */
    ExpectedVoid seek(double seconds);

    /**
     * @brief Set playback volume
     * @param volume Volume level (0.0 = mute, 1.0 = full)
     * @return void on success, Error on failure
     *
     * Volume is clamped to [0.0, 1.0] and applied atomically to both
     * the internal volume state and the audio output device.
     *
     * @retval StatusCode::InvalidArg volume is NaN or infinity
     *
     * @par Thread safety
     * Thread-safe; serialized by the control mutex.
     */
    ExpectedVoid setVolume(float volume);

    /**
     * @brief Get current playback state
     * @return Current State enum value
     *
     * @par Thread safety
     * Thread-safe; loads the state atomically.
     */
    State state() const noexcept;

    /**
     * @brief Get current playback position
     * @return Duration from stream start (0 if stopped/error)
     *
     * Calculates position from posBase (frames at last seek/start) plus
     * elapsed time since playback started. Uses steady_clock for monotonicity.
     *
     * @par Thread safety
     * Thread-safe; reads position state atomically.
     */
    std::chrono::duration<double> position() const noexcept;

    /**
     * @brief Get the reason recorded for the most recent failure
     * @return Last error message; empty when no failure was recorded
     *
     * Asynchronous failures (decode thread, audio device) surface only as
     * `State::Error` at the call site; this accessor carries their reason.
     * A later successful open()/play() replaces it.
     *
     * @par Thread safety
     * Thread-safe; takes the control mutex, which may briefly wait for an
     * in-flight decode chunk.
     */
    std::string lastError() const;

  private:
    /** @brief Private default ctor; use create() factory. */
    Player();

    /// Opaque implementation holding the playback components and state.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::player
