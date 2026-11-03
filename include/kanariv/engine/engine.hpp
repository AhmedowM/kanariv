#pragma once

// Lifecycle
/**
 * @file engine.hpp
 * @brief Playback engine -- state machine, gapless, decode/monitor loops and persistence.
 * @ingroup kanariv_engine
 * @details Core `Engine` class (`kanariv.engine` re-exports it via `engine.hpp`).
 * History/shuffle internals live in src/engine/ (NOT installed).
 * Core class is Engine which owns:
 * - Playback state machine (`Stopped -> Playing -> Paused -> Playing`,
 *   next/prev with shuffle/repeat, see queueNextLocked/queuePrevLocked).
 * - Gapless transition: preroll() fills SpscRing<float> to half capacity
 *   before AudioOutput::start(); decodeLoop continuously refills the ring;
 *   engineTick arms gaplessArmed_ (CAS 0->1) when remaining <= gaplessMs
 *   (300 ms) and calls next().
 * - Threads: decodeLoop runs on decodeThread_ (SPSC ring, decodeMtx_ +
 *   decodeCv_, wakes on Playing); monitorLoop runs on monitorThread_
 *   polling every pollMs (default 10 ms) via monMtx_/monCv_ and calling
 *   engineTick() (history + progress + gapless).
 * - Persistence: engine_state row id=1 stores shuffle_enabled,
 *   repeat_mode, cursor_pos, current_track_id, volume, shuffle_perm BLOB
 *   and active_queue_id. loadState/saveState handle the active_queue_id
 *   migration (try sqlNew, fallback to sqlOld). persistShuffleBlobLocked
 *   and persistCursorLocked update the blob/cursor transactionally.
 * - History: doHistoryMark uses shouldMarkPlayed(historyThresholdPct/Secs)
 *   with atomic CAS on markedPlayed_ and a database-owned transaction.
 * Locking: queueMutex_ (timed mutex) for the queue state -- try_lock on fast
 * and monitor paths, bounded wait for transport commands (play/next/prev);
 * decodeMtx_ (mutex) for decoder_/ring_ vs seek/decodeLoop; persistence and
 * history operations go through the backend-neutral contracts::IDatabase.
 * Events: a bounded event queue (64 slots) with drop-on-full, dispatched via
 * pushEvent to callbacks under cbMutex_.
 * Implementation details (atomics, threads, mpsc queue, ring, DB rows) live
 * in src/engine/engine.cpp behind Engine::Impl (pimpl) so this header stays
 * cheap; the public API is unchanged.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::engine {

namespace detail {
/**
 * @brief Service-only access to `Engine`'s private members.
 * @ingroup kanariv_engine
 * @details Declared here so `Engine` can name it as its single friend; the
 * definition lives in the internal header `src/engine/engine_access.hpp` and
 * is never installed.
 */
struct ServiceAccess;
} // namespace detail

/**
 * @brief Main playback engine -- queue, decoding, gapless, history and persistence.
 * @ingroup kanariv_engine
 * @details See file-level docs for state machine, threading and persistence.
 * Public mutators that touch the queue state use `tryLockQueue()` (non-blocking,
 * `Busy` on contention); transport commands (play/next/prev) instead wait up
 * to 500 ms via `tryLockQueueFor()` so a colliding skip executes instead of
 * failing. Decode/ring access is serialized by `decodeMtx_`. State (`playbackState_`,
 * `hasCurrent_`, `volume_`, `markedPlayed_`, `gaplessArmed_`,
 * `lastProgressMs_`) is atomic.
 *
 * Error codes: `InvalidArg` (null/empty args), `State` (no db / not
 * playing/paused), `NotFound` (empty queue / no event), `Busy` (queue
 * locked / transaction begin failed), `Internal` (SQLite / decoder),
 * `NoMem` (perm too large).
 *
 * Thread safety: thread-safe for concurrent play/pause/next/prev/seek
 * under the documented locks; `queueMutex_` is try_lock on fast paths and
 * bounded-wait on transport commands, so callers must still handle Busy
 * (budget exceeded).
 * @see kanariv::types::PlaybackState
 * @see kanariv::types::EngineConfig
 */
class KANARIV_API Engine final {
  public:
    using ExpectedVoid = std::expected<void, kanariv::utils::Error>; ///< Void or Error.
    using ExpectedEngine =
        std::expected<std::unique_ptr<Engine>, kanariv::utils::Error>; ///< Engine ptr or Error.

