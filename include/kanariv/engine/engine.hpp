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
 * - Gapless transition: the shared playback pipeline prerolls its ring to half
 *   capacity before the device starts and its producer thread continuously
 *   refills it from the engine's Source; engineTick arms gaplessArmed_ (CAS
 *   0->1) when remaining <= gaplessMs (300 ms) and calls next().
 * - Threads: the pipeline's producer thread runs the engine's Source (SPSC
 *   ring, decodeMtx_, per-chunk generation token); monitorLoop runs on
 *   monitorThread_ polling every pollMs (default 10 ms) via monMtx_/monCv_
 *   and calling engineTick() (history + progress + gapless).
 * - Persistence: engine state (shuffle, repeat, cursor, current track, volume,
 *   shuffle permutation, active queue) is loaded at attach and saved on
 *   transport transitions through the `contracts::IPlaybackStateStore` port.
 * - History: doHistoryMark uses shouldMarkPlayed(historyThresholdPct/Secs)
 *   with atomic CAS on markedPlayed_ and a database-owned transaction.
 * Locking: two engine mutexes with a documented ownership split and a fixed
 * order -- `queueMutex_` then `decodeMtx_` then the pipeline's own graph
 * mutex; never the reverse.
 * - `queueMutex_` (timed mutex) owns `state_`, `currentTrack_`, `startedMs_`
 *   and the compound invariant of `queue_` (cursor + shuffle permutation).
 *   Fast/monitor paths try_lock; transport commands (play/next/prev/pause/
 *   stop/seek) wait boundedly (500 ms) and report Busy on timeout.
 * - `decodeMtx_` (mutex) owns the decoders (`reader_`/`decoder_`), `duration_`,
 *   `suspendPos_` and the fade / crossfade counters. The audio graph itself
 *   (ring, sink, producer thread, position clock) belongs to the shared
 *   `player::detail::Pipeline`, whose control operations serialize internally
 *   and never wait on the producer.
 * - Crossfade fields are mutated only by the producer thread under `decodeMtx_`;
 *   `std::atomic<bool> crossfadeActive_` is the lock-free signal the monitor
 *   thread observes (it never reads the crossfade members themselves).
 * - Lock-free readers use only the atomics published by the owning writer
 *   (`playbackState_`, `hasCurrent_`, `state_.currentTrackId`, `volume_`,
 *   `markedPlayed_`, `gaplessArmed_`). Persistence and history operations go
 *   through the backend-neutral contracts::Storage ports and never hold
 *   `decodeMtx_` across a database write.
 * Events: a bounded event queue (64 slots) with drop-on-full, dispatched via
 * pushEvent to callbacks under cbMutex_.
 * Implementation details (atomics, threads, mpsc queue, ring, DB rows) live
 * in src/engine/engine.cpp behind Engine::Impl (pimpl) so this header stays
 * cheap; the public API is unchanged.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/contracts/storage.hpp>
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
 * @brief Service-only access to `Engine`'s implementation.
 * @ingroup kanariv_engine
 * @details Declared here so `Engine` can name it as its single friend; the
 * definition lives in the internal header `src/engine/engine_access.hpp` and
 * is never installed.
 */
struct EngineAccess;
} // namespace detail

/**
 * @brief Main playback engine -- queue, decoding, gapless, history and persistence.
 * @ingroup kanariv_engine
 * @details See file-level docs for state machine, threading and persistence.
 * Public mutators that touch the queue state use `tryLockQueue()` (non-blocking,
 * `Busy` on contention); transport commands (play/next/prev/pause/stop/seek)
 * instead wait up to 500 ms via `tryLockQueueFor()` so a colliding command
 * executes instead of failing. The lock order is `queueMutex_` then
 * `decodeMtx_` then the playback pipeline's graph mutex. `currentTrack_`,
 * `startedMs_` and `state_` are queue-lock state; `duration_`, `suspendPos_`
 * and the decoders are decode-lock state, while the audio graph (ring, sink,
 * producer thread, position clock) belongs to the shared pipeline. Published
 * atomics (`playbackState_`, `hasCurrent_`,
 * `volume_`, `markedPlayed_`, `gaplessArmed_`, `crossfadeActive_` and the
 * atomic fields of `state_`) keep the lock-free API surfaces consistent.
 *
 * Error codes: `InvalidArg` (null/empty args), `State` (no db / not
 * playing/paused), `NotFound` (empty queue / no event), `Busy` (queue
 * locked / transaction begin failed), `Internal` (SQLite / decoder),
 * `NoMem` (perm too large).
 *
 * Thread safety: thread-safe for concurrent play/pause/next/prev/stop/seek/
 * setVolume/position/currentTrackId and the other accessors; each call either
 * takes the documented lock or reads a published atomic. `queueMutex_` is
 * try_lock on fast paths and bounded-wait on transport commands, so callers
 * must still handle Busy (budget exceeded).
 * @see kanariv::types::PlaybackState
 * @see kanariv::types::EngineConfig
 */
