#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <engine/fade.hpp>
#include <engine/history.hpp>
#include <engine/queue_next.hpp>
#include <engine/shuffle.hpp>
#include <expected>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/status_format.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <player/playback_policy.hpp>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <utils/mpsc_queue.hpp>
#include <utils/path_utf8.hpp>
#include <vector>

namespace kanariv::engine {

// Transport-command wait budget: device setup under the queue lock routinely
// takes tens of ms (device open + preroll), so a colliding skip waits
// instead of failing Busy. Monitor paths keep try_lock (fail-fast).
namespace {
constexpr std::chrono::milliseconds queueLockWait{500};
// The decode thread's crossfade publication is on the audio path, so it never
// waits on the queue lock for the full transport budget: a slow transport
// command wins and the publication is retried on the next decode iteration.
constexpr std::chrono::milliseconds crossfadePublishWait{100};
} // namespace

// All engine state and implementation live here so the public header stays
// cheap (no mpsc/ring/std-threading includes). Engine methods delegate.

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
    std::expected<void, kanariv::utils::Error>
    saveStateLocked(std::optional<int64_t> livePositionMs = std::nullopt);
    std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    fetchTrackByPosLocked(int64_t qid, int64_t pos);

    // queue locking
    bool tryLockQueue() noexcept;
    bool tryLockQueueFor(std::chrono::milliseconds wait) noexcept;
    void unlockQueue() noexcept;
    std::unexpected<kanariv::utils::Error> queueBusy(std::string_view op,
                                                     std::string_view msg) const;

    // transport
    ExpectedVoid play(int64_t queueId);
    ExpectedVoid playTrack(int64_t trackId);
    ExpectedVoid pause();
    /// @brief pause() body; @pre `queueMutex_` held (order queue -> decodeMtx_).
    ExpectedVoid pauseLocked();
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
    std::expected<void, kanariv::utils::Error> setShuffleLocked(bool on);
    ExpectedVoid setRepeat(kanariv::types::RepeatMode m);

    // library / history
    std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> stats();
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(int limit);
    std::expected<void, kanariv::utils::Error> clearHistory();

    // queue traversal
    std::expected<void, kanariv::utils::Error> queueNextLocked(kanariv::types::TrackRow& out);
    std::expected<void, kanariv::utils::Error> queuePrevLocked(kanariv::types::TrackRow& out);
    /**
     * @brief Resolved next entry plus the cursor change that selects it.
     * @details Produced read-only by peekNextLocked(), applied by
     * commitNextLocked() under the same lock.
     */
    struct PeekedNext {
        kanariv::types::TrackRow track{}; ///< Entry at `plan.position`.
        detail::NextPlan plan{};       ///< Cursor/permutation change to commit.
        int64_t queueId{1};            ///< Effective queue id (0 normalized to 1).
    };
    /**
     * @brief Resolves the next queue entry without mutating cursor/perm.
     * @pre `queueMutex_` held by the caller.
     */
    std::expected<PeekedNext, kanariv::utils::Error> peekNextLocked();
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
    std::expected<kanariv::types::EngineEvent, kanariv::utils::Error>
    doPlayTrack(kanariv::types::TrackRow& t, double startSec = 0.0);
    void preroll();

    // callbacks / events
    ExpectedVoid setCallbacks(const kanariv::types::EngineCallbacks& cbs);
    std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> pollEvent();
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
    std::string lastErr_{};             ///< Transport-thread only (diagnostics).

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

Engine::Engine(const kanariv::types::EngineConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

Engine::ExpectedVoid Engine::attachStorage(kanariv::contracts::Storage storage) {
    return impl_->attachStorage(std::move(storage));
}

void Engine::shutdown() {
    impl_->shutdown();
}

Engine::ExpectedVoid Engine::play(int64_t queueId) {
    return impl_->play(queueId);
}

Engine::ExpectedVoid Engine::playTrack(int64_t trackId) {
    return impl_->playTrack(trackId);
}

Engine::ExpectedVoid Engine::pause() {
    return impl_->pause();
}

Engine::ExpectedVoid Engine::resume() {
    return impl_->resume();
}

Engine::ExpectedVoid Engine::stop() {
    return impl_->stop();
}

Engine::ExpectedVoid Engine::seek(double seconds) {
    return impl_->seek(seconds);
}

Engine::ExpectedVoid Engine::suspendCurrentTrack() {
    return impl_->suspendCurrentTrack();
}

Engine::ExpectedVoid Engine::resumeAfterTagWrite(const kanariv::types::TrackRow& t, double pos,
                                                 bool wasPaused) {
    return impl_->resumeAfterTagWrite(t, pos, wasPaused);
}

Engine::ExpectedVoid Engine::setVolume(float g) {
    return impl_->setVolume(g);
}

float Engine::volume() const noexcept {
    return impl_->volume();
}

float Engine::preMuteVolume() const noexcept {
    return impl_->preMuteVolume();
}

Engine::ExpectedVoid Engine::setPreMuteVolume(float g) {
    return impl_->setPreMuteVolume(g);
}

kanariv::types::PlaybackState Engine::state() const noexcept {
    return impl_->state();
}

double Engine::duration() const noexcept {
    return impl_->duration();
}

double Engine::position() const noexcept {
    return impl_->position();
}

int64_t Engine::currentTrackId() const noexcept {
    return impl_->currentTrackId();
}

bool Engine::shuffle() const noexcept {
    return impl_->shuffle();
}

std::vector<int64_t> Engine::shufflePermFor(int64_t qid) {
    return impl_->shufflePermFor(qid);
}

void Engine::noteEnqueued(int64_t qid, std::size_t count) {
    impl_->noteEnqueued(qid, count);
}

void Engine::noteQueueMoved(int64_t qid, std::size_t from, std::size_t to) {
    impl_->noteQueueMoved(qid, from, to);
}

kanariv::types::RepeatMode Engine::repeat() const noexcept {
    return impl_->repeat();
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Engine::stats() {
    return impl_->stats();
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Engine::listHistory(int limit) {
    return impl_->listHistory(limit);
}

std::expected<void, kanariv::utils::Error> Engine::clearHistory() {
    return impl_->clearHistory();
}

Engine::ExpectedVoid Engine::setShuffle(bool on) {
    return impl_->setShuffle(on);
}

Engine::ExpectedVoid Engine::toggleShuffle() {
    return impl_->setShuffle(std::nullopt);
}

Engine::ExpectedVoid Engine::setRepeat(kanariv::types::RepeatMode m) {
    return impl_->setRepeat(m);
}

int64_t Engine::activeQueueId() const noexcept {
    return impl_->activeQueueId();
}

Engine::ExpectedVoid Engine::switchQueue(int64_t qid) {
    return impl_->switchQueue(qid);
}

Engine::ExpectedVoid Engine::next() {
    return impl_->next();
}

Engine::ExpectedVoid Engine::autoNext() {
    return impl_->autoNext();
}

Engine::ExpectedVoid Engine::prev() {
    return impl_->prev();
}

Engine::ExpectedVoid Engine::setCallbacks(const kanariv::types::EngineCallbacks& cbs) {
    return impl_->setCallbacks(cbs);
}

std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> Engine::pollEvent() {
    return impl_->pollEvent();
}

Engine::ExpectedVoid Engine::drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n) {
    return impl_->drainEvents(buf, cap, n);
}

std::vector<kanariv::types::EngineEvent> Engine::drainAll() {
    return impl_->drainAll();
}

Engine::ExpectedEngine Engine::create(const kanariv::types::EngineConfig& cfg) {
    auto e = std::unique_ptr<Engine>(new Engine(cfg));
    auto err = e->impl_->init();
    if (err)
        return std::unexpected(*err);
    if (cfg.logger)
        cfg.logger->debug("engine created monitor={}", cfg.enableMonitorThread);
    return e;
}

Engine::ExpectedVoid Engine::Impl::attachStorage(kanariv::contracts::Storage storage) {
    if (!storage.complete() || !storage.queues->isOpen())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db"));
    // loadState() writes the queue-locked state_/queue_ fields, and publishing
    // storage_ first would let a monitor tick observe (and persist) a
    // half-loaded state: take the queue lock across the attach.
    std::lock_guard<std::timed_mutex> qlk(queueMutex_);
    storage_ = std::move(storage);
    if (auto ec = loadState())
        return std::unexpected(*ec);
    return {};
}

Engine::~Engine() {
    shutdown();
}

void Engine::Impl::shutdown() {
    // stop monitor
    monRun_.store(false, std::memory_order_release);
    monCv_.notify_all();
    if (monitorThread_.joinable()) {
        monitorThread_.request_stop();
        monCv_.notify_all();
        monitorThread_.join();
    }
    // stop decode
    decodeRun_.store(false, std::memory_order_release);
    decodeCv_.notify_all();
    if (decodeThread_.joinable()) {
        decodeThread_.request_stop();
        decodeCv_.notify_all();
        decodeThread_.join();
    }
    // persist state (threads are gone; take the queue lock so saveStateLocked's
    // precondition holds and the state_ snapshot is consistent)
    if (hasDb()) {
        std::lock_guard<std::timed_mutex> qlk(queueMutex_);
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
        (void)saveStateLocked();
    }
    if (output_) {
        (void)output_->stop();
        output_.reset();
    }
    decoder_.reset();
    reader_.reset();
    ring_.reset();
    resetCrossfadeLocked();
}

Engine::ExpectedVoid Engine::Impl::play(int64_t queueId) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (queueId == 0)
        queueId = 1;
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Paused) {
        // Same trap as resume(): the paused track may have been suspended for a
        // tag write, in which case there is no decoder to start.
        if (suspended_.load(std::memory_order_acquire))
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                            "track suspended for tag write"));
        // resume, don't dequeue; playStart_/output_ are decodeMtx_ state
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            playStart_ = std::chrono::steady_clock::now();
            if (output_)
                (void)output_->start();
        }
        lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        decodeCv_.notify_all();
        monCv_.notify_all();
        return {};
    }
    if (s == kanariv::types::PlaybackState::Playing) {
        // already playing, restart current track (seek 0), don't dequeue
        // serialize with decodeLoop (decoder_->decode / ring_->write) via decodeMtx_
        std::unique_lock<std::mutex> lk(decodeMtx_);
        if (decoder_) {
            auto r = decoder_->seek(0);
            if (!r)
                return std::unexpected(r.error());
        }
        if (ring_)
            ring_->reset();
        pausePos_ = 0;
        playStart_ = std::chrono::steady_clock::now();
        resetPlaybackAccounting();
        if (output_)
            (void)output_->start();
        lk.unlock();
        decodeCv_.notify_all();
        monCv_.notify_all();
        return {};
    } // end if Playing

    // Stopped -> resume persisted track if config enabled, else start new via cursor
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("play", "queue busy");
    queue_.queue_id = queueId;
    kanariv::types::TrackRow t;
    // Resume persisted track if config enabled and track available
    if (cfg_.autoResumeOnPlay && state_.currentTrackId != 0 && state_.positionMs > 0) {
        auto idx = storage_.queues->queueIndexOf(queue_.queue_id, state_.currentTrackId);
        if (idx && *idx) {
            queue_.cursor = **idx;
            auto r = queueNextLocked(t);
            if (r) {
                auto pr = doPlayTrack(t, static_cast<double>(state_.positionMs) / 1000.0);
                if (pr) {
                    unlockQueue();
                    pushEvent(*pr);
                    return {};
                }
            }
        }
    }
    // Fall back to next track from cursor
    auto r = queueNextLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    // A restart can land on the persisted track again (single-track queue, or
    // a cursor parked on it). Start at the persisted offset instead of letting
    // the device play the head until a client-side seek arrives; an explicit
    // stop zeroes positionMs, so this never resurrects a stopped position.
    double from = 0.0;
    if (t.id == state_.currentTrackId && state_.positionMs > 0)
        from = static_cast<double>(state_.positionMs) / 1000.0;
    auto pr = doPlayTrack(t, from);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::ExpectedVoid Engine::Impl::playTrack(int64_t trackId) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (trackId <= 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad track id"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("playTrack", "queue busy");
    auto idx = storage_.queues->queueIndexOf(queue_.queue_id, trackId);
    if (!idx) {
        unlockQueue();
        return std::unexpected{idx.error()};
    }
    if (!*idx) {
        unlockQueue();
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "track not in active queue")};
    }
    size_t cursor = **idx;
    if (queue_.shuffle) {
        if (queue_.perm.empty()) {
            auto sr = setShuffleLocked(true);
            if (!sr) {
                unlockQueue();
                return std::unexpected(sr.error());
            }
        }
        bool found = false;
        for (size_t i = 0; i < queue_.perm.size(); ++i) {
            if (queue_.perm[i] == static_cast<int64_t>(**idx)) {
                cursor = i;
                found = true;
                break;
            }
        }
        if (!found) {
            unlockQueue();
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                             "track not in shuffle order")};
        }
    }
    queue_.cursor = cursor;
    kanariv::types::TrackRow t;
    auto r = queueNextLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::ExpectedVoid Engine::Impl::pause() {
    // pausePos_/output_ live under decodeMtx_ while the persisted state lives
    // under queueMutex_: take the queue lock first (documented order) so the
    // pause snapshot and saveStateLocked() cannot interleave with a concurrent
    // transport command or the monitor checkpoint.
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("pause", "queue busy");
    auto r = pauseLocked();
    unlockQueue();
    return r;
}

