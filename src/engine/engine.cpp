#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <engine/history.hpp>
#include <engine/shuffle.hpp>
#include <expected>
#include <kanariv/contracts/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
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
#include <vector>

namespace kanariv::engine {

constexpr float defaultPreMuteVolume = 0.5f;
constexpr int eventQueueCapacity = 64;
constexpr int volumeSaveIntervalMs = 1000;
constexpr int msToSecDivisor = 1000;
constexpr int defaultPollMs = 10;
constexpr int defaultGaplessMs = 300;
constexpr int defaultHistoryThresholdPct = 60;
constexpr int defaultHistoryThresholdSecs = 90;
constexpr int defaultSampleRate = 48000;
constexpr int defaultChannels = 2;
constexpr int ringBufferFramesPerChannel = 8192;
constexpr int decodeChunkFrames = 1024;
constexpr int maxDecodeChunkFrames = 2048;
constexpr int pctScale = 100;
constexpr int pctMax = 100;
constexpr int progressCheckIntervalMs = 500;
constexpr int positionSaveIntervalMs = 5000;
constexpr int defaultPollIntervalMs = 25;
constexpr int defaultIdlePollMs = 100;
constexpr int scratchChannels = 8;
constexpr int monitorIdleWaitMs = 50;
constexpr int decodeIdleSleepMs = 5;
constexpr int eofWaitMs = 25;

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
    std::vector<int64_t> perm; ///< Shuffle permutation (positions, not track_ids).
    size_t cursor{0};          ///< Next position index in perm or linear queue.
    int64_t queue_id{1};       ///< Active queue id.
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
        kanariv::types::RepeatMode::Off};      ///< Persisted repeat mode.
    int64_t cursorPos{0};                      ///< Persisted cursor (QueueState::cursor).
    int64_t currentTrackId{0};                 ///< Last current track id.
    float volume{1.0f};                        ///< Persisted volume [0,1].
    int64_t activeQueueId{1};                  ///< Persisted active queue id.
    float preMuteVolume{defaultPreMuteVolume}; ///< Restore level for unmute (0,1].
    int64_t positionMs{0};                     ///< Persisted position in the current track.
};

struct Engine::Impl {
    explicit Impl(const kanariv::types::EngineConfig& cfg);

    // lifecycle
    auto init() -> std::optional<kanariv::utils::Error>;
    void shutdown();

    // database / persistence
    auto hasDb() const noexcept -> bool;
    auto attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db) -> ExpectedVoid;
    auto loadState() -> std::optional<kanariv::utils::Error>;
    auto saveState() -> std::expected<void, kanariv::utils::Error>;
    auto persistShuffleBlobLocked() -> std::expected<void, kanariv::utils::Error>;
    auto persistCursorLocked() -> std::expected<void, kanariv::utils::Error>;
    auto fetchTrackByPosLocked(int64_t qid, int64_t pos) const
        -> std::expected<kanariv::types::Track, kanariv::utils::Error>;

    // queue locking
    auto tryLockQueue() noexcept -> bool;
    auto tryLockQueueFor(std::chrono::milliseconds wait) noexcept -> bool;
    void unlockQueue() noexcept;
    auto queueBusy(std::string_view op, std::string_view msg) const
        -> std::unexpected<kanariv::utils::Error>;

    // transport
    auto play(int64_t queueId) -> ExpectedVoid;
    auto playTrack(int64_t trackId) -> ExpectedVoid;
    auto pause() -> ExpectedVoid;
    auto resume() -> ExpectedVoid;
    auto stop() -> ExpectedVoid;
    auto seek(double seconds) -> ExpectedVoid;
    auto suspendCurrentTrack() -> ExpectedVoid;
    auto resumeAfterTagWrite(const kanariv::types::Track& t, double pos, bool wasPaused)
        -> ExpectedVoid;
    auto next() -> ExpectedVoid;
    auto autoNext() -> ExpectedVoid;
    auto prev() -> ExpectedVoid;
    auto advanceLocked(bool stopAtEnd, int lockWaitMs = 0) -> ExpectedVoid;
    auto switchQueue(int64_t qid) -> ExpectedVoid;

    // volume / state accessors
    auto setVolume(float g) -> ExpectedVoid;
    auto volume() const noexcept -> float;
    auto setPreMuteVolume(float g) -> ExpectedVoid;
    auto preMuteVolume() const noexcept -> float;
    auto state() const noexcept -> kanariv::types::PlaybackState;
    auto duration() const noexcept -> double;
    auto position() const noexcept -> double;
    auto currentTrackId() const noexcept -> int64_t;
    auto currentPositionLocked() const noexcept -> double;
    auto shuffle() const noexcept -> bool;
    auto repeat() const noexcept -> kanariv::types::RepeatMode;
    auto activeQueueId() const noexcept -> int64_t;

    // shuffle / repeat
    auto shufflePermFor(int64_t qid) noexcept -> std::vector<int64_t>;
    void noteEnqueued(int64_t qid, std::size_t count) noexcept;
    auto setShuffle(bool on) -> ExpectedVoid;
    auto setShuffleLocked(bool on) -> std::expected<void, kanariv::utils::Error>;
    auto setRepeat(kanariv::types::RepeatMode m) -> ExpectedVoid;

    // library / history
    auto stats() const -> std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error>;
    auto listHistory(int limit) const
        -> std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>;
    auto clearHistory() const -> std::expected<void, kanariv::utils::Error>;
    auto lastError() const -> std::string;

    // queue traversal
    auto queueNextLocked(kanariv::types::Track& out) -> std::expected<void, kanariv::utils::Error>;
    auto queuePrevLocked(kanariv::types::Track& out) -> std::expected<void, kanariv::utils::Error>;
    auto doPlayTrack(kanariv::types::Track& t, double startSec = 0.0)
        -> std::expected<void, kanariv::utils::Error>;
    void preroll() const;

    // callbacks / events
    auto setCallbacks(const kanariv::types::EngineCallbacks& cbs) -> ExpectedVoid;
    auto pollEvent() -> std::expected<kanariv::types::EngineEvent, kanariv::utils::Error>;
    auto drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n) -> ExpectedVoid;
    auto drainAll() -> std::vector<kanariv::types::EngineEvent>;

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
    std::atomic<float> preMute_{defaultPreMuteVolume};
    std::atomic<kanariv::types::PlaybackState> playbackState_{
        kanariv::types::PlaybackState::Stopped};
    std::chrono::steady_clock::time_point playStart_;
    double pausePos_{0};
    std::atomic<uint64_t> accumulatedPlaybackMs_{0};
    std::atomic<uint64_t> lastPlaybackSampleMs_{0};
    std::atomic<bool> markedPlayed_{false};
    int64_t startedMs_{0};
    std::string lastErr_;

    std::shared_ptr<kanariv::contracts::IDatabase> db_;

    // player
    std::unique_ptr<kanariv::media::Reader> reader_;
    std::unique_ptr<kanariv::media::Decoder> decoder_;
    std::unique_ptr<kanariv::utils::SpscRing<float>> ring_;
    std::unique_ptr<kanariv::player::AudioOutput> output_;

    std::jthread decodeThread_{};
    std::atomic<bool> decodeRun_{false};
    std::mutex decodeMtx_;
    std::condition_variable_any decodeCv_{};

    // monitor
    std::jthread monitorThread_{};
    std::atomic<bool> monRun_{false};
    std::mutex monMtx_;
    std::condition_variable_any monCv_{};

    // events
    kanariv::utils::MpscQueue<kanariv::types::EngineEvent> eventQueue_{eventQueueCapacity};
    std::mutex cbMutex_;
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
    mutable std::timed_mutex queueMutex_;
};

