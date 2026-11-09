#include <atomic>
#include <kanariv/contracts/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <engine/history.hpp>
#include <engine/shuffle.hpp>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
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
}

// All engine state and implementation live here so the public header stays
// cheap (no mpsc/ring/std-threading includes). Engine methods delegate.

/**
 * @brief In-memory queue cursor and shuffle state.
 * @details `perm` is empty when shuffle is off; when on it holds a
 * permutation of `[0, queueCount)` shuffled via detail::shufflePerm.
 * `cursor` is the next index to consume (persisted as `cursor_pos`).
 * `queue_id` is the active queue (persisted as `active_queue_id`).
 * Protected by `Engine::queueMutex_` for writers; readers use atomics
 * or hold the mutex.
 */
struct QueueState {
    bool shuffle{false}; ///< Whether shuffle is enabled.
    kanariv::types::RepeatMode repeat{kanariv::types::RepeatMode::Off}; ///< Repeat mode.
    std::vector<int64_t> perm{}; ///< Shuffle permutation (positions, not track_ids).
    size_t cursor{0};            ///< Next position index in perm or linear queue.
    int64_t queue_id{1};         ///< Active queue id.
};

/**
 * @brief Persisted engine state (row `engine_state.id=1`).
 * @details Mirrors columns `shuffle_enabled, repeat_mode, cursor_pos,
 * current_track_id, volume, shuffle_perm (BLOB), active_queue_id,
 * pre_mute_volume, position_ms`. `active_queue_id` may be missing on old DBs -- loadState falls
 * back to `1` and saveState tries `sqlNew` then `sqlOld`. `pre_mute_volume` and `position_ms` are
 * best-effort both ways (missing columns keep their defaults). `shuffle_perm` is `perm` serialized
 * as `int64_t` blob.
 */
struct EngineState {
    int shuffleEnabled{0}; ///< Persisted shuffle flag (0/1).
    kanariv::types::RepeatMode repeatMode{
        kanariv::types::RepeatMode::Off}; ///< Persisted repeat mode.
    int64_t cursorPos{0};                ///< Persisted cursor (QueueState::cursor).
    int64_t currentTrackId{0};           ///< Last current track id.
    float volume{1.0f};                  ///< Persisted volume [0,1].
    int64_t activeQueueId{1};            ///< Persisted active queue id.
    float preMuteVolume{0.5f};           ///< Restore level for unmute (0,1].
    int64_t positionMs{0};               ///< Persisted position in the current track.
};

struct Engine::Impl {
    explicit Impl(const kanariv::types::EngineConfig& cfg);

    // lifecycle
    std::optional<kanariv::utils::Error> init();
    void shutdown();

    // database / persistence
    bool hasDb() const noexcept;
    ExpectedVoid attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db);
    std::optional<kanariv::utils::Error> loadState();
    std::expected<void, kanariv::utils::Error> saveState();
    std::expected<void, kanariv::utils::Error> persistShuffleBlobLocked();
    std::expected<void, kanariv::utils::Error> persistCursorLocked();
    std::expected<kanariv::types::Track, kanariv::utils::Error> fetchTrackByPosLocked(int64_t qid,
                                                                                    int64_t pos);

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
    ExpectedVoid resume();
    ExpectedVoid stop();
    ExpectedVoid seek(double seconds);
    ExpectedVoid suspendCurrentTrack();
    ExpectedVoid resumeAfterTagWrite(const kanariv::types::Track& t, double pos, bool wasPaused);
    ExpectedVoid next();
    ExpectedVoid autoNext();
    ExpectedVoid prev();
    ExpectedVoid advanceLocked(bool stopAtEnd, int lockWaitMs = 0);
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
    double currentPositionLocked() const noexcept;
    bool shuffle() const noexcept;
    kanariv::types::RepeatMode repeat() const noexcept;
    int64_t activeQueueId() const noexcept;

    // shuffle / repeat
    std::vector<int64_t> shufflePermFor(int64_t qid) noexcept;
    void noteEnqueued(int64_t qid, std::size_t count) noexcept;
    ExpectedVoid setShuffle(bool on);
    std::expected<void, kanariv::utils::Error> setShuffleLocked(bool on);
    ExpectedVoid setRepeat(kanariv::types::RepeatMode m);

    // library / history
    std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> stats();
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(int limit);
    std::expected<void, kanariv::utils::Error> clearHistory();
    std::string lastError() const;

    // queue traversal
    std::expected<void, kanariv::utils::Error> queueNextLocked(kanariv::types::Track& out);
    std::expected<void, kanariv::utils::Error> queuePrevLocked(kanariv::types::Track& out);
    std::expected<void, kanariv::utils::Error> doPlayTrack(kanariv::types::Track& t,
                                                          double startSec = 0.0);
    void preroll();

    // callbacks / events
    ExpectedVoid setCallbacks(const kanariv::types::EngineCallbacks& cbs);
    std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> pollEvent();
    ExpectedVoid drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n);
    std::vector<kanariv::types::EngineEvent> drainAll();

    // monitor / decode
    void engineTick();
    void resetPlaybackAccounting() noexcept;
    void accountPlaybackTime() noexcept;
    void monitorLoop(std::stop_token st);
    void decodeLoop(std::stop_token st);
    void doHistoryMark();
    void pushEvent(const kanariv::types::EngineEvent& ev);

    // members (moved verbatim from Engine)
    kanariv::types::EngineConfig cfg_{};
    EngineState state_{};
    QueueState queue_{};
    kanariv::types::Track currentTrack_{};
    std::atomic<bool> hasCurrent_{false};
    double duration_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> preMute_{0.5f};
    std::atomic<kanariv::types::PlaybackState> playbackState_{kanariv::types::PlaybackState::Stopped};
    std::chrono::steady_clock::time_point playStart_{};
    double pausePos_{0};
    std::atomic<uint64_t> accumulatedPlaybackMs_{0};
    std::atomic<uint64_t> lastPlaybackSampleMs_{0};
    std::atomic<bool> markedPlayed_{false};
    int64_t startedMs_{0};
    std::string lastErr_{};

    std::shared_ptr<kanariv::contracts::IDatabase> db_{};

    // player
    std::unique_ptr<kanariv::media::Reader> reader_{};
    std::unique_ptr<kanariv::media::Decoder> decoder_{};
    std::unique_ptr<kanariv::utils::SpscRing<float>> ring_{};
    std::unique_ptr<kanariv::player::AudioOutput> output_{};

    std::jthread decodeThread_{};
    std::atomic<bool> decodeRun_{false};
    std::mutex decodeMtx_{};
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
    // Playback polish state (real-time safe, touched only in decodeLoop/doPlayTrack):
    // fadeSample_ counts samples into the current fade; fadeInSamples_/fadeOutSamples_
    // are precomputed per track; fadeOutStart_ holds the position where fade-out begins.
    std::atomic<uint64_t> fadeSample_{0};
    uint64_t fadeInSamples_{0};
    uint64_t fadeOutSamples_{0};
    double fadeOutStart_{0};
    // interTrackGapArmed_: set when the current track ends and we're waiting
    // for the inter-track gap to elapse before starting the next track.
    std::atomic<bool> interTrackGapArmed_{false};
    uint64_t interTrackGapStartMs_{0};
    mutable std::timed_mutex queueMutex_;
};

