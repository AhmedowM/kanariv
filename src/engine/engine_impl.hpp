#pragma once

/**
 * @file engine_impl.hpp
 * @brief Engine::Impl and its state records, shared by the engine TUs (internal).
 * @ingroup kanariv_engine
 * @details Holds the queue/persistence mirrors, the crossfade fingerprint and
 * the full Impl declaration; the ownership contracts are documented on the
 * members. The public header stays cheap because this one is never installed.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <engine/queue_next.hpp>
#include <expected>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utils/mpsc_queue.hpp>
#include <vector>

namespace kanariv::engine {

// Transport-command wait budget: device setup under the queue lock routinely
// takes tens of ms (device open + preroll), so a colliding skip waits
// instead of failing Busy. Monitor paths keep try_lock (fail-fast).
inline constexpr std::chrono::milliseconds queueLockWait{500};
// The decode thread's crossfade publication is on the audio path, so it never
// waits on the queue lock for the full transport budget: a slow transport
// command wins and the publication is retried on the next decode iteration.
inline constexpr std::chrono::milliseconds crossfadePublishWait{100};

/**
 * @brief In-memory queue cursor and shuffle state.
 * @details `perm` is empty when shuffle is off; when on it holds a
 * permutation of `[0, queueCount)` shuffled via detail::shufflePerm.
 * `cursor` is the next index to consume (persisted as `cursor_pos`).
 * `queue_id` is the active queue (persisted as `active_queue_id`).
 * @par Ownership
 * `perm` and `cursor` are `queueMutex_`-only (a compound of them only makes
 * sense under that lock). `shuffle`, `repeat` and `queue_id` are atomics so
 * the lock-free observers `shuffle()`/`repeat()`/`activeQueueId()` cannot
 * tear a value: every writer already holds `queueMutex_`, and no multi-field
 * invariant relies on reading them in a particular order.
 */
struct QueueState {
    std::atomic<bool> shuffle{false}; ///< Whether shuffle is enabled.
    std::atomic<kanariv::types::RepeatMode> repeat{
        kanariv::types::RepeatMode::Off}; ///< Repeat mode.
    std::vector<int64_t> perm{};          ///< Shuffle permutation (positions, not track_ids).
    size_t cursor{0};                     ///< Next position index in perm or linear queue.
    std::atomic<int64_t> queue_id{
        kanariv::types::defaultQueueId}; ///< Active queue id (never persisted as 0).
};

/**
 * @brief In-memory mirrors of the persisted engine state.
 * @details `shuffleEnabled/repeatMode/cursorPos/activeQueueId` publish the queue
 * fields; the sidecar record itself is rebuilt from `queue_` at save time.
 * `currentTrackId` is the published current track and `currentPath` its stable
 * identity for the sidecar. `volume`/`preMuteVolume`/`positionMs` mirror the
 * persisted values.
 * @par Ownership
 * `queueMutex_`-only for writers (attach/load, pause/seek/stop/play/track
 * change, retry checkpoint). Every field except `currentPath` is an atomic so
 * the two lock-free observers -- `currentTrackId()` and `position()` without a
 * loaded track -- read one narrow value instead of the struct; consistency
 * across fields still requires `queueMutex_`.
 */
struct EngineState {
    std::atomic<int> shuffleEnabled{0}; ///< Persisted shuffle flag (0/1).
    std::atomic<kanariv::types::RepeatMode> repeatMode{
        kanariv::types::RepeatMode::Off};   ///< Persisted repeat mode.
    std::atomic<int64_t> cursorPos{0};      ///< Persisted cursor (QueueState::cursor).
    std::atomic<int64_t> currentTrackId{0}; ///< Published current track id.
    std::string currentPath;                ///< Current track path (stable sidecar identity).
    std::atomic<float> volume{1.0f};        ///< Persisted volume [0,1].
    std::atomic<int64_t> activeQueueId{1};  ///< Persisted active queue id.
    std::atomic<float> preMuteVolume{0.5f}; ///< Restore level for unmute (0,1].
    std::atomic<int64_t> positionMs{0};     ///< Persisted position in the current track.
};