Engine::Engine(const kanariv::types::EngineConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

auto Engine::attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db)
    -> Engine::ExpectedVoid {
    return impl_->attachDatabase(std::move(db));
}

void Engine::shutdown() {
    impl_->shutdown();
}

auto Engine::play(int64_t queueId) -> Engine::ExpectedVoid {
    return impl_->play(queueId);
}

auto Engine::playTrack(int64_t trackId) -> Engine::ExpectedVoid {
    return impl_->playTrack(trackId);
}

auto Engine::pause() -> Engine::ExpectedVoid {
    return impl_->pause();
}

auto Engine::resume() -> Engine::ExpectedVoid {
    return impl_->resume();
}

auto Engine::stop() -> Engine::ExpectedVoid {
    return impl_->stop();
}

auto Engine::seek(double seconds) -> Engine::ExpectedVoid {
    return impl_->seek(seconds);
}

auto Engine::suspendCurrentTrack() -> Engine::ExpectedVoid {
    return impl_->suspendCurrentTrack();
}

auto Engine::resumeAfterTagWrite(const kanariv::types::Track& t, double pos, bool wasPaused)
    -> Engine::ExpectedVoid {
    return impl_->resumeAfterTagWrite(t, pos, wasPaused);
}

auto Engine::setVolume(float g) -> Engine::ExpectedVoid {
    return impl_->setVolume(g);
}

auto Engine::volume() const noexcept -> float {
    return impl_->volume();
}

auto Engine::preMuteVolume() const noexcept -> float {
    return impl_->preMuteVolume();
}

auto Engine::setPreMuteVolume(float g) -> Engine::ExpectedVoid {
    return impl_->setPreMuteVolume(g);
}

auto Engine::state() const noexcept -> kanariv::types::PlaybackState {
    return impl_->state();
}

auto Engine::duration() const noexcept -> double {
    return impl_->duration();
}

auto Engine::position() const noexcept -> double {
    return impl_->position();
}

auto Engine::currentTrackId() const noexcept -> int64_t {
    return impl_->currentTrackId();
}

auto Engine::shuffle() const noexcept -> bool {
    return impl_->shuffle();
}

auto Engine::shufflePermFor(int64_t qid) noexcept -> std::vector<int64_t> {
    return impl_->shufflePermFor(qid);
}

void Engine::noteEnqueued(int64_t qid, std::size_t count) noexcept {
    impl_->noteEnqueued(qid, count);
}

auto Engine::repeat() const noexcept -> kanariv::types::RepeatMode {
    return impl_->repeat();
}

auto Engine::stats() -> std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> {
    return impl_->stats();
}

auto Engine::listHistory(int limit)
    -> std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error> {
    return impl_->listHistory(limit);
}

auto Engine::clearHistory() -> std::expected<void, kanariv::utils::Error> {
    return impl_->clearHistory();
}

auto Engine::setShuffle(bool on) -> Engine::ExpectedVoid {
    return impl_->setShuffle(on);
}

auto Engine::setRepeat(kanariv::types::RepeatMode m) -> Engine::ExpectedVoid {
    return impl_->setRepeat(m);
}

auto Engine::activeQueueId() const noexcept -> int64_t {
    return impl_->activeQueueId();
}

auto Engine::switchQueue(int64_t qid) -> Engine::ExpectedVoid {
    return impl_->switchQueue(qid);
}

auto Engine::next() -> Engine::ExpectedVoid {
    return impl_->next();
}

auto Engine::autoNext() -> Engine::ExpectedVoid {
    return impl_->autoNext();
}

auto Engine::prev() -> Engine::ExpectedVoid {
    return impl_->prev();
}

auto Engine::setCallbacks(const kanariv::types::EngineCallbacks& cbs) -> Engine::ExpectedVoid {
    return impl_->setCallbacks(cbs);
}

auto Engine::pollEvent() -> std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> {
    return impl_->pollEvent();
}

auto Engine::drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n)
    -> Engine::ExpectedVoid {
    return impl_->drainEvents(buf, cap, n);
}

auto Engine::drainAll() -> std::vector<kanariv::types::EngineEvent> {
    return impl_->drainAll();
}

auto Engine::create(const kanariv::types::EngineConfig& cfg) -> Engine::ExpectedEngine {
    auto e = std::unique_ptr<Engine>(new Engine(cfg));
    auto err = e->impl_->init();
    if (err) {
        return std::unexpected(*err);
    }
    if (cfg.logger != nullptr) {
        cfg.logger->debug("engine created monitor={}", cfg.enableMonitorThread);
    }
    return e;
}