Engine::Engine(const kanariv::types::EngineConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

Engine::ExpectedVoid Engine::attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db) {
    return impl_->attachDatabase(std::move(db));
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

Engine::ExpectedVoid Engine::resumeAfterTagWrite(const kanariv::types::Track& t, double pos,
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

std::vector<int64_t> Engine::shufflePermFor(int64_t qid) noexcept {
    return impl_->shufflePermFor(qid);
}

void Engine::noteEnqueued(int64_t qid, std::size_t count) noexcept {
    impl_->noteEnqueued(qid, count);
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

Engine::ExpectedVoid
Engine::Impl::attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db) {
    if (!db || !db->isOpen())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db"));
    db_ = std::move(db);
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
    // persist state
    if (hasDb()) {
        state_.cursorPos = (int64_t)queue_.cursor;
        (void)saveState();
    }
    if (output_) {
        output_->stop();
        output_.reset();
    }
    decoder_.reset();
    reader_.reset();
    ring_.reset();
}

Engine::ExpectedVoid Engine::Impl::play(int64_t queueId) {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (queueId == 0)
        queueId = 1;
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Paused) {
        // resume, don't dequeue
        playStart_ = std::chrono::steady_clock::now();
        lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        if (output_)
            output_->start();
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
            output_->start();
        lk.unlock();
        decodeCv_.notify_all();
        monCv_.notify_all();
        return {};
    } // end if Playing

    // Stopped -> resume persisted track if config enabled, else start new via cursor
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("play", "queue busy");
    queue_.queue_id = queueId;
    kanariv::types::Track t;
    // Resume persisted track if config enabled and track available
    if (cfg_.autoResumeOnPlay && state_.currentTrackId != 0 && state_.positionMs > 0) {
        auto idx = db_->queueIndexOf(queue_.queue_id, state_.currentTrackId);
        if (idx) {
            queue_.cursor = *idx;
            auto r = queueNextLocked(t);
            if (r) {
                auto pr = doPlayTrack(t, (double)state_.positionMs / 1000.0);
                if (pr) {
                    unlockQueue();
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
        from = (double)state_.positionMs / 1000.0;
    auto pr = doPlayTrack(t, from);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    return {};
}

Engine::ExpectedVoid Engine::Impl::playTrack(int64_t trackId) {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (trackId <= 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad track id"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("playTrack", "queue busy");
    auto idx = db_->queueIndexOf(queue_.queue_id, trackId);
    if (!idx) {
        unlockQueue();
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                        "track not in active queue")};
    }
    size_t cursor = *idx;
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
            if (queue_.perm[i] == (int64_t)*idx) {
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
    kanariv::types::Track t;
    auto r = queueNextLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    return pr;
}

Engine::ExpectedVoid Engine::Impl::pause() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Playing)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not playing"));
    // record pause position
    pausePos_ = currentPositionLocked();
    accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    if (output_)
        output_->stop();
    if (hasDb())
        (void)saveState();
    return {};
}

Engine::ExpectedVoid Engine::Impl::resume() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Paused)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not paused"));
    playStart_ = std::chrono::steady_clock::now();
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    if (output_)
        output_->start();
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

Engine::ExpectedVoid Engine::Impl::stop() {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Stopped)
        return {};
    if (s == kanariv::types::PlaybackState::Playing)
        accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
    // Release playback resources (decoder/output/ring hold tens of MB for
    // large files; measured +47 MB private retained after stop). Replay
    // re-opens via doPlayTrack. decodeMtx_ serializes with an in-flight
    // decode chunk (queue lock already released above: queue-to-decodeMtx_).
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        if (output_) {
            output_->stop();
            output_.reset();
        }
        decoder_.reset();
        reader_.reset();
        ring_.reset();
    }
    pausePos_ = 0;
    resetPlaybackAccounting();
    hasCurrent_.store(false, std::memory_order_release);
    // Rewind the cursor to the stopped track: cursor addresses the NEXT track,
    // so a later play would otherwise skip the stopped one. Already-stopped
    // returns above, so double-stop cannot rewind twice.
    if (tryLockQueue()) {
        if (queue_.cursor > 0) {
            queue_.cursor--;
            (void)persistCursorLocked();
        }
        unlockQueue();
    }
    // An explicit stop starts the track from its head next time (including
    // after a restart), so clear the resume position.
    state_.positionMs = 0;
    if (hasDb())
        (void)saveState();
    return {};
}