/**
 * @brief Snapshot of the queue fields a next-entry peek depends on.
 * @details A crossfade preload peeks the next entry without the queue lock,
 * then re-locks to commit. The commit is applied only while the queue still
 * compares equal to the snapshot taken before the peek, so a concurrent
 * `next()`/`play()`/queue edit cannot have its cursor overwritten.
 */
struct QueueFingerprint {
    int64_t queueId{0};                                                 ///< Active queue id.
    bool shuffle{false};                                                ///< Shuffle flag.
    kanariv::types::RepeatMode repeat{kanariv::types::RepeatMode::Off}; ///< Repeat mode.
    size_t cursor{0};                                                   ///< Cursor at peek time.
    std::vector<int64_t> perm{}; ///< Permutation at peek time.

    bool operator==(const QueueFingerprint& other) const = default;
};

struct Engine::Impl {
    explicit Impl(const kanariv::types::EngineConfig& cfg);

    // lifecycle
    std::optional<kanariv::utils::Error> init();
    void shutdown();

    // persistence
    bool hasDb() const noexcept;
    ExpectedVoid attachStorage(kanariv::contracts::Storage storage);
    std::optional<kanariv::utils::Error> loadState();
    /**
     * @brief Persists the engine state snapshot to the playback-state sidecar.
     * @pre `queueMutex_` is held by the caller (name suffix `Locked`).
     * @param livePositionMs Decode-lock-free position for callers that already
     * hold `decodeMtx_`; omit to derive it here (the usual case).
     * @details Rebuilds the record from `queue_` and the published current
     * track, so cursor/shuffle/repeat edits are persisted by this one call.
     * Takes `decodeMtx_` briefly to derive the live position unless the caller
     * supplies one, so by default the caller must not already hold it. Store
     * I/O therefore runs under the queue lock (the established persistence
     * story) but never under the decode lock.
     */
    kanariv::utils::Expected<void>
    saveStateLocked(std::optional<int64_t> livePositionMs = std::nullopt);
    /**
     * @brief saveStateLocked() ignoring the outcome, logging a failure.
     * @details Used on checkpoint/teardown paths where a persist failure must
     * not fail the operation but must not be silent either.
     * @pre `queueMutex_` is held by the caller (same as saveStateLocked).
     */
    void saveStateBestEffort(std::optional<int64_t> livePositionMs = std::nullopt);
    /// @brief output_->start() ignoring the outcome, logging a failure.
    void startOutputBestEffort();
    /// @brief output_->stop() ignoring the outcome, logging a failure.
    void stopOutputBestEffort();
    kanariv::utils::Expected<kanariv::types::TrackRow> fetchTrackByPosLocked(int64_t qid,
                                                                             int64_t pos);

    // queue locking
    /**
     * @brief RAII handle for `queueMutex_`.
     * @details Produced by tryLockQueue()/tryLockQueueFor(); releases the lock
     * in its destructor so early returns cannot leak it. Call `release()` when
     * the critical section ends before the scope does (e.g. to publish events
     * outside the queue lock).
     */
    class QueueGuard {
      public:
        QueueGuard() noexcept = default;
        QueueGuard(QueueGuard&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}
        QueueGuard(const QueueGuard&) = delete;
        QueueGuard& operator=(const QueueGuard&) = delete;
        QueueGuard& operator=(QueueGuard&&) = delete;
        ~QueueGuard() {
            if (impl_)
                impl_->unlockQueue();
        }

        explicit operator bool() const noexcept {
            return impl_ != nullptr;
        }
        /// @brief Releases the lock now (the destructor then does nothing).
        void release() noexcept {
            if (impl_) {
                impl_->unlockQueue();
                impl_ = nullptr;
            }
        }

      private:
        friend struct Impl;
        explicit QueueGuard(Impl* impl) noexcept : impl_(impl) {}