auto Engine::Impl::attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db)
    -> Engine::ExpectedVoid {
    if (!db || !db->isOpen()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db"));
    }
    db_ = std::move(db);
    if (auto ec = loadState()) {
        return std::unexpected(*ec);
    }
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
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
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

auto Engine::Impl::play(int64_t queueId) -> Engine::ExpectedVoid {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    if (queueId == 0) {
        queueId = 1;
    }
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Paused) {
        // resume, don't dequeue
        playStart_ = std::chrono::steady_clock::now();
        lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        if (output_) {
            output_->start();
        }
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
            if (!r) {
                return std::unexpected(r.error());
            }
        }
        if (ring_) {
            ring_->reset();
        }
        pausePos_ = 0;
        playStart_ = std::chrono::steady_clock::now();
        resetPlaybackAccounting();
        if (output_) {
            output_->start();
        }
        lk.unlock();
        decodeCv_.notify_all();
        monCv_.notify_all();
        return {};
    } // end if Playing

    // Stopped -> resume persisted track if config enabled, else start new via cursor
    if (!tryLockQueueFor(queueLockWait)) {
        return queueBusy("play", "queue busy");
    }
    queue_.queue_id = queueId;
    kanariv::types::Track t;
    // Resume persisted track if config enabled and track available
    if (cfg_.autoResumeOnPlay && state_.currentTrackId != 0 && state_.positionMs > 0) {
        auto idx = db_->queueIndexOf(queue_.queue_id, state_.currentTrackId);
        if (idx) {
            queue_.cursor = *idx;
            auto r = queueNextLocked(t);
            if (r) {
                auto pr = doPlayTrack(t, static_cast<double>(state_.positionMs) / msToSecDivisor);
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
    if (t.id == state_.currentTrackId && state_.positionMs > 0) {
        from = static_cast<double>(state_.positionMs) / msToSecDivisor;
    }
    auto pr = doPlayTrack(t, from);
    unlockQueue();
    if (!pr) {
        return std::unexpected(pr.error());
    }
    return {};
}

auto Engine::Impl::playTrack(int64_t trackId) -> Engine::ExpectedVoid {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    if (trackId <= 0) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad track id"));
    }
    if (!tryLockQueueFor(queueLockWait)) {
        return queueBusy("playTrack", "queue busy");
    }
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
            if (queue_.perm[i] ==
                static_cast<int64_t>(
                    *idx)) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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

auto Engine::Impl::pause() -> Engine::ExpectedVoid {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Playing) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not playing"));
    }
    // record pause position
    pausePos_ = currentPositionLocked();
    accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    if (output_) {
        output_->stop();
    }
    if (hasDb()) {
        (void)saveState();
    }
    return {};
}

auto Engine::Impl::resume() -> Engine::ExpectedVoid {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s != kanariv::types::PlaybackState::Paused) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not paused"));
    }
    playStart_ = std::chrono::steady_clock::now();
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    if (output_) {
        output_->start();
    }
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

auto Engine::Impl::stop() -> Engine::ExpectedVoid {
    auto s = playbackState_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Stopped) {
        return {};
    }
    if (s == kanariv::types::PlaybackState::Playing) {
        accountPlaybackTime();
    }
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
    if (hasDb()) {
        (void)saveState();
    }
    return {};
}

auto Engine::Impl::seek(double seconds) -> Engine::ExpectedVoid {
    if (!hasCurrent_.load(std::memory_order_acquire)) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no track"));
    }
    if (!std::isfinite(seconds) || seconds < 0) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad seconds"));
    }

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
    seconds = std::min(seconds, dur);
    pausePos_ = seconds;
    playStart_ = std::chrono::steady_clock::now();
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    if (ring_) {
        ring_->reset();
    }

    // Resume decode thread
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    decodeCv_.notify_all();
    if (hasDb()) {
        (void)saveState();
    }
    return {};
}

auto Engine::Impl::suspendCurrentTrack() -> Engine::ExpectedVoid {
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
    if (output_) {
        output_->stop();
    }
    if (ring_) {
        ring_->reset();
    }
    decoder_.reset();
    reader_.reset();
    lk.unlock();
    unlockQueue();
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

auto Engine::Impl::resumeAfterTagWrite(const kanariv::types::Track& t, double pos, bool wasPaused)
    -> Engine::ExpectedVoid {
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

auto Engine::Impl::setVolume(float g) -> Engine::ExpectedVoid {
    g = kanariv::utils::clampVolume(g);
    state_.volume = g;
    volume_.store(g, std::memory_order_relaxed);
    if (output_) {
        output_->setVolume(g);
    }
    if (hasDb()) {
        // Throttle persistence (M-3): a volume drag emits dozens of
        // events/s; each saveState() is a WAL transaction rewriting the
        // shuffle BLOB. Persist at most 1x/s here -- shutdown/track-change
        // paths always persist, so the crash window is <= 1 s of volume.
        const uint64_t now = detail::nowMs();
        const uint64_t last = lastVolSaveMs_.load(std::memory_order_acquire);
        if (now - last >= volumeSaveIntervalMs) {
            lastVolSaveMs_.store(now, std::memory_order_release);
            state_.cursorPos = static_cast<int64_t>(queue_.cursor);
            (void)saveState();
        }
    }
    return {};
}

auto Engine::Impl::volume() const noexcept -> float {
    return volume_.load(std::memory_order_relaxed);
}

auto Engine::Impl::setPreMuteVolume(float g) -> Engine::ExpectedVoid {
    if (!(g > 0.0f) || g > 1.0f) {
        g = defaultPreMuteVolume;
    }
    state_.preMuteVolume = g;
    preMute_.store(g, std::memory_order_relaxed);
    // Persisted by the setVolume() call that always follows in mute flows.
    return {};
}

auto Engine::Impl::preMuteVolume() const noexcept -> float {
    return preMute_.load(std::memory_order_relaxed);
}

auto Engine::Impl::state() const noexcept -> kanariv::types::PlaybackState {
    return playbackState_.load(std::memory_order_acquire);
}

auto Engine::Impl::duration() const noexcept -> double {
    return duration_;
}

auto Engine::Impl::position() const noexcept -> double {
    return currentPositionLocked();
}

auto Engine::Impl::currentTrackId() const noexcept -> int64_t {
    if (hasCurrent_.load(std::memory_order_acquire)) {
        return currentTrack_.id;
    }
    return state_.currentTrackId;
}

auto Engine::Impl::shuffle() const noexcept -> bool {
    return queue_.shuffle;
}

auto Engine::Impl::repeat() const noexcept -> kanariv::types::RepeatMode {
    return queue_.repeat;
}

auto Engine::Impl::shufflePermFor(int64_t qid) noexcept -> std::vector<int64_t> {
    if (!tryLockQueue()) {
        return {};
    }
    std::vector<int64_t> out;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        out = queue_.perm;
    }
    unlockQueue();
    return out;
}

void Engine::Impl::noteEnqueued(int64_t qid, std::size_t count) noexcept {
    if (count == 0 || !hasDb() || !tryLockQueue()) {
        return;
    }
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        std::size_t n = db_->queueCount(qid);
        std::size_t start = (n >= count) ? n - count : 0;
        for (std::size_t i = start; i < n; ++i) {
            queue_.perm.push_back(static_cast<int64_t>(i));
        }
        (void)persistShuffleBlobLocked();
    }
    unlockQueue();
}