Engine::ExpectedVoid Engine::Impl::seek(double seconds) {
    if (!hasCurrent_.load(std::memory_order_acquire))
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no track"));
    if (!std::isfinite(seconds) || seconds < 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad seconds"));

    // Pause decode thread to safely seek + reset ring (prevents race with decodeLoop)
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
            playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
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

    // Resume decode thread
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    decodeCv_.notify_all();
    if (hasDb())
        (void)saveState();
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
    playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    if (output_)
        output_->stop();
    if (ring_)
        ring_->reset();
    decoder_.reset();
    reader_.reset();
    lk.unlock();
    unlockQueue();
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

Engine::ExpectedVoid Engine::Impl::resumeAfterTagWrite(const kanariv::types::Track& t, double pos,
                                                       bool wasPaused) {
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
    kanariv::types::Track reopen = t;
    auto r = doPlayTrack(reopen, pos);
    markedPlayed_.store(marked, std::memory_order_release);
    accumulatedPlaybackMs_.store(accumulated, std::memory_order_release);
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    // doPlayTrack seeks before preroll and starts Playing at `pos`; only the
    // prior paused state is left to restore.
    if (wasPaused) {
        auto pr = pause();
        if (!pr) {
            unlockQueue();
            return std::unexpected(pr.error());
        }
    }
    unlockQueue();
    return {};
}

Engine::ExpectedVoid Engine::Impl::setVolume(float g) {
    g = kanariv::utils::clampVolume(g);
    state_.volume = g;
    volume_.store(g, std::memory_order_relaxed);
    if (output_)
        output_->setVolume(g);
    if (hasDb()) {
        // Throttle persistence (M-3): a volume drag emits dozens of
        // events/s; each saveState() is a WAL transaction rewriting the
        // shuffle BLOB. Persist at most 1x/s here -- shutdown/track-change
        // paths always persist, so the crash window is <= 1 s of volume.
        const uint64_t now = detail::nowMs();
        const uint64_t last = lastVolSaveMs_.load(std::memory_order_acquire);
        if (now - last >= 1000) {
            lastVolSaveMs_.store(now, std::memory_order_release);
            state_.cursorPos = (int64_t)queue_.cursor;
            (void)saveState();
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
    return duration_;
}

double Engine::Impl::position() const noexcept {
    return currentPositionLocked();
}

int64_t Engine::Impl::currentTrackId() const noexcept {
    if (hasCurrent_.load(std::memory_order_acquire))
        return currentTrack_.id;
    return state_.currentTrackId;
}

bool Engine::Impl::shuffle() const noexcept {
    return queue_.shuffle;
}

kanariv::types::RepeatMode Engine::Impl::repeat() const noexcept {
    return queue_.repeat;
}

std::vector<int64_t> Engine::Impl::shufflePermFor(int64_t qid) noexcept {
    if (!tryLockQueue())
        return {};
    std::vector<int64_t> out;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty())
        out = queue_.perm;
    unlockQueue();
    return out;
}

void Engine::Impl::noteEnqueued(int64_t qid, std::size_t count) noexcept {
    if (count == 0 || !hasDb() || !tryLockQueue())
        return;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        std::size_t n = db_->queueCount(qid);
        std::size_t start = (n >= count) ? n - count : 0;
        for (std::size_t i = start; i < n; ++i)
            queue_.perm.push_back((int64_t)i);
        (void)persistShuffleBlobLocked();
    }
    unlockQueue();
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Engine::Impl::stats() {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    auto ds = db_->playbackStats();
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
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    kanariv::engine::History hist(db_);
    return hist.listHistory(limit);
}

std::expected<void, kanariv::utils::Error> Engine::Impl::clearHistory() {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    kanariv::engine::History hist(db_);
    return hist.clearHistory();
}

std::string Engine::Impl::lastError() const {
    if (!lastErr_.empty())
        return lastErr_;
    return "";
}

Engine::ExpectedVoid Engine::Impl::setShuffle(bool on) {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueue())
        return queueBusy("setShuffle", "busy");
    auto r = setShuffleLocked(on);
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
    if (!tryLockQueue())
        return queueBusy("setRepeat", "busy");
    queue_.repeat = m;
    state_.repeatMode = m;
    state_.cursorPos = (int64_t)queue_.cursor;
    std::optional<kanariv::utils::Error> err;
    if (auto e = saveState(); !e)
        err = e.error();
    unlockQueue();
    if (err)
        return std::unexpected(*err);
    return {};
}

int64_t Engine::Impl::activeQueueId() const noexcept {
    std::lock_guard<std::timed_mutex> lk(queueMutex_);
    return queue_.queue_id ? queue_.queue_id : state_.activeQueueId ? state_.activeQueueId : 1;
}

Engine::ExpectedVoid Engine::Impl::switchQueue(int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueue())
        return queueBusy("switchQueue", "busy");
    // Validate the queue while holding queueMutex_.
    auto exists = db_->queueExists(qid);
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
    // switch active queue: reset cursor, clear shuffle perm, update state
    queue_.queue_id = qid;
    queue_.cursor = 0;
    queue_.perm.clear();
    state_.activeQueueId = qid;
    state_.cursorPos = 0;
    // keep shuffle flag as-is but perm cleared; next shuffle will regenerate for new queue
    std::optional<kanariv::utils::Error> err;
    if (auto e = saveState(); !e)
        err = e.error();
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
    return advanceLocked(false, (int)queueLockWait.count());
}

Engine::ExpectedVoid Engine::Impl::autoNext() {
    return advanceLocked(true);
}

Engine::ExpectedVoid Engine::Impl::advanceLocked(bool stopAtEnd, int lockWaitMs) {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    // handle repeat one without shuffle without queue lock? match C: if !shuffle && repeat==One
    // && hasCurrent => seek 0 and play - serialize with decodeLoop via decodeMtx_
    if (!queue_.shuffle && queue_.repeat == kanariv::types::RepeatMode::One &&
        hasCurrent_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            if (decoder_) {
                auto r = decoder_->seek(0);
                if (!r)
                    return std::unexpected(r.error());
            }
            if (ring_)
                ring_->reset();
        }
        pausePos_ = 0;
        playStart_ = std::chrono::steady_clock::now();
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        resetPlaybackAccounting();
        markedPlayed_.store(false, std::memory_order_release);
        gaplessArmed_.store(false, std::memory_order_release);
        startedMs_ = (int64_t)detail::wallMs();
        kanariv::types::EngineEvent ev;
        ev.type = kanariv::types::EngineEventType::TrackStarted;
        ev.track_id = currentTrack_.id;
        ev.queue_id = queue_.queue_id;
        ev.duration = duration_;
        pushEvent(ev);
        decodeCv_.notify_all();
        return {};
    }
    // Monitor autoNext passes 0 (fail-fast; the next tick retries), user
    // next() passes the wait budget so colliding skips execute.
    bool locked =
        lockWaitMs > 0 ? tryLockQueueFor(std::chrono::milliseconds(lockWaitMs)) : tryLockQueue();
    if (!locked) {
        // Monitor autoNext fail-fast is routine (next tick retries) â€” stay
        // silent; only user-wait collisions are diagnostic.
        if (lockWaitMs > 0)
            return queueBusy("next", "busy");
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "busy"));
    }
    if (stopAtEnd && queue_.repeat == kanariv::types::RepeatMode::Off) {
        // Natural track end: halt instead of wrapping. The cursor stays parked
        // past the end, so a later play wraps to the head. Manual next() still
        // wraps (same repeat mode).
        bool atEnd = false;
        if (queue_.shuffle) {
            atEnd = queue_.cursor >= queue_.perm.size();
        } else {
            size_t cnt = db_->queueCount(queue_.queue_id);
            atEnd = queue_.cursor >= cnt;
        }
        if (atEnd) {
            // Natural end with repeat off: clear resume position so play()
            // doesn't seek to end-of-track and loop forever.
            state_.positionMs = 0;
            state_.currentTrackId = 0;
            hasCurrent_.store(false, std::memory_order_release);
            (void)saveState();
            unlockQueue();
            playbackState_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
            if (output_)
                output_->stop();
            if (ring_)
                ring_->reset();
            pausePos_ = 0;
            // Notify listeners (e.g. the OS media-session bridge) that the
            // track ended naturally so now-playing UI can switch to Stopped.
            kanariv::types::EngineEvent ev;
            ev.type = kanariv::types::EngineEventType::TrackEnded;
            ev.track_id = currentTrack_.id;
            ev.queue_id = queue_.queue_id;
            ev.position = duration_;
            ev.duration = duration_;
            pushEvent(ev);
            return {};
        }
    }
    kanariv::types::Track t;
    auto r = queueNextLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
    return {};
}