Engine::ExpectedVoid Engine::Impl::pauseLocked() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Playing)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not playing"));
    // record pause position (transport counters are decodeMtx_ state)
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        pausePos_ = currentPositionLocked();
        if (output_)
            (void)output_->stop();
    }
    accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    if (hasDb())
        (void)saveStateLocked();
    return {};
}

Engine::ExpectedVoid Engine::Impl::resume() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Paused)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not paused"));
    // Suspension for a tag write released the decoder: flipping to Playing here
    // would report a moving position with silence. Only resumeAfterTagWrite()
    // owns that reopen (it clears suspended_ through doPlayTrack()).
    if (suspended_.load(std::memory_order_acquire))
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                        "track suspended for tag write"));
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        playStart_ = std::chrono::steady_clock::now();
        if (output_)
            (void)output_->start();
    }
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

Engine::ExpectedVoid Engine::Impl::stop() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Stopped)
        return {};
    // The cursor rewind is part of the stop transition, not a post-hoc
    // best-effort fix-up: a stop that cannot take the queue lock fails Busy
    // (like playTrack/suspendCurrentTrack) and leaves playback untouched, so
    // the next play cannot silently skip the stopped track.
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("stop", "queue busy");
    if (s == kanariv::types::PlaybackState::Playing)
        accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
    // hasCurrent_ drops after this: the rewind below resolves the queue
    // position by identity, not by arithmetic.
    const int64_t stoppedId = currentTrackId();
    // Rewind the cursor onto the stopped track: cursor addresses the NEXT
    // entry, so a later play would otherwise skip the stopped one. Subtracting
    // one is wrong -- in shuffle mode the cursor indexes `perm`, at the end of
    // RepeatMode::One the cursor never advanced, and queue edits can leave the
    // cursor trailing some other entry -- so map the track's listing index
    // through the active permutation instead. A track no longer in the queue
    // (queue switched or edited) leaves the cursor unchanged. The rewind is
    // persisted by the single saveStateLocked() at the end of this transition.
    if (hasDb() && stoppedId != 0) {
        const int64_t rawQid = queue_.queue_id.load(std::memory_order_relaxed);
        const int64_t qid = rawQid != 0 ? rawQid : 1;
        if (auto idx = storage_.queues->queueIndexOf(qid, stoppedId); idx && *idx) {
            size_t target = **idx;
            bool resolved = true;
            if (queue_.shuffle) {
                resolved = false;
                for (size_t i = 0; i < queue_.perm.size(); ++i) {
                    if (queue_.perm[i] == static_cast<int64_t>(**idx)) {
                        target = i;
                        resolved = true;
                        break;
                    }
                }
            }
            if (resolved) {
                queue_.cursor = target;
                state_.cursorPos = static_cast<int64_t>(queue_.cursor);
            }
        }
    }
    hasCurrent_.store(false, std::memory_order_release);
    suspended_.store(false, std::memory_order_release);
    // Release playback resources (decoder/output/ring hold tens of MB for
    // large files; measured +47 MB private retained after stop). Replay
    // re-opens via doPlayTrack. decodeMtx_ serializes with an in-flight
    // decode chunk; queue-to-decodeMtx_ is the established lock order
    // (matched by doPlayTrack/suspendCurrentTrack).
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        if (output_) {
            (void)output_->stop();
            output_.reset();
        }
        decoder_.reset();
        reader_.reset();
        ring_.reset();
        resetCrossfadeLocked();
        interTrackGapArmed_.store(false, std::memory_order_release);
        pausePos_ = 0;
    }
    resetPlaybackAccounting();
    // An explicit stop starts the track from its head next time (including
    // after a restart), so clear the resume position.
    state_.positionMs = 0;
    if (hasDb())
        (void)saveStateLocked();
    unlockQueue();
    return {};
}

Engine::ExpectedVoid Engine::Impl::seek(double seconds) {
    if (!hasCurrent_.load(std::memory_order_acquire))
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no track"));
    // While suspended the decoder is gone; the seek would only move the clock
    // of a stream that cannot produce audio (same failure resume() rejects).
    if (suspended_.load(std::memory_order_acquire))
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                        "track suspended for tag write")};
    if (!std::isfinite(seconds) || seconds < 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad seconds"));

    // Queue lock first: the seek publishes a persisted position (queueMutex_
    // state) and must not be caught by pauseLocked()/engineTick mid-update.
    std::unique_lock<std::timed_mutex> qlk(queueMutex_, std::defer_lock);
    if (!qlk.try_lock_for(queueLockWait))
        return queueBusy("seek", "queue busy");
    // Pause decode thread to safely seek + reset ring (prevents race with decodeLoop)
    // A seek repositions the transport; it must not start one. The state it
    // found is restored on both the success and the failure path.
    const auto previousState = playbackState_.load(std::memory_order_acquire);
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
        // snapshot for restore on error (Task 1: seek hardening)
        double savedPause = pausePos_;
        auto savedStart = playStart_;

        if (decoder_) {
            auto res = decoder_->seek(seconds);
            if (!res) {
                pausePos_ = savedPause;
                playStart_ = savedStart;
                playbackState_.store(previousState, std::memory_order_release);
                return std::unexpected(res.error());
            }
        }

        // adjust position tracking
        double dur = duration_;
        if (seconds > dur)
            seconds = dur;
        pausePos_ = seconds;
        playStart_ = std::chrono::steady_clock::now();
        lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
        if (ring_)
            ring_->reset();

        // Resume the decode thread only if it was running before the seek.
        playbackState_.store(previousState, std::memory_order_release);
    }
    decodeCv_.notify_all();
    if (hasDb())
        (void)saveStateLocked();
    return {};
}

Engine::ExpectedVoid Engine::Impl::suspendCurrentTrack() {
    if (!hasCurrent_.load(std::memory_order_acquire)) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no track"));
    }
    if (!tryLockQueueFor(queueLockWait)) {
        return queueBusy("suspend", "queue busy");
    }
    // Mirror stop() teardown but keep cursor, current track, and history
    // mark: the file handles must go so TagLib can rewrite the file, while
    // everything needed to resume stays in place. decodeMtx_ serializes with
    // an in-flight decode chunk (like stop(); queue-to-decodeMtx_ order).
    std::unique_lock<std::mutex> lk(decodeMtx_);
    // Freeze the transport clock before the state flips: with the decoder gone
    // position() returns pausePos_, which without this snapshot would rewind to
    // the pre-track value.
    pausePos_ = currentPositionLocked();
    suspended_.store(true, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    if (output_)
        (void)output_->stop();
    if (ring_)
        ring_->reset();
    decoder_.reset();
    reader_.reset();
    resetCrossfadeLocked();
    lk.unlock();
    unlockQueue();
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

Engine::ExpectedVoid Engine::Impl::resumeAfterTagWrite(const kanariv::types::TrackRow& t,
                                                       double pos, bool wasPaused) {
    if (!tryLockQueueFor(queueLockWait)) {
        return queueBusy("resume", "queue busy");
    }
    // The user may have moved on while suspended (next/prev/play from
    // another client): the tag/db update already landed, so there is
    // nothing to reopen. Success, not an error.
    if (!hasCurrent_.load(std::memory_order_acquire) || state_.currentTrackId != t.id) {
        unlockQueue();
        return {};
    }
    bool marked = markedPlayed_.load(std::memory_order_acquire);
    uint64_t accumulated = accumulatedPlaybackMs_.load(std::memory_order_acquire);
    kanariv::types::TrackRow reopen = t;
    auto r = doPlayTrack(reopen, pos);
    markedPlayed_.store(marked, std::memory_order_release);
    accumulatedPlaybackMs_.store(accumulated, std::memory_order_release);
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    kanariv::types::EngineEvent started = *r;
    // doPlayTrack seeks before preroll and starts Playing at `pos`; only the
    // prior paused state is left to restore. The queue lock is already held,
    // so use the *Locked body instead of pause() (which would deadlock).
    if (wasPaused) {
        auto pr = pauseLocked();
        if (!pr) {
            unlockQueue();
            return std::unexpected(pr.error());
        }
    }
    unlockQueue();
    pushEvent(started);
    return {};
}

Engine::ExpectedVoid Engine::Impl::setVolume(float g) {
    g = kanariv::utils::clampVolume(g);
    volume_.store(g, std::memory_order_relaxed);
    // state_ is queueMutex_-only and output_ is decodeMtx_-only: take both in
    // the documented order. A busy queue lock is not an error -- the atomic
    // volume is already updated and saveStateLocked() persists volume_ (the
    // authoritative value), so the next checkpoint picks it up.
    std::unique_lock<std::timed_mutex> qlk(queueMutex_, std::defer_lock);
    if (!qlk.try_lock_for(queueLockWait))
        return {};
    state_.volume = g;
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        if (output_)
            output_->setVolume(g);
    }
    if (hasDb()) {
        // Throttle persistence (M-3): a volume drag emits dozens of
        // events/s; each saveStateLocked() rewrites the state sidecar.
        // Persist at most 1x/s here -- shutdown/track-change paths always
        // persist, so the crash window is <= 1 s of volume. The CAS makes the
        // claim atomic: two concurrent drags cannot both pass the window.
        const uint64_t now = detail::nowMs();
        uint64_t last = lastVolSaveMs_.load(std::memory_order_acquire);
        if (now - last >= 1000 &&
            lastVolSaveMs_.compare_exchange_strong(last, now, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            state_.cursorPos = static_cast<int64_t>(queue_.cursor);
            (void)saveStateLocked();
        }
    }
    return {};
}

float Engine::Impl::volume() const noexcept {
    return volume_.load(std::memory_order_relaxed);
}

Engine::ExpectedVoid Engine::Impl::setPreMuteVolume(float g) {
    if (!(g > 0.0f) || g > 1.0f)
        g = 0.5f;
    state_.preMuteVolume = g;
    preMute_.store(g, std::memory_order_relaxed);
    // Persisted by the setVolume() call that always follows in mute flows.
    return {};
}

float Engine::Impl::preMuteVolume() const noexcept {
    return preMute_.load(std::memory_order_relaxed);
}

kanariv::types::PlaybackState Engine::Impl::state() const noexcept {
    return playbackState_.load(std::memory_order_acquire);
}

double Engine::Impl::duration() const noexcept {
    std::lock_guard<std::mutex> lk(decodeMtx_);
    return duration_;
}

double Engine::Impl::position() const noexcept {
    std::lock_guard<std::mutex> lk(decodeMtx_);
    return currentPositionLocked();
}

int64_t Engine::Impl::currentTrackId() const noexcept {
    // state_.currentTrackId is the published id: doPlayTrack() and the
    // crossfade publication write it whenever currentTrack_ changes, so the
    // lock-free reader never touches the queue-locked struct.
    return state_.currentTrackId.load(std::memory_order_acquire);
}

bool Engine::Impl::shuffle() const noexcept {
    return queue_.shuffle.load(std::memory_order_acquire);
}

kanariv::types::RepeatMode Engine::Impl::repeat() const noexcept {
    return queue_.repeat.load(std::memory_order_acquire);
}

std::vector<int64_t> Engine::Impl::shufflePermFor(int64_t qid) {
    if (!tryLockQueue())
        return {};
    std::vector<int64_t> out;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty())
        out = queue_.perm;
    unlockQueue();
    return out;
}