auto Engine::Impl::stats() const
    -> std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    auto ds = db_->playbackStats();
    if (!ds) {
        return std::unexpected(ds.error());
    }
    return kanariv::types::PlaybackStats{
        .num_tracks = ds->num_tracks,
        .num_playlists = ds->num_playlists,
        .num_queue_items = ds->num_queue_items,
        .num_history = ds->num_history,
        .num_bookmarks = ds->num_bookmarks,
        .num_libraries = ds->num_libraries,
        .total_duration_ms = ds->total_duration_ms,
    };
}

auto Engine::Impl::listHistory(int limit) const
    -> std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    kanariv::engine::History hist(db_);
    return hist.listHistory(limit);
}

auto Engine::Impl::clearHistory() const -> std::expected<void, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    kanariv::engine::History hist(db_);
    return hist.clearHistory();
}

auto Engine::Impl::lastError() const -> std::string {
    if (!lastErr_.empty()) {
        return lastErr_;
    }
    return "";
}

auto Engine::Impl::setShuffle(bool on) -> Engine::ExpectedVoid {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    if (!tryLockQueue()) {
        return queueBusy("setShuffle", "busy");
    }
    auto r = setShuffleLocked(on);
    unlockQueue();
    if (!r) {
        return std::unexpected(r.error());
    }
    // push queue changed event
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::QueueChanged;
    ev.queue_id = queue_.queue_id;
    pushEvent(ev);
    return {};
}

auto Engine::Impl::setRepeat(kanariv::types::RepeatMode m) -> Engine::ExpectedVoid {
    if (m != kanariv::types::RepeatMode::Off && m != kanariv::types::RepeatMode::All &&
        m != kanariv::types::RepeatMode::One) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "bad repeat"));
    }
    if (!tryLockQueue()) {
        return queueBusy("setRepeat", "busy");
    }
    queue_.repeat = m;
    state_.repeatMode = m;
    state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    std::optional<kanariv::utils::Error> err;
    if (auto e = saveState(); !e) {
        err = e.error();
    }
    unlockQueue();
    if (err) {
        return std::unexpected(*err);
    }
    return {};
}

auto Engine::Impl::activeQueueId() const noexcept -> int64_t {
    std::scoped_lock lk(queueMutex_);
    return queue_.queue_id != 0        ? queue_.queue_id
           : state_.activeQueueId != 0 ? state_.activeQueueId
                                       : 1;
}

auto Engine::Impl::switchQueue(int64_t qid) -> Engine::ExpectedVoid {
    if (qid == 0) {
        qid = 1;
    }
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    if (!tryLockQueue()) {
        return queueBusy("switchQueue", "busy");
    }
    // Validate the queue while holding queueMutex_.
    auto exists = db_->queueExists(qid);
    if (!exists || !*exists) {
        unlockQueue();
        if (!exists) {
            return std::unexpected(exists.error());
        }
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
    if (auto e = saveState(); !e) {
        err = e.error();
    }
    // also persist via saveState includes active_queue_id, cursor_pos, shuffle_perm cleared
    unlockQueue();
    if (err) {
        return std::unexpected(*err);
    }
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::QueueChanged;
    ev.queue_id = qid;
    pushEvent(ev);
    return {};
}

auto Engine::Impl::next() -> Engine::ExpectedVoid {
    return advanceLocked(false, static_cast<int>(queueLockWait.count()));
}

auto Engine::Impl::autoNext() -> Engine::ExpectedVoid {
    return advanceLocked(true);
}

auto Engine::Impl::advanceLocked(bool stopAtEnd, int lockWaitMs) -> Engine::ExpectedVoid {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    // handle repeat one without shuffle without queue lock? match C: if !shuffle && repeat==One
    // && hasCurrent => seek 0 and play - serialize with decodeLoop via decodeMtx_
    if (!queue_.shuffle && queue_.repeat == kanariv::types::RepeatMode::One &&
        hasCurrent_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            if (decoder_) {
                auto r = decoder_->seek(0);
                if (!r) {
                    return std::unexpected(r.error());
                }
            }
            if (ring_) {
                ring_->reset();
            }
        }
        pausePos_ = 0;
        playStart_ = std::chrono::steady_clock::now();
        playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        resetPlaybackAccounting();
        markedPlayed_.store(false, std::memory_order_release);
        gaplessArmed_.store(false, std::memory_order_release);
        startedMs_ = static_cast<int64_t>(detail::wallMs());
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
        if (lockWaitMs > 0) {
            return queueBusy("next", "busy");
        }
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
            if (output_) {
                output_->stop();
            }
            if (ring_) {
                ring_->reset();
            }
            pausePos_ = 0;
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
    if (!pr) {
        return std::unexpected(pr.error());
    }
    return {};
}

auto Engine::Impl::prev() -> Engine::ExpectedVoid {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    if (!tryLockQueueFor(queueLockWait)) {
        return queueBusy("prev", "busy");
    }
    kanariv::types::Track t;
    auto r = queuePrevLocked(t);
    if (!r) {
        unlockQueue();
        return std::unexpected(r.error());
    }
    auto pr = doPlayTrack(t);
    unlockQueue();
    if (!pr) {
        return std::unexpected(pr.error());
    }
    return {};
}

auto Engine::Impl::setCallbacks(const kanariv::types::EngineCallbacks& cbs)
    -> Engine::ExpectedVoid {
    std::scoped_lock lk(cbMutex_);
    callbacks_ = cbs;
    cfg_.callbacks = cbs;
    return {};
}

auto Engine::Impl::pollEvent()
    -> std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> {
    auto r = eventQueue_.pop();
    if (!r) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no event"));
    }
    return r.value();
}