Engine::ExpectedVoid Engine::Impl::prev() {
    if (!hasDb())
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    if (!tryLockQueueFor(queueLockWait))
        return queueBusy("prev", "busy");
    kanariv::types::Track t;
    auto r = queuePrevLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr)
        return std::unexpected(pr.error());
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

Engine::ExpectedVoid Engine::Impl::drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n) {
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
    return db_ && db_->isOpen();
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
        return (double)state_.positionMs / 1000.0;
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
    auto loaded = db_->loadEngineState();
    if (!loaded)
        return loaded.error();
    state_.shuffleEnabled = loaded->shuffle_enabled;
    state_.repeatMode = loaded->repeat_mode;
    state_.cursorPos = loaded->cursor_pos;
    state_.currentTrackId = loaded->current_track_id;
    state_.volume = loaded->volume;
    state_.activeQueueId = loaded->active_queue_id;
    state_.preMuteVolume = loaded->pre_mute_volume;
    state_.positionMs = loaded->position_ms;
    volume_.store(state_.volume, std::memory_order_relaxed);
    preMute_.store(state_.preMuteVolume, std::memory_order_relaxed);
    queue_.perm = std::move(loaded->shuffle_perm);
    queue_.shuffle = state_.shuffleEnabled != 0;
    queue_.repeat = state_.repeatMode;
    queue_.cursor = state_.cursorPos < 0 ? 0 : static_cast<size_t>(state_.cursorPos);
    if (!queue_.perm.empty() && queue_.cursor >= queue_.perm.size())
        queue_.cursor = 0;
    queue_.queue_id = state_.activeQueueId;
    auto queueExists = db_->queueExists(queue_.queue_id);
    if (!queueExists || !*queueExists) {
        queue_.queue_id = 1;
        state_.activeQueueId = 1;
    }
    return std::nullopt;
}