void Engine::Impl::noteEnqueued(int64_t qid, std::size_t count) {
    if (count == 0 || !hasDb() || !tryLockQueue())
        return;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        auto n = storage_.queues->queueCount(qid);
        if (!n) {
            unlockQueue();
            return;
        }
        std::size_t start = (*n >= count) ? *n - count : 0;
        for (std::size_t i = start; i < *n; ++i)
            queue_.perm.push_back(static_cast<int64_t>(i));
        (void)saveStateLocked();
    }
    unlockQueue();
}

void Engine::Impl::noteQueueMoved(int64_t qid, std::size_t from, std::size_t to) {
    if (from == to || !hasDb() || !tryLockQueue())
        return;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        // The permutation lists positions, so translate old indexes to the new
        // numbering: the moved entry maps to `to`, entries it jumped over shift
        // by one in the opposite direction. The relative play order is kept.
        const auto f = static_cast<int64_t>(from);
        const auto t = static_cast<int64_t>(to);
        for (auto& p : queue_.perm) {
            if (p == f)
                p = t;
            else if (f < t && p > f && p <= t)
                --p;
            else if (t < f && p >= t && p < f)
                ++p;
        }
        (void)saveStateLocked();
    }
    unlockQueue();
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Engine::Impl::stats() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    auto ds = storage_.library->playbackStats();
    if (!ds)
        return std::unexpected(ds.error());
    return kanariv::types::PlaybackStats{
        ds->num_tracks,    ds->num_playlists, ds->num_queue_items,   ds->num_history,
        ds->num_bookmarks, ds->num_libraries, ds->total_duration_ms,
    };
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Engine::Impl::listHistory(int limit) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    kanariv::engine::History hist(storage_.history);
    return hist.listHistory(limit);
}

std::expected<void, kanariv::utils::Error> Engine::Impl::clearHistory() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    kanariv::engine::History hist(storage_.history);
    return hist.clearHistory();
}

Engine::ExpectedVoid Engine::Impl::setShuffle(std::optional<bool> on) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("setShuffle", "busy");
    // Resolve a toggle while holding the lock: reading the mode before the
    // acquisition let two toggles observe the same value and both write it.
    const bool want = on.value_or(!queue_.shuffle.load(std::memory_order_relaxed));
    auto r = setShuffleLocked(want);
    unlockQueue();
    if (!r)
        return std::unexpected(r.error());
    // push queue changed event
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::QueueChanged;
    ev.queue_id = queue_.queue_id;
    pushEvent(ev);
    return {};
}

Engine::ExpectedVoid Engine::Impl::setRepeat(kanariv::types::RepeatMode m) {
    if (m != kanariv::types::RepeatMode::Off && m != kanariv::types::RepeatMode::All &&
        m != kanariv::types::RepeatMode::One)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad repeat"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("setRepeat", "busy");
    const auto prevRepeat = queue_.repeat.load(std::memory_order_relaxed);
    const auto prevStateRepeat = state_.repeatMode.load(std::memory_order_relaxed);
    const int64_t prevCursorPos = state_.cursorPos.load(std::memory_order_relaxed);
    queue_.repeat = m;
    state_.repeatMode = m;
    state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    std::optional<kanariv::utils::Error> err;
    if (auto e = saveStateLocked(); !e) {
        err = e.error();
        // The save that reports the failure must not leave memory ahead of the
        // store: the next checkpoint would silently persist a mode this call
        // rejected.
        queue_.repeat.store(prevRepeat, std::memory_order_relaxed);
        state_.repeatMode.store(prevStateRepeat, std::memory_order_relaxed);
        state_.cursorPos.store(prevCursorPos, std::memory_order_relaxed);
    }
    unlockQueue();
    if (err)
        return std::unexpected(*err);
    return {};
}

int64_t Engine::Impl::activeQueueId() const noexcept {
    // queue_.queue_id is atomic and never published as 0 (loadState resolves
    // it to an existing queue and play/switchQueue normalize the argument).
    const int64_t qid = queue_.queue_id.load(std::memory_order_acquire);
    return qid ? qid : 1;
}

Engine::ExpectedVoid Engine::Impl::switchQueue(int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("switchQueue", "busy");
    // Validate the queue while holding queueMutex_.
    auto exists = storage_.queues->queueExists(qid);
    if (!exists || !*exists) {
        unlockQueue();
        if (!exists)
            return std::unexpected(exists.error());
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "queue not found"));
    }
    if (queue_.queue_id == qid) {
        // already active, ensure cursor consistent with DB state but not error
        unlockQueue();
        return {};
    }
    // switch active queue: reset cursor, clear shuffle perm, adopt the queue's
    // stored repeat mode, update state. Snapshot everything a failed save has
    // to put back, so memory never runs ahead of the persisted record.
    const int64_t prevQid = queue_.queue_id.load(std::memory_order_relaxed);
    const size_t prevCursor = queue_.cursor;
    auto prevPerm = std::move(queue_.perm);
    const int64_t prevActiveQueue = state_.activeQueueId.load(std::memory_order_relaxed);
    const int64_t prevCursorPos = state_.cursorPos.load(std::memory_order_relaxed);
    const auto prevRepeat = queue_.repeat.load(std::memory_order_relaxed);
    const auto prevStateRepeat = state_.repeatMode.load(std::memory_order_relaxed);
    queue_.queue_id = qid;
    queue_.cursor = 0;
    state_.activeQueueId = qid;
    state_.cursorPos = 0;
    // The queue row's repeat_mode is the per-queue setting (written when a queue
    // is created from the configured default), so it wins on switch. A lookup
    // failure keeps the current mode: switching must not fail because of it.
    if (auto target = storage_.queues->getQueue(qid); target) {
        queue_.repeat = target->repeat_mode;
        state_.repeatMode = target->repeat_mode;
    }
    // keep shuffle flag as-is but perm cleared; next shuffle will regenerate for new queue
    std::optional<kanariv::utils::Error> err;
    if (auto e = saveStateLocked(); !e) {
        err = e.error();
        queue_.queue_id.store(prevQid, std::memory_order_relaxed);
        queue_.cursor = prevCursor;
        queue_.perm = std::move(prevPerm);
        state_.activeQueueId.store(prevActiveQueue, std::memory_order_relaxed);
        state_.cursorPos.store(prevCursorPos, std::memory_order_relaxed);
        queue_.repeat.store(prevRepeat, std::memory_order_relaxed);
        state_.repeatMode.store(prevStateRepeat, std::memory_order_relaxed);
    }
    // also persist via saveState includes active_queue_id, cursor_pos, shuffle_perm cleared
    unlockQueue();
    if (err)
        return std::unexpected(*err);
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::QueueChanged;
    ev.queue_id = qid;
    pushEvent(ev);
    return {};
}

Engine::ExpectedVoid Engine::Impl::next() {
    return advanceLocked(false, static_cast<int>(queueLockWait.count()));
}

Engine::ExpectedVoid Engine::Impl::autoNext() {
    return advanceLocked(true);
}

Engine::ExpectedVoid Engine::Impl::autoNextIfEpoch(uint64_t epoch) {
    return advanceLocked(true, 0, epoch);
}

Engine::ExpectedVoid Engine::Impl::advanceLocked(bool stopAtEnd, int lockWaitMs,
                                                std::optional<uint64_t> expectedEpoch) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    // handle repeat one without shuffle without queue lock? match C: if !shuffle && repeat==One
    // && hasCurrent => seek 0 and play - serialize with decodeLoop via decodeMtx_
    if (!queue_.shuffle.load(std::memory_order_relaxed) &&
        queue_.repeat.load(std::memory_order_relaxed) == kanariv::types::RepeatMode::One &&
        hasCurrent_.load(std::memory_order_acquire)) {
        double trackDuration = 0.0;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            if (decoder_) {
                auto r = decoder_->seek(0);
                if (!r)
                    return std::unexpected(r.error());
            }
            if (ring_)
                ring_->reset();
            // transport counters are decodeMtx_ state
            pausePos_ = 0;
            playStart_ = std::chrono::steady_clock::now();
            trackDuration = duration_;
        }
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        resetPlaybackAccounting();
        markedPlayed_.store(false, std::memory_order_release);
        gaplessArmed_.store(false, std::memory_order_release);
        startedMs_.store(static_cast<int64_t>(detail::wallMs()), std::memory_order_relaxed);
        // Repeat-one rewinds the same track: it is still a transition, so the
        // epoch invalidates auto-advance decisions taken before the rewind.
        transitionEpoch_.fetch_add(1, std::memory_order_acq_rel);
        kanariv::types::EngineEvent ev;
        ev.type = kanariv::types::EngineEventType::TrackStarted;
        ev.track_id = state_.currentTrackId.load(std::memory_order_relaxed);
        ev.queue_id = queue_.queue_id.load(std::memory_order_relaxed);
        ev.duration = trackDuration;
        pushEvent(ev);
        decodeCv_.notify_all();
        return {};
    }
    // Monitor autoNext passes 0 (fail-fast; the next tick retries), user
    // next() passes the wait budget so colliding skips execute.
    bool locked =
        lockWaitMs > 0 ? tryLockQueueFor(std::chrono::milliseconds(lockWaitMs)) : tryLockQueue();
    if (!locked) {
        // Monitor autoNext fail-fast is routine (next tick retries) -- stay
        // silent; only user-wait collisions are diagnostic.
        if (lockWaitMs > 0)
            return queueBusy("next", "busy");
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "busy"));
    }
    if (expectedEpoch && transitionEpoch_.load(std::memory_order_acquire) != *expectedEpoch) {
        // Another driver already transitioned the track this decision was
        // taken for; advancing again would skip an entry.
        unlockQueue();
        return {};
    }
    if (stopAtEnd &&
        queue_.repeat.load(std::memory_order_relaxed) == kanariv::types::RepeatMode::Off) {
        // Natural track end: halt instead of wrapping. The cursor stays parked
        // past the end, so a later play wraps to the head. Manual next() still
        // wraps (same repeat mode).
        bool atEnd = false;
        if (queue_.shuffle.load(std::memory_order_relaxed)) {
            atEnd = queue_.cursor >= queue_.perm.size();
        } else {
            auto cnt = storage_.queues->queueCount(queue_.queue_id.load(std::memory_order_relaxed));
            if (!cnt) {
                unlockQueue();
                return std::unexpected(cnt.error());
            }
            atEnd = queue_.cursor >= *cnt;
        }
        if (atEnd) {
            // Natural end with repeat off: clear resume position so play()
            // doesn't seek to end-of-track and loop forever.
            state_.positionMs.store(0, std::memory_order_relaxed);
            state_.currentTrackId.store(0, std::memory_order_release);
            state_.currentPath.clear();
            hasCurrent_.store(false, std::memory_order_release);
            (void)saveStateLocked();
            const int64_t endedId = currentTrack_.id;
            const int64_t qid = queue_.queue_id.load(std::memory_order_relaxed);
            double endedDuration = 0.0;
            // audio graph + duration are decodeMtx_ state
            {
                std::unique_lock<std::mutex> lk(decodeMtx_);
                playbackState_.store(kanariv::types::PlaybackState::Stopped,
                                     std::memory_order_release);
                if (output_)
                    (void)output_->stop();
                if (ring_)
                    ring_->reset();
                pausePos_ = 0;
                endedDuration = duration_;
            }
            unlockQueue();
            // Notify listeners (e.g. the OS media-session bridge) that the
            // track ended naturally so now-playing UI can switch to Stopped.
            kanariv::types::EngineEvent ev;
            ev.type = kanariv::types::EngineEventType::TrackEnded;
            ev.track_id = endedId;
            ev.queue_id = qid;
            ev.position = endedDuration;
            ev.duration = endedDuration;
            pushEvent(ev);
            return {};
        }
    }
    kanariv::types::TrackRow t;
    auto r = queueNextLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::ExpectedVoid Engine::Impl::prev() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("prev", "busy");
    kanariv::types::TrackRow t;
    auto r = queuePrevLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::ExpectedVoid Engine::Impl::setCallbacks(const kanariv::types::EngineCallbacks& cbs) {
    std::lock_guard<std::mutex> lk(cbMutex_);
    callbacks_ = cbs;
    cfg_.callbacks = cbs;
    return {};
}