class KANARIV_API Engine final {
  public:
    using ExpectedVoid = kanariv::utils::Expected<void>; ///< Void or Error.
    using ExpectedEngine =
        kanariv::utils::Expected<std::unique_ptr<Engine>>; ///< Engine ptr or Error.

    /**
     * @brief Creates an Engine with no database attached.
     * @ingroup kanariv_engine
     * @param cfg Engine configuration (poll/gapless/history thresholds).
     * @return Owning Engine or Error from init().
     * @details Creates the playback pipeline and optionally monitorThread_ via
     * init().
     * No database is opened; optionally attach persistence with
     * attachStorage().
     * @par Thread safety
     * Thread-safe; constructs internal atomics/threads.
     * @see attachStorage
     */
    static ExpectedEngine create(const kanariv::types::EngineConfig& cfg = {});

    /**
     * @brief Attaches already-open persistence ports.
     * @ingroup kanariv_engine
     * @param storage Complete port aggregate (every port populated and open).
     * @return Success or Error InvalidArg / loadState failure.
     * @retval StatusCode::InvalidArg if a port is missing or the store is closed.
     * @par Ownership
     * The engine retains shared ownership of every port and keeps them attached
     * until shutdown or replacement; callers may release their own copies after
     * this call returns.
     * @par Thread safety
     * Thread-safe; loads persisted state under the queue lock.
     * @see loadState
     */
    ExpectedVoid attachStorage(kanariv::contracts::Storage storage);

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
     *   start AudioOutput. Fails State when the paused track is suspended
     *   for a tag write (no decoder to start; the daemon reopens it).
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
     * @param trackId TrackRow id to select (must be in the active queue).
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
     * @return Success or Error State/Busy.
     * @retval StatusCode::State if not playing.
     * @retval StatusCode::Busy if `queueMutex_` stays contended past the wait
     * budget; playback and the persisted state are left untouched (retry).
     * @details Waits boundedly (500 ms) on `queueMutex_`, then stops the
     * output and saves the paused position under `decodeMtx_`.
     * @par Thread safety
     * Thread-safe; takes `queueMutex_` then `decodeMtx_` (the engine's
     * queue-to-decode lock order).
     */
    ExpectedVoid pause();

    /**
     * @brief Resumes from paused.
     * @ingroup kanariv_engine
     * @return Success or Error State if not paused, or if the paused track is
     * suspended for a tag write (the decoder is closed; only
     * the daemon's tag-write path may reopen it).
     * @par Thread safety
     * Thread-safe; notifies decode/monitor cvs.
     */
    ExpectedVoid resume();

    /**
     * @brief Stops playback and resets position/ring.
     * @ingroup kanariv_engine
     * @return Success, or Error Busy when the queue lock stays contended past
     * the wait budget.
     * @retval StatusCode::Busy if `queueMutex_` cannot be acquired; playback
     * state, cursor and persisted position are left untouched so the caller
     * can retry.
     * @details Waits boundedly (500 ms) for `queueMutex_`, then, under that
     * lock: clears the resume position, moves the queue cursor back onto the
     * stopped track (by track identity, mapped through the shuffle
     * permutation when one is active), persists the cursor, tears down
     * decoder/output/ring under `decodeMtx_`, and saves state. An
     * already-stopped engine returns success without touching anything.
     * @par Thread safety
     * Thread-safe; takes `queueMutex_` then `decodeMtx_` (the engine's
     * queue-to-decode lock order).
     * @see play
     */
    ExpectedVoid stop();

    /**
     * @brief Seeks to a position in the current track.
     * @ingroup kanariv_engine
     * @param seconds Target position in seconds [0, duration].
     * @return Success or Error State/InvalidArg/Internal/Busy.
     * @retval StatusCode::State if no current track, or the track is
     * suspended for a tag write (no decoder to seek).
     * @retval StatusCode::InvalidArg if seconds is NaN/inf/negative.
     * @retval StatusCode::Internal if decoder seek fails (state restored).
     * @retval StatusCode::Busy if `queueMutex_` stays contended past the wait
     * budget; nothing is changed (retry).
     * @details Waits boundedly (500 ms) for `queueMutex_` (lock order
     * queue-to-decode), then holds decodeMtx_ to serialize with the producer's
     * Source (decoder_->decode) and to clamp the target against `duration_`,
     * then repositions the shared pipeline (clock, ring and source seek in one
     * serialized operation).
     * The playback state found on entry is restored afterwards (and after a
     * failed seek), so seeking while paused does not start audio and seeking
     * while stopped does not begin playback.
     * @par Thread safety
     * Thread-safe; takes `queueMutex_` then `decodeMtx_`.
     */
    ExpectedVoid seek(double seconds);