std::expected<void, kanariv::utils::Error> Engine::Impl::saveState() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    kanariv::types::EngineState persisted;
    persisted.shuffle_enabled = state_.shuffleEnabled;
    persisted.repeat_mode = state_.repeatMode;
    persisted.cursor_pos = state_.cursorPos;
    persisted.current_track_id = state_.currentTrackId;
    persisted.volume = state_.volume;
    persisted.active_queue_id = state_.activeQueueId ? state_.activeQueueId : queue_.queue_id;
    persisted.pre_mute_volume = state_.preMuteVolume;
    persisted.position_ms = hasCurrent_.load(std::memory_order_acquire)
                                ? static_cast<int64_t>(currentPositionLocked() * 1000.0)
                                : state_.positionMs;
    if (persisted.position_ms < 0)
        persisted.position_ms = 0;
    persisted.shuffle_perm = queue_.perm;
    auto result = db_->saveEngineState(persisted);
    if (result)
        state_.positionMs = persisted.position_ms;
    return result;
}

std::expected<void, kanariv::utils::Error> Engine::Impl::persistShuffleBlobLocked() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    auto result =
        db_->saveEngineShuffle(queue_.shuffle, queue_.perm, static_cast<int64_t>(queue_.cursor));
    if (result) {
        state_.shuffleEnabled = queue_.shuffle ? 1 : 0;
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    }
    return result;
}

std::expected<void, kanariv::utils::Error> Engine::Impl::persistCursorLocked() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    auto result = db_->saveEngineCursor(static_cast<int64_t>(queue_.cursor));
    if (result)
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    return result;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Engine::Impl::fetchTrackByPosLocked(int64_t qid, int64_t pos) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    return db_->queueTrackAt(qid, pos);
}

std::expected<void, kanariv::utils::Error> Engine::Impl::setShuffleLocked(bool on) {
    // assert: queueMutex_ locked by caller
    bool want = on;
    if (queue_.shuffle == want && !queue_.perm.empty())
        return {};
    if (want) {
        queue_.perm.clear();
        queue_.cursor = 0;
        size_t cnt = db_->queueCount(queue_.queue_id);
        if (cnt == 0) {
            queue_.shuffle = true;
            queue_.perm.clear();
            queue_.cursor = 0;
            auto r = persistShuffleBlobLocked();
            if (!r)
                return r;
            state_.shuffleEnabled = 1;
            return {};
        }
        queue_.perm.resize(cnt);
        for (size_t i = 0; i < cnt; ++i)
            queue_.perm[i] = (int64_t)i;
        {
            std::mt19937 rng{std::random_device{}()};
            detail::shufflePerm(queue_.perm, rng);
        }
        queue_.shuffle = true;
        queue_.cursor = 0;
        auto r = persistShuffleBlobLocked();
        if (!r)
            return r;
        state_.shuffleEnabled = 1;
        return {};
    } else {
        queue_.perm.clear();
        queue_.cursor = 0;
        queue_.shuffle = false;
        auto r = persistShuffleBlobLocked();
        if (!r)
            return r;
        state_.shuffleEnabled = 0;
        return {};
    }
}