std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> Engine::Impl::pollEvent() {
    auto r = eventQueue_.pop();
    if (!r)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no event"));
    return r.value();
}

Engine::ExpectedVoid Engine::Impl::drainEvents(kanariv::types::EngineEvent* buf, size_t cap,
                                               size_t* n) {
    if (!n)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null n"));
    *n = 0;
    if (!buf && cap != 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null buf"));
    size_t got = 0;
    while (got < cap) {
        auto r = eventQueue_.pop();
        if (!r)
            break;
        buf[got++] = r.value();
    }
    *n = got;
    return {};
}

std::vector<kanariv::types::EngineEvent> Engine::Impl::drainAll() {
    std::vector<kanariv::types::EngineEvent> out;
    while (true) {
        auto r = eventQueue_.pop();
        if (!r)
            break;
        out.push_back(r.value());
    }
    return out;
}

Engine::Impl::Impl(const kanariv::types::EngineConfig& cfg) : cfg_(cfg), state_{}, queue_{} {
    cfg_.pollMs = cfg.pollMs ? cfg.pollMs : 10;
    cfg_.gaplessMs = cfg.gaplessMs ? cfg.gaplessMs : 300;
    cfg_.historyThresholdPct = cfg.historyThresholdPct ? cfg.historyThresholdPct : 60;
    cfg_.historyThresholdSecs = cfg.historyThresholdSecs ? cfg.historyThresholdSecs : 90;
    state_.volume = 1.0f;
    volume_.store(1.0f, std::memory_order_relaxed);
    state_.repeatMode = kanariv::types::RepeatMode::Off;
    queue_.repeat = kanariv::types::RepeatMode::Off;
    queue_.queue_id = 1;
    playbackState_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
    hasCurrent_.store(false, std::memory_order_release);
    markedPlayed_.store(false, std::memory_order_release);
    resetPlaybackAccounting();
    gaplessArmed_.store(false, std::memory_order_release);
    lastProgressMs_.store(0, std::memory_order_release);
    fadeSample_.store(0, std::memory_order_release);
    fadeInSamples_ = 0;
    fadeOutSamples_ = 0;
    fadeOutStart_ = 0;
    interTrackGapArmed_.store(false, std::memory_order_release);
    interTrackGapStartMs_ = 0;
    crossfadeActive_ = false;
    crossfadePending_ = false;
    crossfadeSamples_ = 0;
    crossfadeSample_ = 0;
    // callbacks from config
    callbacks_ = cfg.callbacks;
}

std::optional<kanariv::utils::Error> Engine::Impl::init() {
    // start decode thread
    decodeRun_.store(true, std::memory_order_release);
    decodeThread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });
    // start monitor thread if enabled
    if (cfg_.enableMonitorThread) {
        monRun_.store(true, std::memory_order_release);
        monitorThread_ = std::jthread([this](std::stop_token st) { monitorLoop(st); });
    }
    return std::nullopt;
}

bool Engine::Impl::hasDb() const noexcept {
    return storage_.complete() && storage_.queues->isOpen();
}

bool Engine::Impl::tryLockQueue() noexcept {
    return queueMutex_.try_lock();
}

bool Engine::Impl::tryLockQueueFor(std::chrono::milliseconds wait) noexcept {
    return queueMutex_.try_lock_for(wait);
}

void Engine::Impl::unlockQueue() noexcept {
    queueMutex_.unlock();
}

std::unexpected<kanariv::utils::Error> Engine::Impl::queueBusy(std::string_view op,
                                                               std::string_view msg) const {
    if (cfg_.logger)
        cfg_.logger->debug("queue busy op={}", op);
    return std::unexpected{
        kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, std::string(msg))};
}

double Engine::Impl::currentPositionLocked() const noexcept {
    if (!hasCurrent_.load(std::memory_order_acquire))
        return static_cast<double>(state_.positionMs) / 1000.0;
    auto st = playbackState_.load(std::memory_order_acquire);
    if (st == kanariv::types::PlaybackState::Paused)
        return pausePos_;
    if (st != kanariv::types::PlaybackState::Playing)
        return pausePos_;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - playStart_).count();
    double pos = pausePos_ + elapsed;
    if (pos > duration_)
        pos = duration_;
    if (pos < 0)
        pos = 0;
    return pos;
}

std::optional<kanariv::utils::Error> Engine::Impl::loadState() {
    if (!hasDb())
        return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db");
    auto loaded = storage_.state->load();
    if (!loaded)
        return loaded.error();
    state_.shuffleEnabled.store(loaded->shuffle_enabled ? 1 : 0, std::memory_order_relaxed);
    state_.repeatMode.store(loaded->repeat_mode, std::memory_order_relaxed);
    state_.cursorPos.store(loaded->cursor_pos, std::memory_order_relaxed);
    state_.volume.store(loaded->volume, std::memory_order_relaxed);
    state_.preMuteVolume.store(loaded->pre_mute_volume, std::memory_order_relaxed);
    state_.positionMs.store(loaded->position_ms, std::memory_order_relaxed);
    volume_.store(loaded->volume, std::memory_order_relaxed);
    preMute_.store(loaded->pre_mute_volume, std::memory_order_relaxed);
    queue_.perm = std::move(loaded->shuffle_perm);
    queue_.shuffle.store(loaded->shuffle_enabled, std::memory_order_relaxed);
    queue_.repeat.store(loaded->repeat_mode, std::memory_order_relaxed);
    const int64_t cursor = loaded->cursor_pos < 0 ? 0 : loaded->cursor_pos;
    queue_.cursor = static_cast<size_t>(cursor);
    // The record names its queue instead of storing a row id, so a database
    // rebuild or import finds it again. An unknown name falls back to default.
    int64_t qid = 1;
    if (const auto queues = storage_.queues->listQueues(); queues) {
        for (const auto& q : *queues) {
            if (q.name == loaded->queue_name) {
                qid = q.id;
                break;
            }
        }
    }
    queue_.queue_id.store(qid, std::memory_order_relaxed);
    state_.activeQueueId.store(qid, std::memory_order_relaxed);
    auto queueExists = storage_.queues->queueExists(qid);
    if (!queueExists || !*queueExists) {
        queue_.queue_id.store(1, std::memory_order_relaxed);
        state_.activeQueueId.store(1, std::memory_order_relaxed);
    }
    // A persisted permutation holds queue *indices*, so it is only meaningful
    // for the queue it was written for. If the queue shrank or was replaced
    // while the daemon was down, drop it and restart at the head instead of
    // fetching removed entries (or failing the first play with NotFound).
    if (auto counted = storage_.queues->queueCount(queue_.queue_id.load(std::memory_order_relaxed));
        counted) {
        const std::size_t queueLen = *counted;
        if (!queue_.shuffle.load(std::memory_order_relaxed)) {
            queue_.perm.clear();
        } else if (queue_.perm.size() != queueLen) {
            queue_.perm.clear();
            queue_.cursor = 0;
        }
        if (queue_.cursor >= (queue_.perm.empty() ? queueLen : queue_.perm.size()))
            queue_.cursor = 0;
    } else if (!queue_.perm.empty() && queue_.cursor >= queue_.perm.size()) {
        queue_.cursor = 0;
    }
    // The optional id is a hint; the path is the identity.
    int64_t trackId = 0;
    std::string trackPath = loaded->current_path;
    if (loaded->current_track_id) {
        if (auto track = storage_.tracks->getTrack(*loaded->current_track_id); track) {
            trackId = track->id;
            trackPath = track->path;
        }
    }
    if (trackId <= 0 && !trackPath.empty()) {
        if (auto track = storage_.tracks->findByPath(trackPath); track) {
            trackId = track->id;
            trackPath = track->path;
        }
    }
    state_.currentTrackId.store(trackId, std::memory_order_release);
    state_.currentPath = std::move(trackPath);
    return std::nullopt;
}