        Impl* impl_{nullptr};
    };
    /// @brief Non-blocking acquisition; returns an empty guard when contended.
    QueueGuard tryLockQueue() noexcept;
    /// @brief Bounded acquisition; returns an empty guard on timeout.
    QueueGuard tryLockQueueFor(std::chrono::milliseconds wait) noexcept;
    void unlockQueue() noexcept;
    std::unexpected<kanariv::utils::Error> queueBusy(std::string_view op,
                                                     std::string_view msg) const;

    // transport
    ExpectedVoid play(int64_t queueId);
    ExpectedVoid playTrack(int64_t trackId);
    ExpectedVoid pause();
    /// @brief pause() body; @pre `queueMutex_` held (order queue -> decodeMtx_).
    ExpectedVoid pauseLocked();
    /**
     * @brief Publishes a playback-state change.
     * @param to New state.
     * @param reason Short caller label, used only in the debug log.
     * @details Sole writer of `playbackState_`: every transport path routes
     * through it, so the state machine has one store site and one log line.
     */
    void transition(kanariv::types::PlaybackState to, std::string_view reason);
    ExpectedVoid resume();
    ExpectedVoid stop();
    ExpectedVoid seek(double seconds);
    ExpectedVoid suspendCurrentTrack();
    ExpectedVoid resumeAfterTagWrite(const kanariv::types::TrackRow& t, double pos, bool wasPaused);
    ExpectedVoid next();
    ExpectedVoid autoNext();
    /**
     * @brief autoNext() restricted to the track transition the caller saw.
     * @param epoch `transitionEpoch_` sampled when the caller decided to
     * advance (monitor snapshot, or the decode thread at EOF).
     * @details Two drivers may decide to advance the same track (gapless arm in
     * engineTick, EOF in decodeLoop). The first transition bumps the epoch, so
     * a later call carrying the old one is a no-op instead of a double skip.
     */
    ExpectedVoid autoNextIfEpoch(uint64_t epoch);
    ExpectedVoid prev();
    ExpectedVoid advanceLocked(bool stopAtEnd, int lockWaitMs = 0,
                               std::optional<uint64_t> expectedEpoch = std::nullopt);
    ExpectedVoid switchQueue(int64_t qid);

    // volume / state accessors
    ExpectedVoid setVolume(float g);
    float volume() const noexcept;
    ExpectedVoid setPreMuteVolume(float g);
    float preMuteVolume() const noexcept;
    kanariv::types::PlaybackState state() const noexcept;
    double duration() const noexcept;
    double position() const noexcept;
    int64_t currentTrackId() const noexcept;
    /**
     * @brief Position estimate from the transport counters.
     * @pre `decodeMtx_` is held by the caller (it reads `pausePos_`,
     * `playStart_` and `duration_`); the name suffix documents that contract.
     * Only the `hasCurrent_ == false` branch touches `state_.positionMs`,
     * which is atomic.
     */
    double currentPositionLocked() const noexcept;
    bool shuffle() const noexcept;
    kanariv::types::RepeatMode repeat() const noexcept;
    int64_t activeQueueId() const noexcept;
    // shuffle / repeat
    std::vector<int64_t> shufflePermFor(int64_t qid);
    void noteEnqueued(int64_t qid, std::size_t count);
    void noteQueueMoved(int64_t qid, std::size_t from, std::size_t to);
    ExpectedVoid setShuffle(std::optional<bool> on);
    kanariv::utils::Expected<void> setShuffleLocked(bool on);
    ExpectedVoid setRepeat(kanariv::types::RepeatMode m);

    // library / history
    kanariv::utils::Expected<kanariv::types::PlaybackStats> stats();
    kanariv::utils::Expected<std::vector<kanariv::types::HistoryEntry>> listHistory(int limit);
    kanariv::utils::Expected<void> clearHistory();