std::expected<void, kanariv::utils::Error> Engine::Impl::queueNextLocked(kanariv::types::Track& out) {
    if (queue_.queue_id == 0)
        queue_.queue_id = 1;
    if (queue_.shuffle) {
        if (queue_.perm.empty()) {
            size_t cnt = db_->queueCount(queue_.queue_id);
            if (cnt == 0)
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
            auto sr = setShuffleLocked(true);
            if (!sr)
                return std::unexpected(sr.error());
            if (queue_.perm.empty())
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no perm"));
        }
        if (queue_.cursor >= queue_.perm.size()) {
            if (queue_.repeat == kanariv::types::RepeatMode::One) {
                size_t idx = queue_.perm.size() - 1;
                if (queue_.cursor > 0 && queue_.cursor <= queue_.perm.size())
                    idx = queue_.cursor - 1;
                int64_t pos = queue_.perm[idx];
                auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
                if (!tr)
                    return std::unexpected(tr.error());
                out = tr.value();
                return {};
            } else {
                // wrap/reshuffle for both Off and Queue (shuffle on => new perm)
                auto sr = setShuffleLocked(true);
                if (!sr)
                    return std::unexpected(sr.error());
                queue_.cursor = 0;
                (void)persistCursorLocked();
            }
        }
        int64_t pos = queue_.perm[queue_.cursor];
        queue_.cursor++;
        (void)persistCursorLocked();
        auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
        if (!tr)
            return std::unexpected(tr.error());
        out = tr.value();
        return {};
    }
    size_t cnt = db_->queueCount(queue_.queue_id);
    if (cnt == 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
    if (queue_.cursor >= cnt) {
        if (queue_.repeat == kanariv::types::RepeatMode::One) {
            size_t idx = cnt - 1;
            if (queue_.cursor > 0 && queue_.cursor <= cnt)
                idx = queue_.cursor - 1;
            auto tr = fetchTrackByPosLocked(queue_.queue_id, (int64_t)idx);
            if (!tr)
                return std::unexpected(tr.error());
            out = tr.value();
            return {};
        } else {
            // wrap for both Off and Queue; reshuffle if shuffle on
            if (queue_.shuffle) {
                auto sr = setShuffleLocked(true);
                if (!sr)
                    return std::unexpected(sr.error());
                queue_.cursor = 0;
            } else {
                queue_.cursor = 0;
            }
            (void)persistCursorLocked();
        }
    }
    auto tr = fetchTrackByPosLocked(queue_.queue_id, (int64_t)queue_.cursor);
    if (!tr)
        return std::unexpected(tr.error());
    queue_.cursor++;
    (void)persistCursorLocked();
    out = tr.value();
    return {};
}

std::expected<void, kanariv::utils::Error> Engine::Impl::queuePrevLocked(kanariv::types::Track& out) {
    if (!queue_.shuffle) {
        size_t cnt = db_->queueCount(queue_.queue_id);
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
        auto tr = fetchTrackByPosLocked(queue_.queue_id, (int64_t)queue_.cursor);
        if (!tr)
            return std::unexpected(tr.error());
        queue_.cursor++;
        (void)persistCursorLocked();
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
    (void)persistCursorLocked();
    auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
    if (!tr)
        return std::unexpected(tr.error());
    out = tr.value();
    return {};
}

std::expected<void, kanariv::utils::Error> Engine::Impl::doPlayTrack(kanariv::types::Track& t,
                                                                    double startSec) {
    // try to open decoder path
    std::string path = t.path;
    if (!std::isfinite(startSec) || startSec < 0.0)
        startSec = 0.0;
    // Teardown and reopen must serialize with decodeLoop (decoder_->decode /
    // ring_->write hold decodeMtx_) and with the audio callback, which keeps
    // cfg.ring raw. Freeing those without the lock is a use-after-free (ASan:
    // convertFrameBuffered while doPlayTrack reset decoder_); stopping the
    // device first also quiesces the callback before the ring goes away.
    // Lock order queue-to-decodeMtx_ matches stop()/suspendCurrentTrack();
    // every caller already holds the queue lock.
    std::unique_lock<std::mutex> lk(decodeMtx_);
    if (output_) {
        output_->stop();
        output_.reset();
    }
    decoder_.reset();
    reader_.reset();
    ring_.reset();

    // attempt to open reader & decoder; without allowSimulatedPlayback an
    // unplayable track is an error, never silent timer playback
    bool opened = false;
    if (!path.empty()) {
        // Track paths are stored UTF-8; decode them as UTF-8 rather than the
        // Windows ANSI code page so non-ASCII filenames resolve.
        auto rRes = kanariv::media::FileReader::open(
            kanariv::utils::detail::pathFromUtf8(path));
        if (rRes) {
            reader_ = std::move(rRes.value());
            auto dRes = kanariv::media::Decoder::open(*reader_, cfg_.logger);
            if (dRes) {
                decoder_ = std::move(dRes.value());
                opened = true;
                duration_ = decoder_->totalFrames()
                                ? (double)decoder_->totalFrames() / (double)decoder_->sampleRate()
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
                // create ring and output
                // NOTE (2026-10-05): kept at 8192*ch -- a flat 8192-frame cap
                // measured +9 MB daemon RSS on an mp3 with an attached mjpeg
                // cover (release-lto, clang, 67.7 -> 76.6 MB WS). Mechanism
                // unknown (suspect indirect FFmpeg buffering/timing effect);
                // do NOT re-shrink without a measured explanation.
                uint32_t sr = decoder_->sampleRate();
                uint32_t ch = decoder_->channels();
                if (sr == 0)
                    sr = 48000;
                if (ch == 0)
                    ch = 2;
                ring_ = std::make_unique<kanariv::utils::SpscRing<float>>(8192 * ch, ch);
                kanariv::player::AudioOutput::Config cfg;
                cfg.sampleRate = sr;
                cfg.channels = ch;
                cfg.ring = ring_.get();
                // ReplayGain: apply track/album gain (dB) as a volume multiplier
                // on top of the user volume. dB -> linear: 10^(dB/20).
                float gain = volume_.load(std::memory_order_relaxed);
                if (cfg_.replayGain != kanariv::types::ReplayGainMode::Off) {
                    double gainDb = 0.0;
                    if (cfg_.replayGain == kanariv::types::ReplayGainMode::Track)
                        gainDb = t.replayGainTrack;
                    else if (cfg_.replayGain == kanariv::types::ReplayGainMode::Album)
                        gainDb = t.replayGainAlbum;
                    if (gainDb != 0.0)
                        gain = (float)(gain * std::pow(10.0, gainDb / 20.0));
                }
                cfg.volume = gain;
                cfg.logger = cfg_.logger;
                auto oRes = kanariv::player::AudioOutput::create(cfg);
                if (oRes) {
                    output_ = std::move(oRes.value());
                    // Initialize fade state for this track.
                    fadeSample_.store(0, std::memory_order_release);
                    fadeInSamples_ = 0;
                    fadeOutSamples_ = 0;
                    fadeOutStart_ = 0;
                    if (cfg_.fadeInMs > 0 && sr > 0)
                        fadeInSamples_ = (uint64_t)cfg_.fadeInMs * sr / 1000;
                    if (cfg_.fadeOutMs > 0 && sr > 0 && duration_ > 0) {
                        fadeOutSamples_ = (uint64_t)cfg_.fadeOutMs * sr / 1000;
                        fadeOutStart_ = duration_ - (double)cfg_.fadeOutMs / 1000.0;
                        if (fadeOutStart_ < 0.0)
                            fadeOutStart_ = 0.0;
                    }
                    // preroll: decode some frames before start
                    preroll();
                    output_->start();
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
    // Start each track with a fresh accounting window. Without this the time
    // accumulated on the previous track carried over, so the next track was
    // marked played immediately on entry (and the skip/history window was
    // wrong). resumeAfterTagWrite() saves and restores the counters around
    // doPlayTrack(), so a tag write mid-track still keeps its progress.
    resetPlaybackAccounting();
    startedMs_ = (int64_t)detail::wallMs();
    state_.currentTrackId = t.id;
    state_.cursorPos = (int64_t)queue_.cursor;
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    // Position tracking starts at the resumed offset (0 for next/prev);
    // saveState() below then persists that offset instead of a transient 0.
    // pausePos_ already holds the offset, so playStart_ must be "now" --
    // shifting it back too would count the offset twice.
    pausePos_ = startSec;
    playStart_ = std::chrono::steady_clock::now();
    if (hasDb())
        (void)saveState();
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::TrackStarted;
    ev.track_id = t.id;
    ev.queue_id = queue_.queue_id;
    ev.duration = duration_;
    ev.position = 0;
    pushEvent(ev);
    decodeCv_.notify_all();
    monCv_.notify_all();
    if (cfg_.logger)
        cfg_.logger->debug("playing track={} file={}", t.id, path);
    return {};
}

void Engine::Impl::preroll() {
    if (!decoder_ || !ring_)
        return;
    uint32_t ch = decoder_->channels();
    if (ch == 0)
        ch = 2;
    // preroll cap/2 frames like Player (ring capacity is in frames)
    size_t need = ring_->availableWrite() / 2;
    if (need == 0)
        return;
    size_t chunkFrames = 1024;
    size_t maxChunk = 2048 / ch;
    if (chunkFrames > maxChunk)
        chunkFrames = maxChunk;
    std::vector<float> tmp(chunkFrames * ch);
    while (need > 0 && ring_->availableWrite() >= tmp.size() / ch) {
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
    } else if (ev.type == kanariv::types::EngineEventType::QueueChanged && cbsCopy.on_queue_changed) {
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

void Engine::Impl::doHistoryMark() {
    if (!hasDb())
        return;
    if (!hasCurrent_.load(std::memory_order_acquire))
        return;
    if (markedPlayed_.load(std::memory_order_acquire))
        return;
    double dur = duration_;
    const double played =
        static_cast<double>(accumulatedPlaybackMs_.load(std::memory_order_acquire)) / 1000.0;
    int pct = cfg_.historyThresholdPct ? cfg_.historyThresholdPct : 60;
    int secs = cfg_.historyThresholdSecs ? cfg_.historyThresholdSecs : 90;
    if (!detail::shouldMarkPlayed(dur, played, false, pct, secs))
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
    const double pos = currentPositionLocked();
    if (dur > 0.0) {
        completionPct = (pos / dur) * 100.0;
        if (completionPct > 100.0)
            completionPct = 100.0;
    }
    auto result = db_->markPlayed(currentTrack_.id, startedMs_, completion,
                                  static_cast<int64_t>(pos * 1000.0), completionPct,
                                  queue_.queue_id ? queue_.queue_id : 1);
    if (!result)
        markedPlayed_.store(false, std::memory_order_release);
}

void Engine::Impl::engineTick() {
    if (playbackState_.load(std::memory_order_acquire) ==
        kanariv::types::PlaybackState::Playing)
        accountPlaybackTime();
    doHistoryMark();
    auto st = playbackState_.load(std::memory_order_acquire);
    if (st == kanariv::types::PlaybackState::Playing) {
        uint64_t now = detail::nowMs();
        uint64_t last = lastProgressMs_.load(std::memory_order_acquire);
        if (now - last >= 500) {
            lastProgressMs_.store(now, std::memory_order_release);
            kanariv::types::EngineEvent ev;
            ev.type = kanariv::types::EngineEventType::Progress;
            ev.track_id = hasCurrent_.load(std::memory_order_acquire) ? currentTrack_.id
                                                                      : state_.currentTrackId;
            ev.queue_id = queue_.queue_id ? queue_.queue_id : 1;
            ev.position = currentPositionLocked();
            ev.duration = duration_;
            pushEvent(ev);
        }
        if (duration_ > 0) {
            double pos = currentPositionLocked();
            double remaining = duration_ - pos;
            double gapS = (double)cfg_.gaplessMs / 1000.0;
            // Gapless preroll is skipped when an inter-track gap is configured:
            // the track plays to end, then decodeLoop handles the gap before
            // starting the next track.
            if (cfg_.interTrackGapMs == 0 && remaining <= gapS && remaining >= 0.0) {
                if (hasCurrent_.load(std::memory_order_acquire)) {
                    // gaplessArmed 0->1 CAS, 300ms preroll
                    bool expected = false;
                    if (gaplessArmed_.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        auto nr = autoNext();
                        if (!nr)
                            gaplessArmed_.store(false, std::memory_order_release);
                    }
                }
            }
        }
        // Resume-position checkpoint (5 s crash window). Skip when the queue
        // lock is contended: another thread will save on its own path.
        uint64_t lastSave = lastPosSaveMs_.load(std::memory_order_acquire);
        if (now - lastSave >= 5000 && tryLockQueue()) {
            state_.cursorPos = (int64_t)queue_.cursor;
            if (saveState())
                lastPosSaveMs_.store(now, std::memory_order_release);
            unlockQueue();
        }
    }
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
        if (playbackState_.load(std::memory_order_acquire) == kanariv::types::PlaybackState::Playing)
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
    constexpr std::size_t maxChunkFrames = 1024;
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(maxChunkFrames * 8);
    while (!st.stop_requested() && decodeRun_.load(std::memory_order_acquire)) {
        // Sample the producer state under decodeMtx_: doPlayTrack() frees
        // decoder_/ring_ while holding it, so dereferencing them unlocked here
        // is a use-after-free.
        bool ready = false;
        size_t avail = 0;
        uint32_t ch = 2;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            if (playbackState_.load(std::memory_order_acquire) ==
                    kanariv::types::PlaybackState::Playing &&
                decoder_ && ring_) {
                avail = ring_->availableWrite();
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
        // Inter-track gap: after the current track decodes to EOF, wait
        // interTrackGapMs before starting the next track. Check here so the
        // gap elapses even while the ring is draining to silence.
        if (interTrackGapArmed_.load(std::memory_order_acquire)) {
            uint64_t now = detail::nowMs();
            if (now - interTrackGapStartMs_ >=
                (uint64_t)cfg_.interTrackGapMs) {
                interTrackGapArmed_.store(false, std::memory_order_release);
                if (playbackState_.load(std::memory_order_acquire) ==
                    kanariv::types::PlaybackState::Playing) {
                    auto nr = autoNext();
                    (void)nr;
                }
            }
        }
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        size_t maxFrames = avail / ch;
        if (maxFrames > maxChunkFrames)
            maxFrames = maxChunkFrames;
        size_t maxChunkByCh = 2048 / ch;
        if (maxFrames > maxChunkByCh)
            maxFrames = maxChunkByCh;
        if (maxFrames == 0)
            maxFrames = 1;

        if (scratch.size() < maxFrames * ch)
            scratch.resize(maxFrames * ch);
        std::span<float> chunk(scratch.data(), maxFrames * ch);
        size_t frames;
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
            frames = decoder_->decode(chunk);
        }
        // Apply fade-in/fade-out envelope to decoded samples (decode thread only,
        // real-time safe). fadeSample_ tracks samples decoded for this track.
        if ((fadeInSamples_ > 0 || fadeOutSamples_ > 0) && frames > 0) {
            uint64_t fadeStart = fadeSample_.load(std::memory_order_relaxed);
            double sr = (double)decoder_->sampleRate();
            if (sr == 0.0)
                sr = 48000.0;
            size_t samples = frames * ch;
            for (size_t i = 0; i < samples; ++i) {
                uint64_t sampleIdx = fadeStart + i;
                double gain = 1.0;
                // Fade-in: ramp 0 -> 1 over fadeInSamples.
                if (fadeInSamples_ > 0 && sampleIdx < fadeInSamples_)
                    gain = (double)sampleIdx / (double)fadeInSamples_;
                // Fade-out: ramp 1 -> 0 over fadeOutSamples starting at fadeOutStart_.
                if (fadeOutSamples_ > 0) {
                    double posSec = (double)sampleIdx / sr;
                    if (posSec >= fadeOutStart_) {
                        double fadeProg = (posSec - fadeOutStart_) /
                                          ((double)fadeOutSamples_ / sr);
                        gain = (fadeProg >= 1.0) ? 0.0 : (1.0 - fadeProg);
                    }
                }
                for (uint32_t c = 0; c < ch; ++c)
                    scratch[i * ch + c] = (float)(scratch[i * ch + c] * gain);
            }
            fadeSample_.store(fadeStart + samples, std::memory_order_release);
        }
        if (frames == 0) {
            // EOF reached. If an inter-track gap is configured, arm the gap
            // timer instead of advancing immediately: the next track starts
            // after interTrackGapMs of silence. Otherwise advance as usual
            // (gapless path).
            if (cfg_.interTrackGapMs > 0 &&
                !interTrackGapArmed_.load(std::memory_order_acquire)) {
                interTrackGapArmed_.store(true, std::memory_order_release);
                interTrackGapStartMs_ = detail::nowMs();
            } else if (playbackState_.load(std::memory_order_acquire) ==
                       kanariv::types::PlaybackState::Playing) {
                auto nr = autoNext();
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
    }
}

} // namespace kanariv::engine