std::expected<void, kanariv::utils::Error>
Engine::Impl::saveStateLocked(std::optional<int64_t> livePositionMs) {
    // @pre queueMutex_ held: state_/queue_.perm are queue-lock-only. The live
    // position needs decodeMtx_ (transport counters) when the caller cannot
    // supply it, so take it briefly then -- callers must not already hold it.
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    kanariv::types::EngineState persisted;
    persisted.queue_name = "default";
    if (auto queue = storage_.queues->getQueue(queue_.queue_id.load(std::memory_order_relaxed));
        queue && !queue->name.empty())
        persisted.queue_name = queue->name;
    persisted.cursor_pos = static_cast<int64_t>(queue_.cursor);
    persisted.current_path = state_.currentPath;
    const int64_t trackId = state_.currentTrackId.load(std::memory_order_relaxed);
    if (trackId > 0)
        persisted.current_track_id = trackId;
    persisted.shuffle_enabled = queue_.shuffle.load(std::memory_order_relaxed);
    persisted.repeat_mode = queue_.repeat.load(std::memory_order_relaxed);
    persisted.shuffle_perm = queue_.perm;
    // volume_ is the authoritative value: setVolume() may skip the queue lock
    // while it is contended, and the atomic is still current.
    persisted.volume = volume_.load(std::memory_order_relaxed);
    persisted.pre_mute_volume = preMute_.load(std::memory_order_relaxed);
    if (livePositionMs) {
        persisted.position_ms = *livePositionMs;
    } else {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        persisted.position_ms = hasCurrent_.load(std::memory_order_acquire)
                                    ? static_cast<int64_t>(currentPositionLocked() * 1000.0)
                                    : state_.positionMs.load(std::memory_order_relaxed);
    }
    if (persisted.position_ms < 0)
        persisted.position_ms = 0;
    auto result = storage_.state->save(persisted);
    if (result) {
        state_.positionMs = persisted.position_ms;
        state_.shuffleEnabled.store(persisted.shuffle_enabled ? 1 : 0, std::memory_order_relaxed);
        state_.repeatMode.store(persisted.repeat_mode, std::memory_order_relaxed);
        state_.cursorPos.store(persisted.cursor_pos, std::memory_order_relaxed);
        state_.activeQueueId.store(queue_.queue_id.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
    return result;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Engine::Impl::fetchTrackByPosLocked(int64_t qid, int64_t pos) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    return storage_.queues->queueTrackAt(qid, pos);
}

std::expected<void, kanariv::utils::Error> Engine::Impl::setShuffleLocked(bool on) {
    // assert: queueMutex_ locked by caller
    bool want = on;
    if (queue_.shuffle.load(std::memory_order_relaxed) == want && !queue_.perm.empty())
        return {};
    // Snapshot the fields each branch rewrites; a failed persist puts them
    // back so the in-memory order never diverges from the stored one.
    const bool prevShuffle = queue_.shuffle.load(std::memory_order_relaxed);
    const size_t prevCursor = queue_.cursor;
    auto prevPerm = std::move(queue_.perm);
    const int prevShuffleEnabled = state_.shuffleEnabled.load(std::memory_order_relaxed);
    auto rollback = [&] {
        queue_.perm = std::move(prevPerm);
        queue_.cursor = prevCursor;
        queue_.shuffle.store(prevShuffle, std::memory_order_relaxed);
        state_.shuffleEnabled.store(prevShuffleEnabled, std::memory_order_relaxed);
    };
    if (want) {
        queue_.cursor = 0;
        auto cnt = storage_.queues->queueCount(queue_.queue_id.load(std::memory_order_relaxed));
        if (!cnt) {
            rollback();
            return std::unexpected(cnt.error());
        }
        if (*cnt == 0) {
            queue_.shuffle.store(true, std::memory_order_relaxed);
            queue_.cursor = 0;
            auto r = saveStateLocked();
            if (!r) {
                rollback();
                return r;
            }
            state_.shuffleEnabled.store(1, std::memory_order_relaxed);
            return {};
        }
        queue_.perm.resize(*cnt);
        for (size_t i = 0; i < *cnt; ++i)
            queue_.perm[i] = static_cast<int64_t>(i);
        {
            std::mt19937 rng{std::random_device{}()};
            detail::shufflePerm(queue_.perm, rng);
        }
        queue_.shuffle.store(true, std::memory_order_relaxed);
        queue_.cursor = 0;
        auto r = saveStateLocked();
        if (!r) {
            rollback();
            return r;
        }
        state_.shuffleEnabled.store(1, std::memory_order_relaxed);
        return {};
    } else {
        queue_.cursor = 0;
        queue_.shuffle.store(false, std::memory_order_relaxed);
        auto r = saveStateLocked();
        if (!r) {
            rollback();
            return r;
        }
        state_.shuffleEnabled.store(0, std::memory_order_relaxed);
        return {};
    }
}

QueueFingerprint Engine::Impl::queueFingerprintLocked() const {
    QueueFingerprint fp;
    fp.queueId = queue_.queue_id.load(std::memory_order_relaxed);
    fp.shuffle = queue_.shuffle.load(std::memory_order_relaxed);
    fp.repeat = queue_.repeat.load(std::memory_order_relaxed);
    fp.cursor = queue_.cursor;
    fp.perm = queue_.perm;
    return fp;
}

std::expected<Engine::Impl::PeekedNext, kanariv::utils::Error> Engine::Impl::peekNextLocked() {
    const int64_t rawQid = queue_.queue_id.load(std::memory_order_relaxed);
    const int64_t qid = rawQid != 0 ? rawQid : 1;
    const bool shuffle = queue_.shuffle.load(std::memory_order_relaxed);
    // planNext performs no I/O; resolve the one item count it may need here so
    // a backend failure surfaces as its own error instead of a wrong plan.
    std::size_t count = 0;
    if (!shuffle || queue_.perm.empty()) {
        auto counted = storage_.queues->queueCount(qid);
        if (!counted)
            return std::unexpected(counted.error());
        count = *counted;
    }
    auto plan = detail::planNext(
        shuffle, queue_.repeat.load(std::memory_order_relaxed), queue_.cursor,
        std::span<const int64_t>(queue_.perm.data(), queue_.perm.size()), [count] { return count; },
        [](std::size_t n) {
            std::vector<int64_t> perm(n);
            for (std::size_t i = 0; i < n; ++i)
                perm[i] = static_cast<int64_t>(i);
            detail::shufflePerm(perm);
            return perm;
        });
    if (!plan)
        return std::unexpected(plan.error());
    auto tr = fetchTrackByPosLocked(qid, plan->position);
    if (!tr)
        return std::unexpected(tr.error());
    PeekedNext out;
    out.track = std::move(tr.value());
    out.plan = std::move(*plan);
    out.queueId = qid;
    return out;
}

bool Engine::Impl::commitNextLocked(const QueueFingerprint& expected, PeekedNext&& next,
                                    std::optional<int64_t> livePositionMs) {
    if (!(queueFingerprintLocked() == expected))
        return false;
    queue_.queue_id.store(next.queueId, std::memory_order_relaxed);
    if (next.plan.newPerm) {
        // First shuffle use: install the permutation the peek generated and
        // its head as consumed. The blob write persists perm+cursor at once.
        queue_.perm = std::move(*next.plan.newPerm);
        queue_.shuffle.store(true, std::memory_order_relaxed);
        queue_.cursor = next.plan.cursor;
        (void)saveStateLocked(livePositionMs);
        return true;
    }
    if (queue_.cursor == next.plan.cursor)
        return true; // RepeatMode::One replay: the cursor must not move.
    queue_.cursor = next.plan.cursor;
    (void)saveStateLocked(livePositionMs);
    return true;
}

std::expected<void, kanariv::utils::Error>
Engine::Impl::queueNextLocked(kanariv::types::TrackRow& out) {
    auto peeked = peekNextLocked();
    if (!peeked)
        return std::unexpected(peeked.error());
    // No other writer can run while queueMutex_ is held, so the fingerprint
    // captured here can never be stale and the commit cannot be rejected.
    const QueueFingerprint current = queueFingerprintLocked();
    out = std::move(peeked->track);
    (void)commitNextLocked(current, std::move(*peeked));
    return {};
}

std::expected<void, kanariv::utils::Error>
Engine::Impl::queuePrevLocked(kanariv::types::TrackRow& out) {
    const int64_t qid = queue_.queue_id.load(std::memory_order_relaxed);
    if (!queue_.shuffle.load(std::memory_order_relaxed)) {
        auto counted = storage_.queues->queueCount(qid);
        if (!counted)
            return std::unexpected(counted.error());
        const size_t cnt = *counted;
        if (cnt == 0)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
        if (queue_.cursor <= 1) {
            if (queue_.cursor == 0)
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "at start"));
            queue_.cursor = 0;
        } else {
            queue_.cursor -= 2;
            if (queue_.cursor >= cnt)
                queue_.cursor = cnt - 1;
        }
        auto tr = fetchTrackByPosLocked(qid, static_cast<int64_t>(queue_.cursor));
        if (!tr)
            return std::unexpected(tr.error());
        queue_.cursor++;
        (void)saveStateLocked();
        out = tr.value();
        return {};
    }
    if (queue_.perm.empty())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no perm"));
    if (queue_.cursor <= 1) {
        if (queue_.cursor == 0)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "at start"));
        queue_.cursor = 0;
    } else {
        queue_.cursor -= 2;
        if (queue_.cursor >= queue_.perm.size())
            queue_.cursor = queue_.perm.size() - 1;
    }
    int64_t pos = queue_.perm[queue_.cursor];
    queue_.cursor++;
    (void)saveStateLocked();
    auto tr = fetchTrackByPosLocked(qid, pos);
    if (!tr)
        return std::unexpected(tr.error());
    out = tr.value();
    return {};
}