    // queue traversal
    kanariv::utils::Expected<void> queueNextLocked(kanariv::types::TrackRow& out);
    kanariv::utils::Expected<void> queuePrevLocked(kanariv::types::TrackRow& out);
    /**
     * @brief Resolved next entry plus the cursor change that selects it.
     * @details Produced read-only by peekNextLocked(), applied by
     * commitNextLocked() under the same lock.
     */
    struct PeekedNext {
        kanariv::types::TrackRow track{}; ///< Entry at `plan.position`.
        detail::NextPlan plan{};          ///< Cursor/permutation change to commit.
        int64_t queueId{1};               ///< Effective queue id (0 normalized to 1).
    };
    /**
     * @brief Resolves the next queue entry without mutating cursor/perm.
     * @pre `queueMutex_` held by the caller.
     */
    kanariv::utils::Expected<PeekedNext> peekNextLocked();
    /**
     * @brief Commits @p next when the queue still matches @p expected.
     * @pre `queueMutex_` held by the caller.
     * @param livePositionMs Forwarded to the persistence call; required when
     * the caller also holds `decodeMtx_` (decodeLoop's preload path).
     * @return false when the queue changed since the peek (nothing committed).
     */
    bool commitNextLocked(const QueueFingerprint& expected, PeekedNext&& next,
                          std::optional<int64_t> livePositionMs = std::nullopt);
    /// @brief Snapshots the queue fields tracked by QueueFingerprint.
    /// @pre `queueMutex_` held by the caller.
    QueueFingerprint queueFingerprintLocked() const;
    /**
     * @brief Opens and starts (or resumes) @p t at @p startSec.
     * @pre `queueMutex_` held by the caller.
     * @return The TrackStarted event to publish with pushEvent() *after*
     * releasing `queueMutex_`: callbacks run on the caller's thread and must
     * never fire under the queue lock (E8).
     */
    kanariv::utils::Expected<kanariv::types::EngineEvent> doPlayTrack(kanariv::types::TrackRow& t,
                                                                      double startSec = 0.0);
    void preroll();