    /**
     * @brief Sets playback volume and persists it.
     * @ingroup kanariv_engine
     * @param g Volume in [0,1] (clamped; NaN/inf -> 0).
     * @return Success (always).
     * @details Updates volume_ atomically first (so a contended lock never
     * loses the new level), then takes `queueMutex_` best-effort to publish
     * state_.volume and persist via saveState(), and `decodeMtx_` to forward
     * to AudioOutput::setVolume. Returns success even when both locks are
     * contended; the persisted value is refreshed by the next save.
     * @par Thread safety
     * Thread-safe; atomic volume plus queue-then-decode locks.
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
     * @return Duration (cached from decoder or metadata; decodeMtx_ state, so
     * the call takes that mutex briefly). */
    double duration() const noexcept;
    /** @brief Returns current playback position in seconds. @ingroup kanariv_engine
     * @return Position (persisted resume position if stopped with no loaded
     * track; the frozen position while suspended; otherwise the pipeline's
     * clock, clamped to the duration).
     * Takes `decodeMtx_` briefly; queue-lock state is never read without it. */
    double position() const noexcept;
    /** @brief Returns current track id (or persisted one if no current). @ingroup kanariv_engine
     * @return TrackRow id if hasCurrent_, else state_.currentTrackId (persisted).
     * Lock-free: reads the atomic published by the queue-lock writer. */
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
     * @return `kanariv::utils::Expected<types::PlaybackStats>` -- stats on success, Error if no db.
     * @par Thread safety
     * Thread-safe; delegates to Database::getStats() which takes the db lock.
     * @see kanariv::db::Database::getStats
     * @see kanariv::types::PlaybackStats
     */
    kanariv::utils::Expected<kanariv::types::PlaybackStats> stats();

    /**
     * @brief Lists history entries.
     * @ingroup kanariv_engine
     * @param limit Max rows (0 = no limit, default 50).
     * @return `kanariv::utils::Expected<std::vector<types::HistoryEntry>>` -- vector on success,
     * State if no db. Entries carry track snapshots (title/artist/path/duration).
     * @par Thread safety
     * Thread-safe; takes the database mutex exclusively.
     * @see kanariv::types::HistoryEntry
     */
    kanariv::utils::Expected<std::vector<kanariv::types::HistoryEntry>> listHistory(int limit = 50);

    /**
     * @brief Clears all history entries.
     * @ingroup kanariv_engine
     * @return `kanariv::utils::Expected<void>` -- success or State/Internal.
     * @par Thread safety
     * Thread-safe; takes unique_lock on the database mutex.
     */
    kanariv::utils::Expected<void> clearHistory();

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
     * @brief Inverts the current shuffle mode.
     * @ingroup kanariv_engine
     * @return Success or Error State/Busy/Internal.
     * @details Reads and rewrites the mode under one queueMutex_ acquisition,
     * so a concurrent setShuffle cannot slip between the read and the write.
     * @par Thread safety
     * Thread-safe; try_lock on queueMutex_.
     */
    ExpectedVoid toggleShuffle();

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
     * @return queue_.queue_id if set, else 1 (atomic load; no lock).
     * @par Thread safety
     * Thread-safe; lock-free. `queue_.queue_id` is atomic and written only
     * under `queueMutex_`.
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
     * @details Validates via `storage_.queues->getQueue(qid)` while holding
     * queueMutex_ (then the store lock). On switch: resets cursor to 0,
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
     * @return `kanariv::utils::Expected<kanariv::types::EngineEvent>` -- event on success, NotFound
     * if queue empty.
     * @par Thread safety
     * Thread-safe; the event queue is a bounded MPSC.
     * @see drainEvents
     * @see drainAll
     * @see kanariv::types::EngineEvent
     */
    kanariv::utils::Expected<kanariv::types::EngineEvent> pollEvent();

    /**
     * @brief Drains up to cap events into a caller-provided buffer.
     * @ingroup kanariv_engine
     * @param buf Output buffer (may be null if cap==0).
     * @param cap Capacity of buf.
     * @param n Out: number of events written (must be non-null).
     * @return `kanariv::utils::Expected<void>` -- success or InvalidArg if n is null or buf is null
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
    // playback API. `detail::EngineAccess` (src/engine/engine_access.hpp) is
    // the single friend that lets `src/service` reach them.
    friend struct engine::detail::EngineAccess;

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