std::expected<kanariv::types::EngineEvent, kanariv::utils::Error>
Engine::Impl::doPlayTrack(kanariv::types::TrackRow& t, double startSec) {
    // try to open decoder path
    std::string path = t.path;
    if (!std::isfinite(startSec) || startSec < 0.0)
        startSec = 0.0;
    // lastErr_ describes *this* attempt's open failure; without the reset a
    // failure of a different kind surfaced the previous track's message.
    lastErr_.clear();
    // Teardown and reopen must serialize with decodeLoop (decoder_->decode /
    // ring_->write hold decodeMtx_) and with the audio callback, which keeps
    // cfg.ring raw. Freeing those without the lock is a use-after-free (ASan:
    // convertFrameBuffered while doPlayTrack reset decoder_); stopping the
    // device first also quiesces the callback before the ring goes away.
    // Lock order queue-to-decodeMtx_ matches stop()/suspendCurrentTrack();
    // every caller already holds the queue lock.
    std::unique_lock<std::mutex> lk(decodeMtx_);
    if (output_) {
        (void)output_->stop();
        output_.reset();
    }
    decoder_.reset();
    reader_.reset();
    ring_.reset();
    // A track change cancels any armed/in-flight crossfade; otherwise the
    // stale preloaded decoder would keep mixing into the new track and later
    // be published over it.
    resetCrossfadeLocked();

    // attempt to open reader & decoder; without allowSimulatedPlayback an
    // unplayable track is an error, never silent timer playback
    bool opened = false;
    if (!path.empty()) {
        // TrackRow paths are stored UTF-8; decode them as UTF-8 rather than the
        // Windows ANSI code page so non-ASCII filenames resolve.
        auto rRes = kanariv::media::FileReader::open(kanariv::utils::detail::pathFromUtf8(path));
        if (rRes) {
            reader_ = std::move(rRes.value());
            auto dRes = kanariv::media::Decoder::open(*reader_, cfg_.logger);
            if (dRes) {
                decoder_ = std::move(dRes.value());
                opened = true;
                duration_ = decoder_->totalFrames()
                                ? static_cast<double>(decoder_->totalFrames()) / static_cast<double>(decoder_->sampleRate())
                                : t.duration;
                if (duration_ <= 0)
                    duration_ = t.duration > 0 ? t.duration : 1.0;
                // Resume: seek before the ring exists and is prerolled, so the
                // device starts at the requested offset. Seeking after
                // output_->start() leaks the ring's preroll (4096 frames,
                // ~93 ms) of the track head into playback.
                if (startSec > 0.0) {
                    double target = startSec;
                    if (duration_ > 0 && target > duration_)
                        target = duration_;
                    auto sRes = decoder_->seek(target);
                    if (!sRes) {
                        if (cfg_.logger)
                            cfg_.logger->warn("resume seek failed file={} pos={} err={}", path,
                                              target, sRes.error().message);
                        startSec = 0.0;
                    } else if (cfg_.logger) {
                        cfg_.logger->debug("resume seek pos={} file={}", target, path);
                    }
                }
                // create ring and output; engineRingFrames() carries the
                // measured reason this path does not use Player's flat cap.
                uint32_t sr = decoder_->sampleRate();
                uint32_t ch = decoder_->channels();
                if (sr == 0)
                    sr = 48000;
                if (ch == 0)
                    ch = 2;
                ring_ = std::make_shared<kanariv::utils::SpscRing<float>>(
                    playback::policy::engineRingFrames(ch), ch);
                kanariv::player::AudioOutput::Config cfg;
                cfg.sampleRate = sr;
                cfg.channels = ch;
                cfg.ring = ring_.get();
                // ReplayGain: apply track/album gain (dB) as a volume multiplier
                // on top of the user volume. dB -> linear: 10^(dB/20). Nothing
                // populates the gain columns yet (no tag importer), so gainDb
                // stays 0 and this is currently a no-op (M10).
                float gain = volume_.load(std::memory_order_relaxed);
                if (cfg_.replayGain != kanariv::types::ReplayGainMode::Off) {
                    double gainDb = 0.0;
                    if (cfg_.replayGain == kanariv::types::ReplayGainMode::Track)
                        gainDb = t.replayGainTrack;
                    else if (cfg_.replayGain == kanariv::types::ReplayGainMode::Album)
                        gainDb = t.replayGainAlbum;
                    if (gainDb != 0.0)
                        gain = static_cast<float>((gain * std::pow(10.0, gainDb / 20.0)));
                }
                cfg.volume = gain;
                cfg.logger = cfg_.logger;
                cfg.deviceId = cfg_.device;
                cfg.deviceName = cfg_.deviceName;
                auto oRes = kanariv::player::AudioOutput::create(cfg);
                if (!oRes && cfg_.allowSimulatedPlayback) {
                    // Headless fallback: `allowSimulatedPlayback` already means
                    // "this process may run without a device", so retry once on
                    // miniaudio's silent null backend. That keeps the real
                    // decode/ring path (and therefore gapless/crossfade, which
                    // need a ring consumer to progress) alive instead of
                    // degrading straight to timer-only playback.
                    cfg.nullBackend = true;
                    oRes = kanariv::player::AudioOutput::create(cfg);
                }
                if (oRes) {
                    output_ = std::move(oRes.value());
                    // Initialize fade state for this track.
                    fadeSample_.store(0, std::memory_order_release);
                    fadeInSamples_ = 0;
                    fadeOutSamples_ = 0;
                    fadeOutStart_ = 0;
                    if (cfg_.fadeInMs > 0 && sr > 0)
                        fadeInSamples_ = static_cast<uint64_t>(cfg_.fadeInMs) * sr / 1000;
                    if (cfg_.fadeOutMs > 0 && sr > 0 && duration_ > 0) {
                        fadeOutSamples_ = static_cast<uint64_t>(cfg_.fadeOutMs) * sr / 1000;
                        fadeOutStart_ = duration_ - static_cast<double>(cfg_.fadeOutMs) / 1000.0;
                        if (fadeOutStart_ < 0.0)
                            fadeOutStart_ = 0.0;
                    }
                    // preroll: decode some frames before start
                    preroll();
                    (void)output_->start();
                } else {
                    lastErr_ = oRes.error().message;
                }
            } else {
                lastErr_ = dRes.error().message;
            }
        } else {
            lastErr_ = rRes.error().message;
        }
    }
    if (!opened) {
        if (!cfg_.allowSimulatedPlayback) {
            const std::string msg =
                lastErr_.empty() ? "track file not playable: " + path : lastErr_;
            if (cfg_.logger)
                cfg_.logger->warn("track not playable file={} err={}", path, msg);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, msg)};
        }
        // timer-only fallback to track metadata duration (no audio output)
        if (cfg_.logger)
            cfg_.logger->warn("simulated playback (no audio) file={}", path);
        duration_ = t.duration > 0 ? t.duration : 1.0;
        // no ring/output needed for simulated playback
    } else if (!output_) {
        // decoder opened but no audio device (Player::openReader fails here
        // with Device); timer fallback only when explicitly opted in
        if (!cfg_.allowSimulatedPlayback) {
            decoder_.reset();
            reader_.reset();
            ring_.reset();
            const std::string msg = lastErr_.empty() ? "audio device unavailable" : lastErr_;
            if (cfg_.logger)
                cfg_.logger->warn("audio device unavailable file={} err={}", path, msg);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Device, msg)};
        }
    }
    currentTrack_ = t;
    // update state
    hasCurrent_.store(true, std::memory_order_release);
    markedPlayed_.store(false, std::memory_order_release);
    gaplessArmed_.store(false, std::memory_order_release);
    // The new track owns the next transition: a gap half-elapsed on the
    // outgoing track must not shorten (or skip) its own inter-track silence.
    interTrackGapArmed_.store(false, std::memory_order_release);
    // Start each track with a fresh accounting window. Without this the time
    // accumulated on the previous track carried over, so the next track was
    // marked played immediately on entry (and the skip/history window was
    // wrong). resumeAfterTagWrite() saves and restores the counters around
    // doPlayTrack(), so a tag write mid-track still keeps its progress.
    resetPlaybackAccounting();
    startedMs_.store(static_cast<int64_t>(detail::wallMs()), std::memory_order_relaxed);
    state_.currentTrackId.store(t.id, std::memory_order_release);
    state_.currentPath = t.path;
    state_.cursorPos.store(static_cast<int64_t>(queue_.cursor), std::memory_order_relaxed);
    // A reopened decoder ends any tag-write suspension (resumeAfterTagWrite).
    suspended_.store(false, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    // This track owns the transition now; auto-advance decisions taken for the
    // previous track carry the old epoch and must not move the queue again.
    transitionEpoch_.fetch_add(1, std::memory_order_acq_rel);
    // Position tracking starts at the resumed offset (0 for next/prev);
    // saveStateLocked() below then persists that offset instead of a transient
    // 0. pausePos_ already holds the offset, so playStart_ must be "now" --
    // shifting it back too would count the offset twice.
    pausePos_ = startSec;
    playStart_ = std::chrono::steady_clock::now();
    const double trackDuration = duration_;
    // saveStateLocked() takes decodeMtx_ itself, so it must run unlocked (the
    // queue lock the caller holds is what it requires).
    lk.unlock();
    if (hasDb())
        (void)saveStateLocked();
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::TrackStarted;
    ev.track_id = t.id;
    ev.queue_id = queue_.queue_id.load(std::memory_order_relaxed);
    ev.duration = trackDuration;
    ev.position = 0;
    decodeCv_.notify_all();
    monCv_.notify_all();
    if (cfg_.logger)
        cfg_.logger->debug("playing track={} file={}", t.id, path);
    // The callers publish this after releasing the queue lock: pushEvent runs
    // the media-session callbacks synchronously (E8).
    return ev;
}

void Engine::Impl::preroll() {
    if (!decoder_ || !ring_)
        return;
    uint32_t ch = decoder_->channels();
    if (ch == 0)
        ch = 2;
    std::size_t need = playback::policy::prerollFrames(ring_->capacity());
    if (need == 0)
        return;
    std::size_t chunkFrames = playback::policy::chunkCapFrames(ch);
    std::vector<float> tmp(chunkFrames * ch);
    while (need > 0 && ring_->availableWrite() >= chunkFrames) {
        size_t frames = decoder_->decode(std::span<float>(tmp.data(), tmp.size()));
        if (frames == 0)
            break;
        size_t samples = frames * ch;
        size_t writtenFrames = ring_->write(std::span<float>(tmp.data(), samples));
        if (writtenFrames < frames)
            break;
        if (need > frames)
            need -= frames;
        else
            break;
    }
}

void Engine::Impl::pushEvent(const kanariv::types::EngineEvent& ev) {
    // The bounded event queue is a best-effort stream for pollEvent/drainEvents
    // consumers. Nothing in the daemon drains it, so periodic Progress samples
    // fill all 64 slots after ~30 s of playback. Progress is only enqueued
    // while there is spare capacity so state transitions are not crowded out.
    const bool isProgress = ev.type == kanariv::types::EngineEventType::Progress;
    if (!isProgress || eventQueue_.size() < eventQueue_.capacity() / 2)
        (void)eventQueue_.push(ev);

    // Callbacks are the in-process notification channel (the daemon's OS
    // media-session bridge, history, ...). They must fire regardless of the
    // diagnostic queue state: tying dispatch to a successful push would
    // silently swallow every track transition once the queue saturated, which
    // left the OS now-playing UI stuck on the previous track.
    kanariv::types::EngineCallbacks cbsCopy;
    {
        std::lock_guard<std::mutex> lk(cbMutex_);
        cbsCopy = callbacks_;
    }
    if (ev.type == kanariv::types::EngineEventType::TrackStarted && cbsCopy.on_track_started) {
        cbsCopy.on_track_started(ev.track_id);
    } else if (ev.type == kanariv::types::EngineEventType::TrackEnded && cbsCopy.on_track_ended) {
        double pct = 0;
        if (ev.duration > 0) {
            pct = (ev.position / ev.duration) * 100;
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
        }
        cbsCopy.on_track_ended(ev.track_id, pct);
    } else if (ev.type == kanariv::types::EngineEventType::QueueChanged &&
               cbsCopy.on_queue_changed) {
        cbsCopy.on_queue_changed(ev.queue_id);
    } else if (ev.type == kanariv::types::EngineEventType::Error && cbsCopy.on_error) {
        cbsCopy.on_error(kanariv::utils::StatusCode::Internal, ev.msg);
    }
}

void Engine::Impl::resetPlaybackAccounting() noexcept {
    accumulatedPlaybackMs_.store(0, std::memory_order_release);
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
}

void Engine::Impl::accountPlaybackTime() noexcept {
    const uint64_t now = detail::nowMs();
    const uint64_t last = lastPlaybackSampleMs_.exchange(now, std::memory_order_acq_rel);
    if (last == 0 || now < last)
        return;
    accumulatedPlaybackMs_.fetch_add(now - last, std::memory_order_acq_rel);
}

void Engine::Impl::doHistoryMark(const TickSnapshot& snap) {
    if (!hasDb() || !snap.hasCurrent)
        return;
    if (markedPlayed_.load(std::memory_order_acquire))
        return;
    const double dur = snap.duration;
    const double played =
        static_cast<double>(accumulatedPlaybackMs_.load(std::memory_order_acquire)) / 1000.0;
    int pct = cfg_.historyThresholdPct ? cfg_.historyThresholdPct : 60;
    int secs = cfg_.historyThresholdSecs ? cfg_.historyThresholdSecs : 90;
    if (!detail::shouldMarkPlayed(dur, played, false, pct, secs))
        return;
    // The snapshot is a few instructions old; a track change in between would
    // leave the new track's mark flag set (it is reset on entry) and history
    // would never record it. Re-check the published id first.
    if (state_.currentTrackId.load(std::memory_order_acquire) != snap.trackId)
        return;
    bool expected = false;
    bool desired = true;
    // CAS exactly-once
    bool exchanged = markedPlayed_.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    if (!exchanged)
        return;
    int64_t completion = static_cast<int64_t>(detail::wallMs());
    double completionPct = 0.0;
    const double pos = snap.position;
    if (dur > 0.0) {
        completionPct = (pos / dur) * 100.0;
        if (completionPct > 100.0)
            completionPct = 100.0;
    }
    auto result = storage_.history->markPlayed(snap.trackId, snap.startedMs, completion,
                                               static_cast<int64_t>(pos * 1000.0), completionPct,
                                               snap.queueId ? snap.queueId : 1);
    if (!result)
        markedPlayed_.store(false, std::memory_order_release);
}

Engine::Impl::TickSnapshot Engine::Impl::snapshotLocked() const {
    // @pre queueMutex_ held. decodeMtx_ is taken inside (queue-to-decode order,
    // never the reverse) to read the transport counters.
    TickSnapshot snap;
    snap.hasCurrent = hasCurrent_.load(std::memory_order_acquire);
    snap.trackId = state_.currentTrackId.load(std::memory_order_acquire);
    snap.queueId = queue_.queue_id.load(std::memory_order_relaxed);
    snap.startedMs = startedMs_.load(std::memory_order_relaxed);
    snap.transitionEpoch = transitionEpoch_.load(std::memory_order_acquire);
    snap.playback = playbackState_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        snap.duration = duration_;
        snap.position = currentPositionLocked();
    }
    return snap;
}

