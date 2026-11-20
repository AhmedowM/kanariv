#pragma once

/**
 * @file pipeline.hpp
 * @brief Single-track playback pipeline shared by Player and Engine.
 * @details Both playback paths own the same graph: a device sink draining an
 * SPSC ring, a producer thread that decodes into it, a wall-clock position and
 * the transport state machine around open/start/stop/seek. Player and Engine
 * each had a copy, and the copies drifted (preroll depth, clock math, error
 * handling). This header holds the graph once; the owner supplies a Source and
 * the pipeline owns everything else.
 *
 * @par Lock protocol
 * - `controlMutex_` serializes control operations end to end, so preroll(),
 *   play() and seek() never interleave on one graph.
 * - `mtx_` guards the graph (ring, sink, source, clock, transport state). It is
 *   held for bookkeeping only -- never across a Source call or a ring wait.
 *   Device start/stop may run under it: the audio callback takes no pipeline
 *   mutex, so joining it cannot wait on `mtx_`. Owners may therefore call
 *   control operations while holding their own locks as long as the Source
 *   does not need them.
 * - The producer thread takes `mtx_` in the same short sections and never
 *   `controlMutex_`; the audio callback takes neither (AudioOutput::stop()
 *   joins it before the ring is reset or the graph replaced).
 *
 * @par Generation tokens
 * Every transport mutation bumps `gen_` before it touches the decoder or the
 * ring. A producer samples the generation when it starts a chunk and
 * re-validates it under its own decoder lock and again before the ring write,
 * so a chunk that straddles a seek/open/stop is discarded instead of being
 * played over the new position or consuming the new decoder's first frames.
 *
 * @par Terminal transitions
 * The producer thread commits a clean end of stream (after the ring drains) and
 * a decode failure itself, under `mtx_`, so a concurrent control operation
 * cannot race with it: whichever runs first wins, exactly as when the state
 * machine and the decode loop shared one mutex.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kanariv::player::detail {

/**
 * @brief Producer epoch handed to Source::produce().
 * @details The source calls current() under its own decoder lock before it
 * decodes anything; when the epoch moved, the chunk's output would land in a
 * replaced graph and must not consume decoder frames.
 */
struct SourceToken {
    const std::atomic<std::uint64_t>* generation{nullptr};
    std::uint64_t value{0};

    [[nodiscard]] bool current() const noexcept {
        return generation != nullptr &&
               generation->load(std::memory_order_acquire) == value;
    }
};

/**
 * @brief Audio producer feeding a Pipeline's ring.
 * @details Called from the pipeline's producer thread (and from preroll() on
 * the caller's thread), one chunk at a time. The pipeline never holds its own
 * mutex across the call, so the source owns all locking around the decoder.
 * Implementations may be shared with parked producer threads; they must stay
 * alive until the last in-flight produce() returns (the pipeline keeps a
 * shared_ptr).
 */
class Source {
  public:
    /// How one produce call ended.
    enum class Status : std::uint8_t {
        Continue,    ///< More audio may follow (possibly after a stall).
        EndOfStream, ///< No new frames and none will come: the track is over.
        Failed       ///< Decoder/IO failure; never a clean end of stream.
    };

    /// One chunk: produced frames plus how the call ended.
    struct Result {
        /// Interleaved samples in the span handed to produce(); empty on
        /// stall, end of stream or failure.
        std::span<const float> samples{};
        Status status{Status::Continue};
        /// Failure reason for lastError(); empty means a generic message.
        std::string_view error{};
    };

    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    virtual ~Source() = default;

    /**
     * @brief Fill @p out with interleaved samples for the current epoch.
     * @param out Scratch span sized by the shared chunk policy; overwrite it.
     * @param token Epoch snapshot; return an empty result when it is stale.
     * @return Produced frames and the end condition.
     * @par Thread safety
     * Serialized with seek() by the implementation; called from at most one
     * thread at a time for a given epoch, but a stale call may overlap the
     * control thread.
     */
    virtual Result produce(std::span<float> out, SourceToken token) = 0;

    /**
     * @brief Reposition the producer.
     * @return Error when the underlying decoder cannot seek.
     * @par Thread safety
     * Not overlapping produce(); the pipeline never calls it from the producer
     * thread itself.
     */
    virtual kanariv::utils::Expected<void> seek(double seconds) = 0;
};

/**
 * @brief Ring + device sink + producer thread + transport state for one track.
 * @details One instance lives as long as its owner (Player or Engine). open()
 * replaces the graph while the old one is quiesced, preroll() fills the ring
 * before the device starts, and play()/pause()/resume()/stop()/seek() drive
 * the state machine:
 *
 *     Stopped <-> Playing <-> Paused
 *       any ------------> Error   (decode/device failure; open() clears it)
 *
 * The owner keeps its own richer state machine when it has one (the engine's
 * queue); this one describes the audio device and the clock.
 */
class Pipeline {
  public:
    using ExpectedVoid = kanariv::utils::Expected<void>;