auto Engine::Impl::drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n)
    -> Engine::ExpectedVoid {
    if (n == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null n"));
    }
    *n = 0;
    if (buf == nullptr && cap != 0) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null buf"));
    }
    size_t got = 0;
    while (got < cap) {
        auto r = eventQueue_.pop();
        if (!r) {
            break;
        }
        buf[got++] = r.value();
    }
    *n = got;
    return {};
}

auto Engine::Impl::drainAll() -> std::vector<kanariv::types::EngineEvent> {
    std::vector<kanariv::types::EngineEvent> out;
    while (true) {
        auto r = eventQueue_.pop();
        if (!r) {
            break;
        }
        out.push_back(r.value());
    }
    return out;
}

Engine::Impl::Impl(const kanariv::types::EngineConfig& cfg) : cfg_(cfg), state_{}, queue_{} {
    cfg_.pollMs = cfg.pollMs != 0 ? cfg.pollMs : defaultPollMs;
    cfg_.gaplessMs = cfg.gaplessMs != 0 ? cfg.gaplessMs : defaultGaplessMs;
    cfg_.historyThresholdPct =
        cfg.historyThresholdPct != 0 ? cfg.historyThresholdPct : defaultHistoryThresholdPct;
    cfg_.historyThresholdSecs =
        cfg.historyThresholdSecs != 0 ? cfg.historyThresholdSecs : defaultHistoryThresholdSecs;
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
    // callbacks from config
    callbacks_ = cfg.callbacks;
}

auto Engine::Impl::init() -> std::optional<kanariv::utils::Error> {
    // start decode thread
    decodeRun_.store(true, std::memory_order_release);
    decodeThread_ = std::jthread([this](std::stop_token st) -> void { decodeLoop(std::move(st)); });
    // start monitor thread if enabled
    if (cfg_.enableMonitorThread) {
        monRun_.store(true, std::memory_order_release);
        monitorThread_ =
            std::jthread([this](std::stop_token st) -> void { monitorLoop(std::move(st)); });
    }
    return std::nullopt;
}

auto Engine::Impl::hasDb() const noexcept -> bool {
    return db_ && db_->isOpen();
}

auto Engine::Impl::tryLockQueue() noexcept -> bool {
    return queueMutex_.try_lock();
}

auto Engine::Impl::tryLockQueueFor(std::chrono::milliseconds wait) noexcept -> bool {
    return queueMutex_.try_lock_for(wait);
}

void Engine::Impl::unlockQueue() noexcept {
    queueMutex_.unlock();
}

auto Engine::Impl::queueBusy(std::string_view op, std::string_view msg) const
    -> std::unexpected<kanariv::utils::Error> {
    if (cfg_.logger != nullptr) {
        cfg_.logger->debug("queue busy op={}", op);
    }
    return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, msg)};
}

auto Engine::Impl::currentPositionLocked() const noexcept -> double {
    if (!hasCurrent_.load(std::memory_order_acquire)) {
        return static_cast<double>(state_.positionMs) / msToSecDivisor;
    }
    auto st = playbackState_.load(std::memory_order_acquire);
    if (st == kanariv::types::PlaybackState::Paused) {
        return pausePos_;
    }
    if (st != kanariv::types::PlaybackState::Playing) {
        return pausePos_;
    }
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - playStart_).count();
    double pos = pausePos_ + elapsed;
    pos = std::min(pos, duration_);
    pos = std::max<double>(pos, 0);
    return pos;
}

auto Engine::Impl::loadState() -> std::optional<kanariv::utils::Error> {
    if (!hasDb()) {
        return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db");
    }
    auto loaded = db_->loadEngineState();
    if (!loaded) {
        return loaded.error();
    }
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
    if (!queue_.perm.empty() && queue_.cursor >= queue_.perm.size()) {
        queue_.cursor = 0;
    }
    queue_.queue_id = state_.activeQueueId;
    auto queueExists = db_->queueExists(queue_.queue_id);
    if (!queueExists || !*queueExists) {
        queue_.queue_id = 1;
        state_.activeQueueId = 1;
    }
    return std::nullopt;
}

auto Engine::Impl::saveState() -> std::expected<void, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    }
    kanariv::types::EngineState persisted;
    persisted.shuffle_enabled = state_.shuffleEnabled;
    persisted.repeat_mode = state_.repeatMode;
    persisted.cursor_pos = state_.cursorPos;
    persisted.current_track_id = state_.currentTrackId;
    persisted.volume = state_.volume;
    persisted.active_queue_id = state_.activeQueueId != 0 ? state_.activeQueueId : queue_.queue_id;
    persisted.pre_mute_volume = state_.preMuteVolume;
    persisted.position_ms = hasCurrent_.load(std::memory_order_acquire)
                                ? static_cast<int64_t>(currentPositionLocked() * msToSecDivisor)
                                : state_.positionMs;
    persisted.position_ms = std::max<int64_t>(persisted.position_ms, 0);
    persisted.shuffle_perm = queue_.perm;
    auto result = db_->saveEngineState(persisted);
    if (result) {
        state_.positionMs = persisted.position_ms;
    }
    return result;
}

auto Engine::Impl::persistShuffleBlobLocked() -> std::expected<void, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    }
    auto result =
        db_->saveEngineShuffle(queue_.shuffle, queue_.perm, static_cast<int64_t>(queue_.cursor));
    if (result) {
        state_.shuffleEnabled = queue_.shuffle ? 1 : 0;
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    }
    return result;
}

auto Engine::Impl::persistCursorLocked() -> std::expected<void, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    }
    auto result = db_->saveEngineCursor(static_cast<int64_t>(queue_.cursor));
    if (result) {
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    }
    return result;
}

auto Engine::Impl::fetchTrackByPosLocked(int64_t qid, int64_t pos) const
    -> std::expected<kanariv::types::Track, kanariv::utils::Error> {
    if (!hasDb()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    }
    return db_->queueTrackAt(qid, pos);
}