void Engine::Impl::engineTick() {
    if (playbackState_.load(std::memory_order_acquire) == kanariv::types::PlaybackState::Playing)
        accountPlaybackTime();
    const uint64_t now = detail::nowMs();
    // One queue-lock section builds the snapshot and runs the 5 s checkpoint;
    // history, event dispatch and the advance decisions then run lock-free, so
    // no callback and no database write happens under the queue lock. The
    // monitor is fail-fast: a contended tick is skipped and retried.
    if (!tryLockQueue())
        return;
    const TickSnapshot snap = snapshotLocked();
    if (snap.playback == kanariv::types::PlaybackState::Playing) {
        const uint64_t lastSave = lastPosSaveMs_.load(std::memory_order_acquire);
        if (now - lastSave >= 5000) {
            state_.cursorPos.store(static_cast<int64_t>(queue_.cursor), std::memory_order_relaxed);
            if (saveStateLocked())
                lastPosSaveMs_.store(now, std::memory_order_release);
        }
    }
    unlockQueue();

    doHistoryMark(snap);
    if (snap.playback != kanariv::types::PlaybackState::Playing)
        return;
    const uint64_t lastProgress = lastProgressMs_.load(std::memory_order_acquire);
    if (now - lastProgress >= 500) {
        lastProgressMs_.store(now, std::memory_order_release);
        kanariv::types::EngineEvent ev;
        ev.type = kanariv::types::EngineEventType::Progress;
        ev.track_id = snap.trackId;
        ev.queue_id = snap.queueId ? snap.queueId : 1;
        ev.position = snap.position;
        ev.duration = snap.duration;
        pushEvent(ev);
    }
    if (snap.duration > 0) {
        const double remaining = snap.duration - snap.position;
        const double gapS = static_cast<double>(cfg_.gaplessMs) / 1000.0;
        const double xfS = static_cast<double>(cfg_.crossfadeMs) / 1000.0;
        if (cfg_.crossfadeMs > 0 && remaining <= xfS && remaining >= 0.0 &&
            !crossfadeActive_.load(std::memory_order_acquire) &&
            !crossfadePending_.load(std::memory_order_acquire) && snap.hasCurrent) {
            // Arm crossfade: decodeLoop will load the next track into
            // crossfadeDecoder_ and mix it with the outgoing track.
            crossfadePending_.store(true, std::memory_order_release);
        } else if (cfg_.crossfadeMs == 0 && cfg_.interTrackGapMs == 0 && remaining <= gapS &&
                   remaining >= 0.0) {
            // Gapless pre-roll. A configured gap must not pre-empt itself here:
            // the track has to reach EOF for the silence to start.
            if (snap.hasCurrent) {
                // gaplessArmed 0->1 CAS, 300ms preroll
                bool expected = false;
                if (gaplessArmed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
                    auto nr = autoNextIfEpoch(snap.transitionEpoch);
                    if (!nr)
                        gaplessArmed_.store(false, std::memory_order_release);
                }
            }
        }
    }
}

bool Engine::Impl::tryPublishCrossfade() {
    // @pre decode thread, neither mutex held (see the declaration).
    // Queue lock first (documented order queueMutex_ -> decodeMtx_), bounded:
    // a contended queue is not an error, the decode loop retries because
    // crossfadeActive_ is still set.
    std::unique_lock<std::timed_mutex> qlk(queueMutex_, std::defer_lock);
    if (!qlk.try_lock_for(crossfadePublishWait))
        return false;

    double trackDuration = 0.0;
    int64_t trackId = 0;
    double sampleRate = 0.0;
    std::string trackPath;
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        if (!crossfadeActive_.load(std::memory_order_acquire) || !crossfadeDecoder_) {
            return false; // someone else completed or cancelled the transition
        }
        // Read the incoming track's metadata while it is still crossfadeDecoder_:
        // the swap below moves it into decoder_ (same decode-lock ownership).
        sampleRate = static_cast<double>(crossfadeDecoder_->sampleRate());
        const uint64_t frames = crossfadeDecoder_->totalFrames();
        if (sampleRate > 0.0 && frames > 0)
            trackDuration = static_cast<double>(frames) / sampleRate;
        if (trackDuration <= 0.0)
            trackDuration = crossfadeTrack_.duration;
        trackId = crossfadeTrack_.id;
        trackPath = crossfadeTrack_.path;

        decoder_ = std::move(crossfadeDecoder_);
        reader_ = std::move(crossfadeReader_);
        currentTrack_ = std::move(crossfadeTrack_);
        crossfadeTrack_ = {};
        duration_ = trackDuration;
        // The new track starts its own transport window at position 0; the
        // decoder itself continues where the overlap stopped.
        pausePos_ = 0;
        playStart_ = std::chrono::steady_clock::now();
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        // Fade envelope for the newly published track (decodeMtx_ state): no
        // fade-in (we are mid-track), normal fade-out near its end.
        fadeSample_.store(0, std::memory_order_release);
        fadeInSamples_ = 0;
        fadeOutSamples_ = 0;
        fadeOutStart_ = 0;
        if (cfg_.fadeOutMs > 0 && sampleRate > 0.0 && trackDuration > 0.0) {
            fadeOutSamples_ = static_cast<uint64_t>(cfg_.fadeOutMs) * static_cast<uint64_t>(sampleRate) / 1000;
            fadeOutStart_ = trackDuration - static_cast<double>(cfg_.fadeOutMs) / 1000.0;
            if (fadeOutStart_ < 0.0)
                fadeOutStart_ = 0.0;
        }
        crossfadeActive_.store(false, std::memory_order_release);
        crossfadePending_.store(false, std::memory_order_release);
        crossfadeSamples_ = 0;
        crossfadeSample_ = 0;
    }

    // Queue-locked publication (no decode lock held here: saveStateLocked()
    // takes decodeMtx_ itself and no callback runs under the queue lock).
    hasCurrent_.store(true, std::memory_order_release);
    markedPlayed_.store(false, std::memory_order_release);
    gaplessArmed_.store(false, std::memory_order_release);
    startedMs_.store(static_cast<int64_t>(detail::wallMs()), std::memory_order_relaxed);
    // The publication is a transition like any other: an auto-advance decision
    // taken for the outgoing track must not move the queue past the new one.
    transitionEpoch_.fetch_add(1, std::memory_order_acq_rel);
    state_.currentTrackId.store(trackId, std::memory_order_release);
    state_.currentPath = std::move(trackPath);
    state_.cursorPos.store(static_cast<int64_t>(queue_.cursor), std::memory_order_relaxed);
    resetPlaybackAccounting();
    if (hasDb())
        (void)saveStateLocked();
    const int64_t queueId = queue_.queue_id.load(std::memory_order_relaxed);
    qlk.unlock();

    // Same event doPlayTrack() publishes: listeners learn the incoming track
    // took over (position restarts at 0 as documented for TrackStarted).
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::TrackStarted;
    ev.track_id = trackId;
    ev.queue_id = queueId;
    ev.duration = trackDuration;
    ev.position = 0;
    pushEvent(ev);
    decodeCv_.notify_all();
    monCv_.notify_all();
    if (cfg_.logger)
        cfg_.logger->debug("crossfade complete track={}", trackId);
    return true;
}

void Engine::Impl::resetCrossfadeLocked() noexcept {
    crossfadeActive_.store(false, std::memory_order_release);
    crossfadePending_.store(false, std::memory_order_release);
    crossfadeDecoder_.reset();
    crossfadeReader_.reset();
    crossfadeTrack_ = {};
    crossfadeSamples_ = 0;
    crossfadeSample_ = 0;
}

void Engine::Impl::monitorLoop(std::stop_token st) {
    // Two-tier waits (C-1): the 10 ms cadence is only needed while playing
    // (300 ms gapless pre-roll window still gets ~12 looks at 25 ms).
    // Idle backs off to 100 ms. An explicit kanariv::types::EngineConfig::pollMs override is
    // honored for the playing interval; idle is always >= 100 ms.
    int cfgPoll = cfg_.pollMs ? cfg_.pollMs : 10;
    if (cfgPoll <= 0)
        cfgPoll = 10;
    const bool customPoll = (cfgPoll != 10);
    std::unique_lock<std::mutex> lk(monMtx_);
    while (!st.stop_requested() && monRun_.load(std::memory_order_acquire)) {
        lk.unlock();
        engineTick();
        int poll;
        if (playbackState_.load(std::memory_order_acquire) ==
            kanariv::types::PlaybackState::Playing)
            poll = customPoll ? cfgPoll : 25;
        else
            poll = customPoll ? (cfgPoll > 100 ? cfgPoll : 100) : 100;
        lk.lock();
        monCv_.wait_for(lk, std::chrono::milliseconds(poll), [&st, this] {
            return st.stop_requested() || !monRun_.load(std::memory_order_acquire);
        });
    }
}