    /**
     * @struct Config
     * @brief Sink and graph configuration fixed for the pipeline's lifetime.
     */
    struct Config {
        /// Ring capacity in frames; 0 means clock-only (no ring, no device).
        std::size_t ringFrames{0};
        /// Initial user volume, clamped to [0, 1].
        float volume{1.0f};
        /// Initial per-track gain (ReplayGain); 1.0 is neutral.
        float gain{1.0f};
        /// Non-owning logger; must outlive the pipeline.
        kanariv::utils::Logger* logger{nullptr};
        /// Use miniaudio's silent null backend (headless tests, simulations).
        bool nullBackend{false};
        /// Configured playback device (opaque id, name, or legacy index).
        std::string deviceId{};
        /// Display name paired with deviceId.
        std::string deviceName{};
    };

    explicit Pipeline(Config cfg);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /**
     * @brief Install a new graph: ring, sink and source.
     * @param sampleRate Output rate in hertz; 0 falls back to 48000.
     * @param channels Output channels; 0 falls back to 2.
     * @param source Producer feeding the ring; may be null for clock-only.
     * @return Device creation error; on failure the pipeline stays closed.
     * @details Stops and releases whatever was installed first. The old graph
     * is only torn down here, so the ring and source seen by a parked producer
     * stay alive (shared ownership) until its chunk returns.
     */
    ExpectedVoid open(std::uint32_t sampleRate, std::uint32_t channels,
                      std::shared_ptr<Source> source);

    /**
     * @brief Release the graph and return to Stopped.
     * @details Stops the device, drops the ring/source, and rewinds the clock.
     * The producer thread keeps running (parked) so the pipeline can be
     * reopened; the destructor stops and joins it.
     */
    void close();

    /**
     * @brief Fill the ring before the device starts.
     * @return Io error when the source fails while prerolling.
     * @details Fills up to prerollFrames(capacity) using the shared chunk
     * policy, stopping early on end of stream (short files are buffered whole)
     * or a stall. Requires a stopped producer, which the internal control
     * mutex guarantees.
     */
    ExpectedVoid preroll();

    /**
     * @brief Start producing and start the device.
     * @retval StatusCode::State Not opened, already playing, or in Error.
     * @retval StatusCode::Device Device start failed; state becomes Error.
     * @details A Stopped pipeline that still has a source rewinds it to 0
     * first (end of stream or stop() must not replay from the middle); a seek
     * performed while stopped keeps its position.
     */
    ExpectedVoid play();

    /** @brief Playing -> Paused; folds elapsed time into the clock. */
    ExpectedVoid pause();

    /** @brief Paused -> Playing; resumes the device and the clock. */
    ExpectedVoid resume();

    /**
     * @brief Any -> Stopped; stops the device, clears the ring and the clock.
     * @details Keeps the source (a later play() rewinds it), so stop() is
     * restartable without reopening.
     */
    ExpectedVoid stop();

    /**
     * @brief Reposition the source, clear the ring and rebase the clock.
     * @param seconds Target position; the caller validates it.
     * @details Stops the device when playing, seeks the source, resets the
     * ring and restarts the device, publishing Playing last. A stopped or
     * paused graph is left in its state with the new clock value.
     */
    ExpectedVoid seek(double seconds);

    /**
     * @brief Move the clock without touching the source or the ring.
     * @param seconds New base position; a playing clock restarts from now.
     */
    void rebase(double seconds);

    /** @brief Set the user volume (clamped to [0, 1]) on the current sink. */
    void setVolume(float volume);

    /** @brief Set the per-track gain on the current sink (NaN/negative -> 1). */
    void setGain(float gain);

    /// @brief Current transport state (atomic read).
    [[nodiscard]] kanariv::types::PlaybackState state() const noexcept;

    /**
     * @brief Force the Error state with a reason (owner-side open failures).
     * @details Stops the device and bumps the generation, so nothing keeps
     * producing over a graph the owner could not build.
     */
    void fail(std::string_view reason);

    /// @brief Reason recorded for the most recent failure; empty when none.
    [[nodiscard]] std::string lastError() const;

    /// @brief Base position plus elapsed time while playing; 0 when stopped.
    [[nodiscard]] std::chrono::duration<double> position() const noexcept;

  private:
    using Ring = kanariv::utils::SpscRing<float>;

    void producerLoop(std::stop_token st);
    void failLocked(std::string_view reason);
    void finishEndOfStream(const SourceToken& token, const Ring& ring);
    void finishFailed(const SourceToken& token, const Ring& ring, std::string_view why);

    Config cfg_;
    /// Serializes control operations; see @par Lock protocol.
    mutable std::mutex controlMutex_;
    /// Guards the graph and the clock; see @par Lock protocol.
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::jthread producer_;

    std::shared_ptr<Ring> ring_;
    std::shared_ptr<Source> source_;
    std::unique_ptr<AudioOutput> output_;
    bool opened_{false};
    /// play() must rewind: set by stop() and by a clean end of stream.
    bool needsRewind_{false};
    std::uint32_t sampleRate_{48000};
    std::uint32_t channels_{2};
    double baseSec_{0.0};
    int64_t startMs_{0};
    float volume_{1.0f};
    float gain_{1.0f};
    std::atomic<std::uint64_t> gen_{0};
    std::atomic<kanariv::types::PlaybackState> state_{
        kanariv::types::PlaybackState::Stopped};
    std::string lastErr_;
};

} // namespace kanariv::player::detail