    // callbacks / events
    ExpectedVoid setCallbacks(const kanariv::types::EngineCallbacks& cbs);
    kanariv::utils::Expected<kanariv::types::EngineEvent> pollEvent();
    ExpectedVoid drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n);
    std::vector<kanariv::types::EngineEvent> drainAll();

    // monitor / decode
    /**
     * @brief Immutable copy of the published transport state for one tick.
     * @details Built under `queueMutex_` (plus a brief `decodeMtx_`, the
     * documented queue-then-decode order) so the monitor never reads
     * queue-lock-only or decode-lock-only fields without their writer lock.
     * Database writes and event dispatch then happen after the locks are
     * released.
     */
    struct TickSnapshot {
        bool hasCurrent{false};      ///< hasCurrent_ at tick time.
        int64_t trackId{0};          ///< Published current track id.
        int64_t queueId{1};          ///< Active queue id.
        int64_t startedMs{0};        ///< TrackRow start wall clock (ms).
        double duration{0};          ///< duration_ at tick time.
        double position{0};          ///< currentPositionLocked() at tick time.
        uint64_t transitionEpoch{0}; ///< transitionEpoch_ at tick time.
        kanariv::types::PlaybackState playback{kanariv::types::PlaybackState::Stopped};
    };
    /// @brief Builds a TickSnapshot; @pre `queueMutex_` held by the caller.
    TickSnapshot snapshotLocked() const;
    void engineTick();
    void resetPlaybackAccounting() noexcept;
    void accountPlaybackTime() noexcept;
    void monitorLoop(std::stop_token st);
    void decodeLoop(std::stop_token st);
    /**
     * @brief Publishes the incoming crossfade track as the playing track.
     * @pre Called on the decode thread with neither mutex held.
     * @details Takes `queueMutex_` (bounded, `crossfadePublishWait`) then
     * `decodeMtx_`, swaps `crossfadeDecoder_`/`crossfadeReader_` into
     * `decoder_`/`reader_`, republishes `currentTrack_`/`duration_`/position,
     * resets the playback accounting and persists the new cursor/current
     * track under the queue lock (no DB I/O under `decodeMtx_`). The
     * TrackStarted event is pushed after both locks are released.
     * @return false when the queue lock stayed contended; `crossfadeActive_`
     * remains set and the caller retries on a later decode iteration.
     */
    bool tryPublishCrossfade();
    /**
     * @brief Drops an armed or in-flight crossfade.
     * @pre `decodeMtx_` is held by the caller (or no decode thread is running).
     * @details A track change must not inherit the previous transition's
     * preloaded decoder: otherwise decodeLoop keeps mixing the stale incoming
     * track and tryPublishCrossfade() would publish it over the new track.
     */
    void resetCrossfadeLocked() noexcept;
    /// @brief History mark for one ticked snapshot (no locks required).
    void doHistoryMark(const TickSnapshot& snap);
    void pushEvent(const kanariv::types::EngineEvent& ev);

    // members (moved verbatim from Engine)
    //
    // Ownership contract (see also include/kanariv/engine/engine.hpp):
    // - queueMutex_ owns state_, currentTrack_, startedMs_ and the compound
    //   invariant of queue_ (cursor + perm). Writers must hold it; lock-free
    //   readers use the narrow atomics documented on those structs/fields.
    // - decodeMtx_ owns reader_/decoder_/ring_/output_, duration_, pausePos_,
    //   playStart_ and the fade/crossfade counters. Control access (start,
    //   stop, setVolume, reset) to the audio graph happens under it.
    // - Lock order is queueMutex_ then decodeMtx_; the queue lock is never
    //   taken while decodeMtx_ is held.
    // - playbackState_, hasCurrent_, markedPlayed_, gaplessArmed_ and the
    //   accounting counters are atomics (written under the relevant lock,
    //   read lock-free).
    kanariv::types::EngineConfig cfg_{};
    EngineState state_{};
    QueueState queue_{};
    kanariv::types::TrackRow currentTrack_{}; ///< queueMutex_ only.
    std::atomic<bool> hasCurrent_{false};
    double duration_{0}; ///< decodeMtx_ only.
    std::atomic<float> volume_{1.0f};
    std::atomic<float> preMute_{0.5f};
    std::atomic<kanariv::types::PlaybackState> playbackState_{
        kanariv::types::PlaybackState::Stopped};
    // suspended_: set by suspendCurrentTrack() while decoder/reader are released
    // for an out-of-band tag write; the transport keeps reporting Paused. Every
    // Paused -> Playing path (resume/play/seek) fails State while it is set,
    // because there is no decoder to play. doPlayTrack() reopens the file and
    // clears it (resumeAfterTagWrite's path).
    std::atomic<bool> suspended_{false};
    std::chrono::steady_clock::time_point playStart_{}; ///< decodeMtx_ only.
    double pausePos_{0};                                ///< decodeMtx_ only.
    std::atomic<uint64_t> accumulatedPlaybackMs_{0};
    std::atomic<uint64_t> lastPlaybackSampleMs_{0};
    std::atomic<bool> markedPlayed_{false};
    std::atomic<int64_t> startedMs_{0}; ///< Atomic mirror of the queue-locked start time.
    // transitionEpoch_: bumped by every successful track transition (a fresh
    // doPlayTrack or a repeat-one restart) while `queueMutex_` is held. The two
    // auto-advance drivers (monitor gapless arm, decode EOF) carry the value
    // they decided on, so only the first of them moves the queue (E12).
    std::atomic<uint64_t> transitionEpoch_{0};
    std::string lastErr_{}; ///< Transport-thread only (diagnostics).

    kanariv::contracts::Storage storage_{};

    // player (decodeMtx_ only)
    std::unique_ptr<kanariv::media::Reader> reader_{};
    std::unique_ptr<kanariv::media::Decoder> decoder_{};
    // Shared so the decode thread can keep a parked ring alive while it waits
    // after releasing decodeMtx_ (P8).
    std::shared_ptr<kanariv::utils::SpscRing<float>> ring_{};
    std::unique_ptr<kanariv::player::AudioOutput> output_{};

    std::jthread decodeThread_{};
    std::atomic<bool> decodeRun_{false};
    mutable std::mutex decodeMtx_{};
    std::condition_variable_any decodeCv_{};

    // monitor
    std::jthread monitorThread_{};
    std::atomic<bool> monRun_{false};
    std::mutex monMtx_{};
    std::condition_variable_any monCv_{};

    // events
    kanariv::utils::MpscQueue<kanariv::types::EngineEvent> eventQueue_{64};
    std::mutex cbMutex_{};
    kanariv::types::EngineCallbacks callbacks_{};

    // atomics spec required
    std::atomic<uint64_t> lastProgressMs_{0};
    // M-3: volume-drag throttle -- setVolume() persists at most 1x/s
    // (crash-window volume loss <= 1 s; shutdown/track-change saves cover it).
    std::atomic<uint64_t> lastVolSaveMs_{0};
    // Session position persistence: engineTick() writes at most 1x/5s while
    // playing; pause/seek/shutdown always save (crash window <= 5 s).
    std::atomic<uint64_t> lastPosSaveMs_{0};
    // gaplessArmed_: 0->1 CAS arms gapless pre-roll ~300ms before track end (gaplessMs).
    // Reset to false on TrackStarted / next() failure. Requires engineTick() single-writer.
    std::atomic<bool> gaplessArmed_{false};
    // Playback polish state (real-time safe; decodeMtx_ owns the per-track
    // envelope values, decodeLoop snapshots them under that lock so the mix
    // never reads them unlocked). fadeSample_ counts frames into the current
    // fade; fadeInSamples_/fadeOutSamples_ are precomputed per track in
    // frames; fadeOutStart_ holds the position where fade-out begins.
    std::atomic<uint64_t> fadeSample_{0};
    uint64_t fadeInSamples_{0};  ///< decodeMtx_ only.
    uint64_t fadeOutSamples_{0}; ///< decodeMtx_ only.
    double fadeOutStart_{0};     ///< decodeMtx_ only.
    // interTrackGapArmed_: the outgoing tail has drained and the silence clock
    // is running; decodeLoop advances once interTrackGapMs has elapsed. Owned by
    // the decode thread; cleared by doPlayTrack()/stop() so a manual transition
    // cannot inherit a half-elapsed gap.
    std::atomic<bool> interTrackGapArmed_{false};
    uint64_t interTrackGapStartMs_{0}; ///< Decode thread only.
    // Crossfade state: when crossfadeMs > 0, the outgoing track is faded out
    // while the incoming track is faded in, and both are mixed into the ring
    // during the overlap (an overlap is not a gap; interTrackGapMs is ignored).
    // Ownership: only the decode thread mutates these, always under decodeMtx_
    // (the preload transaction and the completion swap both run in decodeLoop).
    // crossfadeActive_ is atomic because engineTick (monitor thread) reads it
    // unlocked to avoid arming a second crossfade.
    std::unique_ptr<kanariv::media::Decoder> crossfadeDecoder_; ///< decodeMtx_ only.
    std::unique_ptr<kanariv::media::Reader> crossfadeReader_;   ///< decodeMtx_ only.
    kanariv::types::TrackRow crossfadeTrack_{};                 ///< decodeMtx_ only.
    uint64_t crossfadeSamples_{0}; ///< decodeMtx_ only: total crossfade length in frames.
    uint64_t crossfadeSample_{0};  ///< decodeMtx_ only: current frame within crossfade.
    std::atomic<bool> crossfadeActive_{false};  ///< Written under decodeMtx_, read lock-free.
    std::atomic<bool> crossfadePending_{false}; // set by engineTick, consumed by decodeLoop
    mutable std::timed_mutex queueMutex_;
};

} // namespace kanariv::engine