void Engine::Impl::decodeLoop(std::stop_token st) {
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(playback::policy::maxChunkFrames * 8);
    while (!st.stop_requested() && decodeRun_.load(std::memory_order_acquire)) {
        // Sample the producer state under decodeMtx_: doPlayTrack() frees
        // decoder_/ring_ while holding it, so dereferencing them unlocked here
        // is a use-after-free.
        bool ready = false;
        size_t avail = 0;
        uint32_t ch = 2;
        uint64_t consumed = 0;
        // Local reference: once decodeMtx_ is released for the wait below, the
        // ring may be replaced (teardown), and the parked pointer must survive.
        std::shared_ptr<kanariv::utils::SpscRing<float>> ring;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            if (playbackState_.load(std::memory_order_acquire) ==
                    kanariv::types::PlaybackState::Playing &&
                decoder_ && ring_) {
                ring = ring_;
                // Sample the read counter before the space check so a drain
                // landing in between is visible to waitForRead() (P8).
                consumed = ring->readSequence();
                avail = ring->availableWrite();
                ch = decoder_->channels();
                if (ch == 0)
                    ch = 2;
                ready = true;
            }
        }
        if (!ready) {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Idle backoff (C-1): woken immediately by decodeCv_ on
            // play/seek/resume, so the longer timeout costs no latency.
            decodeCv_.wait_for(lk, std::chrono::milliseconds(50), [&st, this] {
                return st.stop_requested() || playbackState_.load(std::memory_order_acquire) ==
                                                  kanariv::types::PlaybackState::Playing;
            });
            continue;
        }
        // Consume a crossfade pending request as a transaction: resolve the
        // next entry without touching the cursor, open its reader+decoder
        // unlocked, then commit the cursor advance and arm the crossfade under
        // the queue lock. A failure (unreadable entry, concurrent queue change,
        // teardown) must leave the cursor on the unplayed entry -- the old code
        // advanced first and dropped the decoder, so one unreadable file was
        // skipped permanently (persisted across restarts).
        // Lock order: queueMutex_ -> decodeMtx_ (never the reverse).
        if (crossfadePending_.load(std::memory_order_acquire) &&
            !crossfadeActive_.load(std::memory_order_acquire)) {
            PeekedNext next;
            QueueFingerprint fingerprint;
            bool peeked = false;
            {
                std::unique_lock<std::timed_mutex> qlk(queueMutex_);
                if (playbackState_.load(std::memory_order_acquire) ==
                    kanariv::types::PlaybackState::Playing) {
                    fingerprint = queueFingerprintLocked();
                    if (auto r = peekNextLocked(); r) {
                        next = std::move(*r);
                        peeked = true;
                    }
                }
            }
            if (!peeked) {
                crossfadePending_.store(false, std::memory_order_release);
                if (cfg_.logger)
                    cfg_.logger->debug("crossfade preload cancelled");
            } else {
                // Slow I/O: open reader+decoder for the crossfade track (no mutex held).
                std::unique_ptr<kanariv::media::Reader> xfReader;
                std::unique_ptr<kanariv::media::Decoder> xfDecoder;
                auto path = kanariv::utils::detail::pathFromUtf8(next.track.path);
                if (auto rRes = kanariv::media::FileReader::open(path); rRes) {
                    xfReader = std::move(rRes.value());
                    if (auto dRes = kanariv::media::Decoder::open(*xfReader, cfg_.logger); dRes)
                        xfDecoder = std::move(dRes.value());
                }
                bool armed = false;
                if (xfReader && xfDecoder) {
                    // Commit and arm atomically against both state machines:
                    // queueMutex_ keeps doPlayTrack()/stop() out, decodeMtx_
                    // keeps pause()/seek() out, so the state checked here still
                    // holds when the crossfade becomes active.
                    std::unique_lock<std::timed_mutex> qlk(queueMutex_);
                    std::unique_lock<std::mutex> lk(decodeMtx_);
                    // decodeMtx_ is held here, so hand the position down
                    // instead of letting the persist path relock it.
                    const int64_t livePositionMs =
                        static_cast<int64_t>(currentPositionLocked() * 1000.0);
                    if (playbackState_.load(std::memory_order_acquire) ==
                            kanariv::types::PlaybackState::Playing &&
                        !crossfadeActive_.load(std::memory_order_relaxed) &&
                        commitNextLocked(fingerprint, std::move(next), livePositionMs)) {
                        crossfadeReader_ = std::move(xfReader);
                        crossfadeDecoder_ = std::move(xfDecoder);
                        crossfadeTrack_ = std::move(next.track);
                        crossfadeActive_.store(true, std::memory_order_release);
                        crossfadePending_.store(false, std::memory_order_release);
                        // crossfadeSample_ advances by decoded *outgoing* frames,
                        // so the overlap length must be expressed in the
                        // outgoing clock. The incoming rate only matters once
                        // this track is published.
                        uint32_t overlapRate = decoder_ ? decoder_->sampleRate() : 0;
                        if (overlapRate == 0)
                            overlapRate = crossfadeDecoder_->sampleRate();
                        crossfadeSamples_ =
                            static_cast<uint64_t>(cfg_.crossfadeMs) * static_cast<uint64_t>(overlapRate) / 1000;
                        crossfadeSample_ = 0;
                        armed = true;
                    }
                }
                if (!armed) {
                    // Keep the cursor on the failed entry: the next attempt
                    // (engineTick re-arms, or an explicit next()/play())
                    // targets it again instead of skipping past it.
                    crossfadePending_.store(false, std::memory_order_release);
                    if (cfg_.logger)
                        cfg_.logger->debug("crossfade preload failed track={} file={}",
                                           next.track.id, next.track.path);
                } else if (cfg_.logger) {
                    cfg_.logger->debug("crossfade armed track={}", crossfadeTrack_.id);
                }
            }
        }
        // Crossfade is armed by engineTick when the outgoing track nears its
        // end (remaining <= crossfadeMs). decodeLoop consumes the pending
        // request above and mixes both decoders during the overlap; an
        // interTrackGapMs silence is a separate policy handled at EOF.
        if (avail == 0) {
            // Ring full: park until the audio callback drains (consumer-side
            // signal) instead of polling; the bound covers a stopped device.
            (void)ring->waitForRead(consumed, playback::policy::ringWakeBudget);
            continue;
        }
        size_t maxFrames = playback::policy::chunkFrames(ch, avail);

        if (scratch.size() < maxFrames * ch)
            scratch.resize(maxFrames * ch);
        std::span<float> chunk(scratch.data(), maxFrames * ch);
        size_t frames = 0;
        media::DecodeStatus decodeStatus = media::DecodeStatus::Ok;
        uint64_t eofEpoch = 0;
        size_t xfFrames = 0;
        uint32_t sampleRate = 0;
        uint32_t xfCh = 0;
        std::vector<float> xfBuf;
        // Fade envelope parameters, snapped under decodeMtx_ (they are
        // decode-lock state; the mix below runs after the scope and must not
        // read the members unlocked).
        uint64_t fadeIn = 0;
        uint64_t fadeOut = 0;
        double fadeOutStart = 0.0;
        double xfProgress = 0.0;
        bool xfEarlyEof = false;
        bool xfComplete = false;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Re-check state under lock to avoid race with seek()
            if (playbackState_.load(std::memory_order_acquire) !=
                    kanariv::types::PlaybackState::Playing ||
                !decoder_)
                continue;
            // A track switch may have swapped the decoder for one with a
            // different channel count since the chunk was sized.
            ch = decoder_->channels();
            if (ch == 0)
                ch = 2;
            // Snapshot the rate under decodeMtx_: doPlayTrack() frees/replaces
            // decoder_ under the same lock, so the fade envelope below must not
            // dereference the decoder again.
            sampleRate = decoder_->sampleRate();
            fadeIn = fadeInSamples_;
            fadeOut = fadeOutSamples_;
            fadeOutStart = fadeOutStart_;
            frames = decoder_->decode(chunk, &decodeStatus);
            // Epoch of the transition this decoder was opened for. The EOF
            // ladder below runs after decodeMtx_ is released, so this is what
            // tells it whether the track it decoded is still the current one.
            eofEpoch = transitionEpoch_.load(std::memory_order_acquire);
            // During crossfade, decode the incoming track into a second buffer.
            if (crossfadeActive_.load(std::memory_order_relaxed) && crossfadeDecoder_ &&
                frames > 0) {
                // The incoming decoder keeps its own channel layout; sizing the
                // buffer from the outgoing channels decoded the wrong number of
                // frames and mixed interleaved data with the wrong stride.
                xfCh = crossfadeDecoder_->channels();
                if (xfCh == 0)
                    xfCh = ch;
                xfBuf.resize(frames * xfCh);
                xfFrames = crossfadeDecoder_->decode(std::span<float>(xfBuf.data(), xfBuf.size()));
                if (xfFrames == 0) {
                    // Incoming track reached EOF before the overlap did: it has
                    // become the playing track, so the transition completes on
                    // this iteration (tryPublishCrossfade below). Dropping the
                    // crossfade here without publishing left currentTrack_ and
                    // the persisted state describing the outgoing track.
                    xfEarlyEof = true;
                } else if (crossfadeSamples_ > 0) {
                    xfProgress = static_cast<double>(crossfadeSample_) /
                                 static_cast<double>(crossfadeSamples_);
                    if (xfProgress > 1.0)
                        xfProgress = 1.0;
                    crossfadeSample_ += frames;
                    if (crossfadeSample_ >= crossfadeSamples_)
                        xfComplete = true;
                }
            }
        }
        // Apply fade-in/fade-out envelope and crossfade mix (decode thread only,
        // real-time safe). fadeSample_ tracks frames into the outgoing track;
        // the envelope yields one gain per frame, applied to every channel.
        if ((fadeIn > 0 || fadeOut > 0 || crossfadeActive_.load(std::memory_order_relaxed)) &&
            frames > 0) {
            const uint64_t fadeStart = fadeSample_.load(std::memory_order_relaxed);
            kanariv::engine::detail::FadeParams fade;
            fade.startFrame = fadeStart;
            fade.fadeInFrames = fadeIn;
            fade.fadeOutFrames = fadeOut;
            fade.fadeOutStartSec = fadeOutStart;
            fade.sampleRate = static_cast<double>(sampleRate); // snapped under decodeMtx_
            std::span<const float> incoming;
            if (xfFrames > 0) {
                // Blend outgoing (envelope gain) with the incoming track; the
                // two buffers are interleaved independently, so the mix needs
                // each side's own channel count.
                incoming = std::span<const float>(xfBuf.data(), xfFrames * xfCh);
            }
            (void)kanariv::engine::detail::applyFadeEnvelope(
                std::span<float>(scratch.data(), frames * ch), frames, ch, fade, incoming,
                xfProgress, xfCh);
            fadeSample_.store(fadeStart + frames, std::memory_order_release);
        }
        if (frames == 0) {
            const auto outcome = playback::policy::classifyDecode(decodeStatus, frames);
            if (outcome == playback::policy::DecodeOutcome::Continue) {
                // No frames and no final status: not an end of track. Retry
                // instead of running the transition ladder.
                std::this_thread::sleep_for(playback::policy::decodeStallPoll);
                continue;
            }
            if (outcome == playback::policy::DecodeOutcome::Failed) {
                // A decode failure is not a clean end (P2). Nothing more can
                // come from this decoder, so report it and let the configured
                // transition move on; silently advancing hid the failure.
                if (cfg_.logger)
                    cfg_.logger->warn("decode failed track_id={}",
                                      state_.currentTrackId.load(std::memory_order_relaxed));
                kanariv::types::EngineEvent ev;
                ev.type = kanariv::types::EngineEventType::Error;
                ev.msg = "decode error";
                pushEvent(ev);
            }
            // EOF reached. Which policy owns the transition depends on what is
            // configured; none of them may advance the cursor while a crossfade
            // is still mixing the incoming track in.
            if (crossfadeActive_.load(std::memory_order_acquire)) {
                // The outgoing track is done while the incoming one is already
                // in use: the crossfade owns the transition, so complete it.
                // Waiting for the overlap to run out would leave nothing to
                // write (the outgoing decoder is at EOF) and stall the ring.
                (void)tryPublishCrossfade();
            } else if (cfg_.crossfadeMs > 0) {
                // A crossfade window is configured but nothing was armed (the
                // preload failed or is still pending). Keep the cursor on this
                // track: engineTick re-arms, and an explicit next()/play() still
                // moves on. Advancing here would silently skip the next entry.
            } else if (cfg_.interTrackGapMs > 0) {
                // Real inter-track silence: wait for the buffered tail to be
                // consumed by the device (the callback zero-fills on underrun,
                // so the ring being empty is the audible end), then hold the
                // gap before starting the next track.
                if (!interTrackGapArmed_.load(std::memory_order_acquire)) {
                    bool drained = true;
                    {
                        std::unique_lock<std::mutex> lk(decodeMtx_);
                        drained = !ring_ || ring_->availableRead() == 0;
                    }
                    if (drained) {
                        interTrackGapStartMs_ = detail::nowMs();
                        interTrackGapArmed_.store(true, std::memory_order_release);
                    }
                } else if (detail::nowMs() - interTrackGapStartMs_ >=
                           static_cast<uint64_t>(cfg_.interTrackGapMs)) {
                    // doPlayTrack() clears the armed flag on success; a failed
                    // advance (busy queue) is retried on the next pass with the
                    // gap already elapsed.
                    if (playbackState_.load(std::memory_order_acquire) ==
                        kanariv::types::PlaybackState::Playing) {
                        auto nr = autoNextIfEpoch(eofEpoch);
                        (void)nr;
                    }
                }
            } else if (playbackState_.load(std::memory_order_acquire) ==
                       kanariv::types::PlaybackState::Playing) {
                auto nr = autoNextIfEpoch(eofEpoch);
                (void)nr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        size_t samples = frames * ch;
        size_t writtenFrames;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Re-check state under lock to avoid race with seek() ring reset
            if (playbackState_.load(std::memory_order_acquire) !=
                    kanariv::types::PlaybackState::Playing ||
                !ring_)
                continue;
            writtenFrames = ring_->write(std::span<float>(scratch.data(), samples));
        }
        if (writtenFrames < frames) {
            // Ring full, will retry next iteration
        }
        if (xfComplete || xfEarlyEof) {
            // The overlap is finished (or the incoming track ran out first):
            // publish the incoming track so currentTrack_/duration_/history/
            // persistence describe what is now playing. When the queue lock is
            // contended the crossfade stays active and the next iteration (or
            // the EOF branch above) retries.
            (void)tryPublishCrossfade();
        }
    }
}

} // namespace kanariv::engine