auto Engine::Impl::setShuffleLocked(bool on) -> std::expected<void, kanariv::utils::Error> {
    // assert: queueMutex_ locked by caller
    bool want = on;
    if (queue_.shuffle == want && !queue_.perm.empty()) {
        return {};
    }
    if (want) {
        queue_.perm.clear();
        queue_.cursor = 0;
        size_t cnt = db_->queueCount(queue_.queue_id);
        if (cnt == 0) {
            queue_.shuffle = true;
            queue_.perm.clear();
            queue_.cursor = 0;
            auto r = persistShuffleBlobLocked();
            if (!r) {
                return r;
            }
            state_.shuffleEnabled = 1;
            return {};
        }
        queue_.perm.resize(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            queue_.perm[i] = static_cast<int64_t>(
                i); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        {
            std::mt19937 rng{std::random_device{}()};
            detail::shufflePerm(queue_.perm, rng);
        }
        queue_.shuffle = true;
        queue_.cursor = 0;
        auto r = persistShuffleBlobLocked();
        if (!r) {
            return r;
        }
        state_.shuffleEnabled = 1;
        return {};
    } else {
        queue_.perm.clear();
        queue_.cursor = 0;
        queue_.shuffle = false;
        auto r = persistShuffleBlobLocked();
        if (!r) {
            return r;
        }
        state_.shuffleEnabled = 0;
        return {};
    }
}

auto Engine::Impl::queueNextLocked(kanariv::types::Track& out)
    -> std::expected<void, kanariv::utils::Error> {
    if (queue_.queue_id == 0) {
        queue_.queue_id = 1;
    }
    if (queue_.shuffle) {
        if (queue_.perm.empty()) {
            size_t cnt = db_->queueCount(queue_.queue_id);
            if (cnt == 0) {
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
            }
            auto sr = setShuffleLocked(true);
            if (!sr) {
                return std::unexpected(sr.error());
            }
            if (queue_.perm.empty()) {
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no perm"));
            }
        }
        if (queue_.cursor >= queue_.perm.size()) {
            if (queue_.repeat == kanariv::types::RepeatMode::One) {
                size_t idx = queue_.perm.size() - 1;
                if (queue_.cursor > 0 && queue_.cursor <= queue_.perm.size()) {
                    idx = queue_.cursor - 1;
                }
                int64_t pos =
                    queue_.perm
                        [idx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
                if (!tr) {
                    return std::unexpected(tr.error());
                }
                out = tr.value();
                return {};
            } else {
                // wrap/reshuffle for both Off and Queue (shuffle on => new perm)
                auto sr = setShuffleLocked(true);
                if (!sr) {
                    return std::unexpected(sr.error());
                }
                queue_.cursor = 0;
                (void)persistCursorLocked();
            }
        }
        int64_t pos =
            queue_.perm
                [queue_
                     .cursor]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        queue_.cursor++;
        (void)persistCursorLocked();
        auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
        if (!tr) {
            return std::unexpected(tr.error());
        }
        out = tr.value();
        return {};
    }
    size_t cnt = db_->queueCount(queue_.queue_id);
    if (cnt == 0) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
    }
    if (queue_.cursor >= cnt) {
        if (queue_.repeat == kanariv::types::RepeatMode::One) {
            size_t idx = cnt - 1;
            if (queue_.cursor > 0 && queue_.cursor <= cnt) {
                idx = queue_.cursor - 1;
            }
            auto tr = fetchTrackByPosLocked(queue_.queue_id, static_cast<int64_t>(idx));
            if (!tr) {
                return std::unexpected(tr.error());
            }
            out = tr.value();
            return {};
        } else {
            // wrap for both Off and Queue; reshuffle if shuffle on
            if (queue_.shuffle) {
                auto sr = setShuffleLocked(true);
                if (!sr) {
                    return std::unexpected(sr.error());
                }
                queue_.cursor = 0;
            } else {
                queue_.cursor = 0;
            }
            (void)persistCursorLocked();
        }
    }
    auto tr = fetchTrackByPosLocked(queue_.queue_id, static_cast<int64_t>(queue_.cursor));
    if (!tr) {
        return std::unexpected(tr.error());
    }
    queue_.cursor++;
    (void)persistCursorLocked();
    out = tr.value();
    return {};
}

auto Engine::Impl::queuePrevLocked(kanariv::types::Track& out)
    -> std::expected<void, kanariv::utils::Error> {
    if (!queue_.shuffle) {
        size_t cnt = db_->queueCount(queue_.queue_id);
        if (cnt == 0) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
        }
        if (queue_.cursor <= 1) {
            if (queue_.cursor == 0) {
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "at start"));
            }
            queue_.cursor = 0;
        } else {
            queue_.cursor -= 2;
            if (queue_.cursor >= cnt) {
                queue_.cursor = cnt - 1;
            }
        }
        auto tr = fetchTrackByPosLocked(queue_.queue_id, static_cast<int64_t>(queue_.cursor));
        if (!tr) {
            return std::unexpected(tr.error());
        }
        queue_.cursor++;
        (void)persistCursorLocked();
        out = tr.value();
        return {};
    }
    if (queue_.perm.empty()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no perm"));
    }
    if (queue_.cursor <= 1) {
        if (queue_.cursor == 0) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "at start"));
        }
        queue_.cursor = 0;
    } else {
        queue_.cursor -= 2;
        if (queue_.cursor >= queue_.perm.size()) {
            queue_.cursor = queue_.perm.size() - 1;
        }
    }
    int64_t pos =
        queue_.perm
            [queue_
                 .cursor]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    queue_.cursor++;
    (void)persistCursorLocked();
    auto tr = fetchTrackByPosLocked(queue_.queue_id, pos);
    if (!tr) {
        return std::unexpected(tr.error());
    }
    out = tr.value();
    return {};
}