    /**
     * @brief Creates an Engine with no database attached.
     * @ingroup kanariv_engine
     * @param cfg Engine configuration (poll/gapless/history thresholds).
     * @return Owning Engine or Error from init().
     * @details Starts decodeThread_ and optionally monitorThread_ via init().
     * No database is opened; optionally attach a store with attachDatabase().
     * @par Thread safety
     * Thread-safe; constructs internal atomics/threads.
     * @see attachDatabase
     */
    static ExpectedEngine create(const kanariv::types::EngineConfig& cfg = {});

    /**
     * @brief Attaches an already-open database implementation.
     * @ingroup kanariv_engine
     * @param db Shared database contract (must be non-null and open).
     * @return Success or Error InvalidArg / loadState failure.
     * @retval StatusCode::InvalidArg if db is null or not open.
     * @par Ownership
     * The engine retains the shared pointer and keeps the database attached
     * until shutdown or replacement; callers may release their own copy after
     * this call returns.
     * @par Thread safety
     * Thread-safe; calls the database implementation to load state.
     * @see loadState
     */
    ExpectedVoid attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db);

    /** @brief Tears down threads and persists state. @ingroup kanariv_engine */
    ~Engine();

    /**
     * @brief Shuts down monitor/decode threads and persists state.
     * @ingroup kanariv_engine
     * @details Stops monRun_/decodeRun_, notifies cvs, joins jthreads
     * via request_stop(), persists cursorPos from queue_.cursor via
     * saveState(), then stops AudioOutput and resets decoder/reader/ring.
     * Idempotent.
     * @par Thread safety
     * Thread-safe; joins threads and locks where needed.
     */
    void shutdown();

    /**
     * @brief Starts or resumes playback.
     * @ingroup kanariv_engine
     * @param queueId Active queue id (0 defaults to 1).
     * @return Success or Error State/Busy/NotFound/Internal.
     * @retval StatusCode::State if no db.
     * @retval StatusCode::Busy if the queue stays contended past the wait budget.
     * @retval StatusCode::NotFound if queue empty, or track file not
     * playable without allowSimulatedPlayback.
     * @retval StatusCode::Device if the audio device cannot be created
     * without allowSimulatedPlayback.
     * @details State machine:
     * - Paused -> Playing: resume (no dequeue), restart monotonic clock,
     *   start AudioOutput.
     * - Playing -> Playing: restart current track (seek 0 under decodeMtx_,
     *   ring reset).
     * - Stopped -> Playing: queueNextLocked under queueMutex_ (bounded
     *   500 ms wait), then doPlayTrack.
     * @par Blocking
     * Gapless playback prerolls the next track before returning and pushes a
     * `TrackStarted` event when playback begins.
     * @par Thread safety
     * Thread-safe; Stopped path waits boundedly on queueMutex_, decoder
     * under decodeMtx_.
     * @see pause
     * @see resume
     * @see stop
     */
    // Playback transport
    ExpectedVoid play(int64_t queueId = 1);

    /**
     * @brief Plays a specific track already present in the active queue.
     * @ingroup kanariv_engine
     * @param trackId Track id to select (must be in the active queue).
     * @return Success or Error NotFound/InvalidArg/Busy/Device.
     * @details Repositions the queue cursor to the track (respecting an
     * active shuffle permutation) and starts it from the head. Used by
     * bookmark goto; does not modify the queue itself.
     * @par Thread safety
     * Thread-safe; waits boundedly on queueMutex_.
     */
    ExpectedVoid playTrack(int64_t trackId);

    /**
     * @brief Pauses playback.
     * @ingroup kanariv_engine
     * @return Success or Error State if not playing.
     * @par Thread safety
     * Thread-safe; atomics only.
     */
    ExpectedVoid pause();

    /**
     * @brief Resumes from paused.
     * @ingroup kanariv_engine
     * @return Success or Error State if not paused.
     * @par Thread safety
     * Thread-safe; notifies decode/monitor cvs.
     */
    ExpectedVoid resume();

    /**
     * @brief Stops playback and resets position/ring.
     * @ingroup kanariv_engine
     * @return Success (always).
     * @par Thread safety
     * Thread-safe.
     * @see play
     */
    ExpectedVoid stop();

    /**
     * @brief Seeks to a position in the current track.
     * @ingroup kanariv_engine
     * @param seconds Target position in seconds [0, duration].
     * @return Success or Error State/InvalidArg/Internal.
     * @retval StatusCode::State if no current track.
     * @retval StatusCode::InvalidArg if seconds is NaN/inf/negative.
     * @retval StatusCode::Internal if decoder seek fails (state restored).
     * @details Holds decodeMtx_ to serialize with decodeLoop (decoder_->decode
     * / ring_->write). Temporarily Pauses, snapshots pausePos_/playStart_ so
     * a failed seek restores the prior state and resumes Playing.
     * @par Thread safety
     * Thread-safe; locks decodeMtx_.
     */
    ExpectedVoid seek(double seconds);

    /**
     * @brief Sets playback volume and persists it.
     * @ingroup kanariv_engine
     * @param g Volume in [0,1] (clamped; NaN/inf -> 0).
     * @return Success (always).
     * @details Updates state_.volume, volume_ atomically and forwards to
     * AudioOutput::setVolume; persists via saveState() if DB attached.
     * @par Thread safety
     * Thread-safe; atomic volume + saveState transaction.
     */
    ExpectedVoid setVolume(float g);

    // Playback state
    /** @brief Returns current volume [0,1]. @ingroup kanariv_engine
     * @return Volume level (atomic load, relaxed). */
    float volume() const noexcept;
    /** @brief Returns current playback state. @ingroup kanariv_engine
     * @return PlaybackState (atomic load, acquire). */
    kanariv::types::PlaybackState state() const noexcept;
    /** @brief Returns current track duration in seconds. @ingroup kanariv_engine
     * @return Duration (cached from decoder or metadata). */
    double duration() const noexcept;
    /** @brief Returns current playback position in seconds. @ingroup kanariv_engine
     * @return Position (persisted resume position if stopped with no loaded track;
     * pausePos_ if paused; pausePos_ + elapsed if playing, clamped). */
    double position() const noexcept;
    /** @brief Returns current track id (or persisted one if no current). @ingroup kanariv_engine
     * @return Track id if hasCurrent_, else state_.currentTrackId (persisted). */
    int64_t currentTrackId() const noexcept;

    /** @brief Returns whether shuffle is enabled. @ingroup kanariv_engine
     * @return True if shuffle mode active. */
    bool shuffle() const noexcept;

    /** @brief Returns current repeat mode. @ingroup kanariv_engine
     * @return RepeatMode (Off/All/One). */
    kanariv::types::RepeatMode repeat() const noexcept;

    // Database-backed history and statistics
    /**
     * @brief Returns database statistics.
     * @ingroup kanariv_engine
     * @return `std::expected<types::PlaybackStats, Error>` -- stats on success, Error if no db.
     * @par Thread safety
     * Thread-safe; delegates to Database::getStats() which takes shared_lock.
     * @see kanariv::db::Database::getStats
     * @see kanariv::types::PlaybackStats
     */
    std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> stats();

    /**
     * @brief Lists history entries.
     * @ingroup kanariv_engine
     * @param limit Max rows (0 = no limit, default 50).
     * @return `std::expected<std::vector<types::HistoryEntry>, Error>` -- vector on success, State
     * if no db. Entries carry track snapshots (title/artist/path/duration).
     * @par Thread safety
     * Thread-safe; takes shared_lock on the database mutex.
     * @see kanariv::types::HistoryEntry
     */
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(int limit = 50);

    /**
     * @brief Clears all history entries.
     * @ingroup kanariv_engine
     * @return `std::expected<void, Error>` -- success or State/Internal.
     * @par Thread safety
     * Thread-safe; takes unique_lock on the database mutex.
     */
    std::expected<void, kanariv::utils::Error> clearHistory();

    // Queue selection and navigation
    /**
     * @brief Enables or disables shuffle.
     * @ingroup kanariv_engine
     * @param on True to enable, false to disable.
     * @return Success or Error State/Busy/Internal.
     * @retval StatusCode::State if no db.
     * @retval StatusCode::Busy if queueMutex_ try_lock fails.
     * @details Delegates to setShuffleLocked under queueMutex_, then
     * pushes QueueChanged event.
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_.
     * @see setShuffleLocked
     */
    ExpectedVoid setShuffle(bool on);

    /**
     * @brief Sets repeat mode and persists it.
     * @ingroup kanariv_engine
     * @param m RepeatMode (Off/All/One).
     * @return Success or Error InvalidArg/Busy/Internal.
     * @retval StatusCode::InvalidArg if m is out of range.
     * @details Under queueMutex_: updates queue_.repeat/state_.repeatMode
     * and persists via saveState() (handles active_queue_id migration).
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_.
     */
    ExpectedVoid setRepeat(kanariv::types::RepeatMode m);

    /**
     * @brief Returns the active queue id.
     * @ingroup kanariv_engine
     * @return queue_.queue_id if set, else state_.activeQueueId, else 1 (atomic under queueMutex_).
     * @par Thread safety
     * Thread-safe; locks queueMutex_.
     * @see switchQueue
     */
    int64_t activeQueueId() const noexcept;

    /**
     * @brief Switches the active queue.
     * @ingroup kanariv_engine
     * @param qid Queue id to switch to (0 defaults to 1).
     * @return Success or Error State/Busy/NotFound.
     * @retval StatusCode::State if no db.
     * @retval StatusCode::Busy if queueMutex_ try_lock fails.
     * @details Validates via db_->getQueue(qid) while holding
     * queueMutex_ (then dbMutex_ shared). On switch: resets cursor to 0,
     * clears perm (shuffle flag kept but perm regenerated on next shuffle),
     * updates state_.activeQueueId/cursorPos and persists via saveState().
     * Pushes QueueChanged event.
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_.
     */
    ExpectedVoid switchQueue(int64_t qid);

    /**
     * @brief Advances to the next track per RepeatMode/shuffle.
     * @ingroup kanariv_engine
     * @return Success or Error State/Busy/NotFound/Device.
     * @details Delegates to advanceLocked with a 500 ms queue-lock budget,
     * so a colliding skip waits instead of failing Busy. Fast-path: if
     * !shuffle && repeat==One && hasCurrent, seeks decoder to 0 under
     * decodeMtx_ and resets ring without touching the queue.
     * @par Thread safety
     * Thread-safe; fast-path locks decodeMtx_, otherwise queueMutex_.
     * @see queueNextLocked
     * @see doPlayTrack
     */
    ExpectedVoid next();

    /**
     * @brief Moves to the previous track.
     * @ingroup kanariv_engine
     * @return Success or Error State/Busy/NotFound/Device ("at start"/"no perm").
     * @details Bounded 500 ms wait on queueMutex_, then queuePrevLocked + doPlayTrack.
     * Shuffle path steps cursor back by 2 and clamps; non-shuffle mirrors.
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_.
     * @see queuePrevLocked
     */
    ExpectedVoid prev();

    // Event delivery
    /**
     * @brief Sets runtime callbacks.
     * @ingroup kanariv_engine
     * @param cbs New kanariv::types::EngineCallbacks struct (copied).
     * @return Success (always).
     * @par Thread safety
     * Thread-safe; locks cbMutex_.
     * @see kanariv::types::EngineCallbacks
     * @see pushEvent
     */
    ExpectedVoid setCallbacks(const kanariv::types::EngineCallbacks& cbs);

    /**
     * @brief Pops one event from the MPSC queue (non-blocking).
     * @ingroup kanariv_engine
     * @return `std::expected<kanariv::types::EngineEvent, Error>` -- event on success, NotFound if
     * queue empty.
     * @par Thread safety
     * Thread-safe; the event queue is a bounded MPSC.
     * @see drainEvents
     * @see drainAll
     * @see kanariv::types::EngineEvent
     */
    std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> pollEvent();

    /**
     * @brief Drains up to cap events into a caller-provided buffer.
     * @ingroup kanariv_engine
     * @param buf Output buffer (may be null if cap==0).
     * @param cap Capacity of buf.
     * @param n Out: number of events written (must be non-null).
     * @return `std::expected<void, Error>` -- success or InvalidArg if n is null or buf is null
     * with cap>0.
     * @par Thread safety
     * Thread-safe; pops from the event queue (bounded MPSC).
     * @see pollEvent
     * @see drainAll
     * @see kanariv::types::EngineEvent
     */
    ExpectedVoid drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n);

    /**
     * @brief Drains all queued events into a vector.
     * @ingroup kanariv_engine
     * @return Vector of events (may be empty).
     * @par Thread safety
     * Thread-safe; pops from the event queue (bounded MPSC).
     * @see pollEvent
     * @see drainEvents
     * @see kanariv::types::EngineEvent
     */
    std::vector<kanariv::types::EngineEvent> drainAll();

  private:
    // The members below are service-only: they are not part of the public
    // playback API. `detail::ServiceAccess` (src/engine/engine_access.hpp) is
    // the single friend that lets `src/service` reach them.
    friend struct engine::detail::ServiceAccess;

    /**
     * @brief Suspends the loaded track and releases its file handles.
     * @ingroup kanariv_engine
     * @return Success or Error State/Busy.
     * @retval StatusCode::State if no track is loaded.
     * @retval StatusCode::Busy if the queue stays contended past the wait budget.
     * @details Closes reader/decoder (releasing the audio file) without
     * touching queue cursor, current track, or history mark. The file is
     * free for out-of-band modification (tag write) until
     * resumeAfterTagWrite(). State becomes Paused; output stopped.
     * @par Thread safety
     * Thread-safe; holds the queue lock across teardown (queue to
     * decodeMtx_ order, matching the rest of the engine).
     * @see resumeAfterTagWrite
     */
    ExpectedVoid suspendCurrentTrack();

    /**
     * @brief Reopens the suspended track at a captured position.
     * @ingroup kanariv_engine
     * @param t Track row to open (usually re-registered after a tag write).
     * @param pos Seconds to resume from (clamped to duration).
     * @param wasPaused Restore Paused instead of Playing when true.
     * @return Success or Error Busy/NotFound/Device/Internal.
     * @details No-op success when the current track id no longer matches
     * (the user moved on while suspended; the tag/db update already
     * landed). Otherwise replays via doPlayTrack, restores position with
     * seek, preserves the history mark (a tag edit is not a new listen),
     * and restores the prior paused/playing state.
     * @par Thread safety
     * Thread-safe; holds the queue lock across reopen (same order as above).
     * @see suspendCurrentTrack
     */
    ExpectedVoid resumeAfterTagWrite(const kanariv::types::Track& t, double pos, bool wasPaused);

    /** @brief Returns the unmute restore level (0,1]. @ingroup kanariv_engine
     * @details Last non-zero level before mute (persisted pre_mute_volume).
     * @return Restore level (atomic load, relaxed). */
    float preMuteVolume() const noexcept;

    /** @brief Sets the unmute restore level (sanitized, persisted on next save).
     * @ingroup kanariv_engine
     * @param g Level in (0,1]; anything else becomes 0.5.
     * @details Callers follow with setVolume(), which persists via saveState().
     * @par Thread safety
     * Thread-safe; atomic store. */
    ExpectedVoid setPreMuteVolume(float g);

    /** @brief Returns the shuffle permutation (positions) for a queue.
     * @ingroup kanariv_engine
     * @param qid Queue id.
     * @return Perm copy when shuffle applies to qid, else empty (insertion order).
     * Empty on lock contention too (callers fall back to insertion order).
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_. */
    std::vector<int64_t> shufflePermFor(int64_t qid) noexcept;

    /** @brief Extends the shuffle permutation after tracks were appended.
     * @ingroup kanariv_engine
     * @param qid Queue id the tracks were appended to.
     * @param count Number of appended tracks (at the queue end).
     * @details Best-effort: no-op unless shuffle applies to qid or the queue
     * lock is contended (stale-perm fallbacks cover display and playback).
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_. */
    void noteEnqueued(int64_t qid, std::size_t count) noexcept;

    /**
     * @brief Monitor auto-advance: like next(), but halts at queue end.
     * @ingroup kanariv_engine
     * @return Success or Error State/Busy/NotFound/Device.
     * @details With repeat Off and the cursor past the end, stops playback
     * (cursor stays parked so a later play wraps to the head) instead of
     * wrapping like next(). Repeat One/All behave like next().
     * @par Thread safety
     * Thread-safe; same locking as next().
     * @see next
     */
    ExpectedVoid autoNext();

    /**
     * @brief Constructs Engine with config defaults applied.
     * @ingroup kanariv_engine
     * @param cfg kanariv::types::EngineConfig; zero poll/gapless/history values are replaced by
     * defaults.
     * @details Applies defaults: pollMs 10, gaplessMs 300, historyThresholdPct 60,
     * historyThresholdSecs 90; initializes atomics and copies callbacks.
     */
    explicit Engine(const kanariv::types::EngineConfig& cfg);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::engine