auto Engine::Impl::doPlayTrack(kanariv::types::Track& t, double startSec)
    -> std::expected<void, kanariv::utils::Error> {
    // try to open decoder path
    std::string path = t.path;
    if (!std::isfinite(startSec) || startSec < 0.0) {
        startSec = 0.0;
    }
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
        auto rRes = kanariv::media::FileReader::open(path);
        if (rRes) {
            reader_ = std::move(rRes.value());
            auto dRes = kanariv::media::Decoder::open(*reader_, cfg_.logger);
            if (dRes) {
                decoder_ = std::move(dRes.value());
                opened = true;
                duration_ = decoder_->totalFrames() != 0
                                ? static_cast<double>(decoder_->totalFrames()) /
                                      static_cast<double>(decoder_->sampleRate())
                                : t.duration;
                if (duration_ <= 0) {
                    duration_ = t.duration > 0 ? t.duration : 1.0;
                }
                // Resume: seek before the ring exists and is prerolled, so the
                // device starts at the requested offset. Seeking after
                // output_->start() leaks the ring's preroll (4096 frames,
                // ~93 ms) of the track head into playback.
                if (startSec > 0.0) {
                    double target = startSec;
                    if (duration_ > 0 && target > duration_) {
                        target = duration_;
                    }
                    auto sRes = decoder_->seek(target);
                    if (!sRes) {
                        if (cfg_.logger != nullptr) {
                            cfg_.logger->warn("resume seek failed file={} pos={} err={}", path,
                                              target, sRes.error().message);
                        }
                        startSec = 0.0;
                    } else if (cfg_.logger != nullptr) {
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
                if (sr == 0) {
                    sr = defaultSampleRate;
                }
                if (ch == 0) {
                    ch = defaultChannels;
                }
                ring_ = std::make_unique<kanariv::utils::SpscRing<float>>(
                    ringBufferFramesPerChannel * ch, ch);
                kanariv::player::AudioOutput::Config cfg;
                cfg.sampleRate = sr;
                cfg.channels = ch;
                cfg.ring = ring_.get();
                cfg.volume = volume_.load(std::memory_order_relaxed);
                cfg.logger = cfg_.logger;
                auto oRes = kanariv::player::AudioOutput::create(cfg);
                if (oRes) {
                    output_ = std::move(oRes.value());
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
            if (cfg_.logger != nullptr) {
                cfg_.logger->warn("track not playable file={} err={}", path, msg);
            }
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, msg)};
        }
        // timer-only fallback to track metadata duration (no audio output)
        if (cfg_.logger != nullptr) {
            cfg_.logger->warn("simulated playback (no audio) file={}", path);
        }
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
            if (cfg_.logger != nullptr) {
                cfg_.logger->warn("audio device unavailable file={} err={}", path, msg);
            }
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Device, msg)};
        }
    }
    currentTrack_ = t;
    // update state
    hasCurrent_.store(true, std::memory_order_release);
    markedPlayed_.store(false, std::memory_order_release);
    gaplessArmed_.store(false, std::memory_order_release);
    startedMs_ = static_cast<int64_t>(detail::wallMs());
    state_.currentTrackId = t.id;
    state_.cursorPos = static_cast<int64_t>(queue_.cursor);
    playbackState_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    // Position tracking starts at the resumed offset (0 for next/prev);
    // saveState() below then persists that offset instead of a transient 0.
    // pausePos_ already holds the offset, so playStart_ must be "now" --
    // shifting it back too would count the offset twice.
    pausePos_ = startSec;
    playStart_ = std::chrono::steady_clock::now();
    if (hasDb()) {
        (void)saveState();
    }
    kanariv::types::EngineEvent ev;
    ev.type = kanariv::types::EngineEventType::TrackStarted;
    ev.track_id = t.id;
    ev.queue_id = queue_.queue_id;
    ev.duration = duration_;
    ev.position = 0;
    pushEvent(ev);
    decodeCv_.notify_all();
    monCv_.notify_all();
    if (cfg_.logger != nullptr) {
        cfg_.logger->debug("playing track={} file={}", t.id, path);
    }
    return {};
}

void Engine::Impl::preroll() const {
    if (!decoder_ || !ring_) {
        return;
    }
    uint32_t ch = decoder_->channels();
    if (ch == 0) {
        ch = 2;
    }
    // preroll cap/2 frames like Player (ring capacity is in frames)
    size_t need = ring_->availableWrite() / 2;
    if (need == 0) {
        return;
    }
    size_t chunkFrames = decodeChunkFrames;
    size_t maxChunk = maxDecodeChunkFrames / ch;
    chunkFrames = std::min(chunkFrames, maxChunk);
    std::vector<float> tmp(chunkFrames * ch);
    while (need > 0 && ring_->availableWrite() >= tmp.size() / ch) {
        size_t frames = decoder_->decode(std::span<float>(tmp.data(), tmp.size()));
        if (frames == 0) {
            break;
        }
        size_t samples = frames * ch;
        size_t writtenFrames = ring_->write(std::span<float>(tmp.data(), samples));
        if (writtenFrames < frames) {
            break;
        }
        if (need > frames) {
            need -= frames;
        } else {
            break;
        }
    }
}

void Engine::Impl::pushEvent(const kanariv::types::EngineEvent& ev) {
    // MpscQueue cap 64 drop policy
    auto r = eventQueue_.push(ev);
    if (!r) {
        return; // drop if full
    }
    // dispatch callbacks outside queue lock
    kanariv::types::EngineCallbacks cbsCopy;
    {
        std::scoped_lock lk(cbMutex_);
        cbsCopy = callbacks_;
    }
    if (ev.type == kanariv::types::EngineEventType::TrackStarted && cbsCopy.on_track_started) {
        cbsCopy.on_track_started(ev.track_id);
    } else if (ev.type == kanariv::types::EngineEventType::TrackEnded && cbsCopy.on_track_ended) {
        double pct = 0;
        if (ev.duration > 0) {
            pct = (ev.position / ev.duration) * pctScale;
            pct = std::max<double>(pct, 0);
            pct = std::min<double>(pct, pctMax);
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
    if (last == 0 || now < last) {
        return;
    }
    accumulatedPlaybackMs_.fetch_add(now - last, std::memory_order_acq_rel);
}

void Engine::Impl::doHistoryMark() {
    if (!hasDb()) {
        return;
    }
    if (!hasCurrent_.load(std::memory_order_acquire)) {
        return;
    }
    if (markedPlayed_.load(std::memory_order_acquire)) {
        return;
    }
    double dur = duration_;
    const double played =
        static_cast<double>(accumulatedPlaybackMs_.load(std::memory_order_acquire)) / 1000.0;
    int pct = cfg_.historyThresholdPct != 0 ? cfg_.historyThresholdPct : defaultHistoryThresholdPct;
    int secs =
        cfg_.historyThresholdSecs != 0 ? cfg_.historyThresholdSecs : defaultHistoryThresholdSecs;
    if (!detail::shouldMarkPlayed(dur, played, false, pct, secs)) {
        return;
    }
    bool expected = false;
    bool desired = true;
    // CAS exactly-once
    bool exchanged = markedPlayed_.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    if (!exchanged) {
        return;
    }
    auto completion = static_cast<int64_t>(detail::wallMs());
    double completionPct = 0.0;
    const double pos = currentPositionLocked();
    if (dur > 0.0) {
        completionPct = (pos / dur) * 100.0;
        completionPct = std::min(completionPct, 100.0);
    }
    auto result = db_->markPlayed(currentTrack_.id, startedMs_, completion,
                                  static_cast<int64_t>(pos * msToSecDivisor), completionPct,
                                  queue_.queue_id != 0 ? queue_.queue_id : 1);
    if (!result) {
        markedPlayed_.store(false, std::memory_order_release);
    }
}

void Engine::Impl::engineTick() {
    if (playbackState_.load(std::memory_order_acquire) == kanariv::types::PlaybackState::Playing) {
        accountPlaybackTime();
    }
    doHistoryMark();
    auto st = playbackState_.load(std::memory_order_acquire);
    if (st == kanariv::types::PlaybackState::Playing) {
        uint64_t now = detail::nowMs();
        uint64_t last = lastProgressMs_.load(std::memory_order_acquire);
        if (now - last >= progressCheckIntervalMs) {
            lastProgressMs_.store(now, std::memory_order_release);
            kanariv::types::EngineEvent ev;
            ev.type = kanariv::types::EngineEventType::Progress;
            ev.track_id = hasCurrent_.load(std::memory_order_acquire) ? currentTrack_.id
                                                                      : state_.currentTrackId;
            ev.queue_id = queue_.queue_id != 0 ? queue_.queue_id : 1;
            ev.position = currentPositionLocked();
            ev.duration = duration_;
            pushEvent(ev);
        }
        if (duration_ > 0) {
            double pos = currentPositionLocked();
            double remaining = duration_ - pos;
            double gapS = static_cast<double>(cfg_.gaplessMs) / msToSecDivisor;
            if (remaining <= gapS && remaining >= 0.0) {
                if (hasCurrent_.load(std::memory_order_acquire)) {
                    // gaplessArmed 0->1 CAS, 300ms preroll
                    bool expected = false;
                    if (gaplessArmed_.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        auto nr = autoNext();
                        if (!nr) {
                            gaplessArmed_.store(false, std::memory_order_release);
                        }
                    }
                }
            }
        }
        // Resume-position checkpoint (5 s crash window). Skip when the queue
        // lock is contended: another thread will save on its own path.
        uint64_t lastSave = lastPosSaveMs_.load(std::memory_order_acquire);
        if (now - lastSave >= positionSaveIntervalMs && tryLockQueue()) {
            state_.cursorPos = static_cast<int64_t>(queue_.cursor);
            if (saveState()) {
                lastPosSaveMs_.store(now, std::memory_order_release);
            }
            unlockQueue();
        }
    }
}

void Engine::Impl::monitorLoop(std::stop_token st) {
    // Two-tier waits (C-1): the 10 ms cadence is only needed while playing
    // (300 ms gapless pre-roll window still gets ~12 looks at 25 ms).
    // Idle backs off to 100 ms. An explicit kanariv::types::EngineConfig::pollMs override is
    // honored for the playing interval; idle is always >= 100 ms.
    int cfgPoll = cfg_.pollMs != 0 ? cfg_.pollMs : defaultPollMs;
    if (cfgPoll <= 0) {
        cfgPoll = defaultPollMs;
    }
    const bool customPoll = (cfgPoll != defaultPollMs);
    std::unique_lock<std::mutex> lk(monMtx_);
    while (!st.stop_requested() && monRun_.load(std::memory_order_acquire)) {
        lk.unlock();
        engineTick();
        int poll;
        if (playbackState_.load(std::memory_order_acquire) ==
            kanariv::types::PlaybackState::Playing) {
            poll = customPoll ? cfgPoll : defaultPollIntervalMs;
        } else {
            poll = customPoll ? (cfgPoll > defaultIdlePollMs ? cfgPoll : defaultIdlePollMs)
                              : defaultIdlePollMs;
        }
        lk.lock();
        monCv_.wait_for(lk, std::chrono::milliseconds(poll), [&st, this] -> bool {
            return st.stop_requested() || !monRun_.load(std::memory_order_acquire);
        });
    }
}

void Engine::Impl::decodeLoop(std::stop_token st) {
    constexpr std::size_t maxChunkFrames = 1024;
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(maxChunkFrames * scratchChannels);
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
                if (ch == 0) {
                    ch = 2;
                }
                ready = true;
            }
        }
        if (!ready) {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Idle backoff (C-1): woken immediately by decodeCv_ on
            // play/seek/resume, so the longer timeout costs no latency.
            decodeCv_.wait_for(
                lk, std::chrono::milliseconds(monitorIdleWaitMs), [&st, this] -> bool {
                    return st.stop_requested() || playbackState_.load(std::memory_order_acquire) ==
                                                      kanariv::types::PlaybackState::Playing;
                });
            continue;
        }
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(decodeIdleSleepMs));
            continue;
        }
        size_t maxFrames = avail / ch;
        maxFrames = std::min(maxFrames, maxChunkFrames);
        size_t maxChunkByCh = maxDecodeChunkFrames / ch;
        maxFrames = std::min(maxFrames, maxChunkByCh);
        if (maxFrames == 0) {
            maxFrames = 1;
        }

        if (scratch.size() < maxFrames * ch) {
            scratch.resize(maxFrames * ch);
        }
        std::span<float> chunk(scratch.data(), maxFrames * ch);
        size_t frames;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Re-check state under lock to avoid race with seek()
            if (playbackState_.load(std::memory_order_acquire) !=
                    kanariv::types::PlaybackState::Playing ||
                !decoder_) {
                continue;
            }
            // A track switch may have swapped the decoder for one with a
            // different channel count since the chunk was sized.
            ch = decoder_->channels();
            if (ch == 0) {
                ch = 2;
            }
            frames = decoder_->decode(chunk);
        }
        if (frames == 0) {
            // EOF reached - wait a bit, let monitor handle next (gapless) or stop
            std::this_thread::sleep_for(std::chrono::milliseconds(eofWaitMs));
            continue;
        }
        size_t samples = frames * ch;
        size_t writtenFrames;
        {
            std::unique_lock<std::mutex> lk(decodeMtx_);
            // Re-check state under lock to avoid race with seek() ring reset
            if (playbackState_.load(std::memory_order_acquire) !=
                    kanariv::types::PlaybackState::Playing ||
                !ring_) {
                continue;
            }
            writtenFrames = ring_->write(std::span<float>(scratch.data(), samples));
        }
        if (writtenFrames < frames) {
            // Ring full, will retry next iteration
        }
    }
}

} // namespace kanariv::engine
