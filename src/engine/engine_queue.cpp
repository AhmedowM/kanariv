#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <engine/history.hpp>
#include <engine/queue_next.hpp>
#include <engine/shuffle.hpp>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine_impl.hpp"

namespace kanariv::engine {

namespace detail {

void shufflePerm(std::vector<int64_t>& perm, std::mt19937& rng) {
    std::ranges::shuffle(perm, rng);
}

void shufflePerm(std::vector<int64_t>& perm) {
    std::random_device rd;
    std::mt19937 gen(rd());
    shufflePerm(perm, gen);
}

} // namespace detail

std::vector<int64_t> Engine::Impl::shufflePermFor(int64_t qid) {
    auto qlk = tryLockQueue();
    if (!qlk)
        return {};
    std::vector<int64_t> out;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty())
        out = queue_.perm;
    return out;
}

void Engine::Impl::noteEnqueued(int64_t qid, std::size_t count) {
    if (count == 0 || !hasDb())
        return;
    auto qlk = tryLockQueue();
    if (!qlk)
        return;
    if (queue_.queue_id == qid && queue_.shuffle && !queue_.perm.empty()) {
        auto n = storage_.queues->queueCount(qid);
        if (!n)
            return;
        std::size_t start = (*n >= count) ? *n - count : 0;
        for (std::size_t i = start; i < *n; ++i)
            queue_.perm.push_back(static_cast<int64_t>(i));
        saveStateBestEffort();
    }
}

void Engine::Impl::noteQueueMoved(int64_t qid, std::size_t from, std::size_t to) {
    if (from == to || !hasDb())
        return;
    auto qlk = tryLockQueue();
    if (!qlk)
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
        saveStateBestEffort();
    }
}

Engine::ExpectedVoid Engine::Impl::setShuffle(std::optional<bool> on) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("setShuffle", "busy");
    // Resolve a toggle while holding the lock: reading the mode before the
    // acquisition let two toggles observe the same value and both write it.
    const bool want = on.value_or(!queue_.shuffle.load(std::memory_order_relaxed));
    auto r = setShuffleLocked(want);
    qlk.release();
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
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
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
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("switchQueue", "busy");
    // Validate the queue while holding queueMutex_.
    auto exists = storage_.queues->queueExists(qid);
    if (!exists || !*exists) {
        if (!exists)
            return std::unexpected(exists.error());
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "queue not found"));
    }
    if (queue_.queue_id == qid) {
        // already active, ensure cursor consistent with DB state but not error
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
    qlk.release();
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
    // && hasCurrent => rewind the track through the pipeline
    if (!queue_.shuffle.load(std::memory_order_relaxed) &&
        queue_.repeat.load(std::memory_order_relaxed) == kanariv::types::RepeatMode::One &&
        hasCurrent_.load(std::memory_order_acquire)) {
        double trackDuration = 0.0;
        {
            std::lock_guard<std::mutex> lk(decodeMtx_);
            trackDuration = duration_;
        }
        // Rewind through the pipeline (one serialized transport operation): it
        // stops the device, seeks the source, clears the buffered tail and
        // restarts, so the replay starts at the head instead of playing the old
        // tail first. A clock-only graph just rebases its position.
        if (auto r = pipeline_->seek(0.0); !r)
            return std::unexpected(r.error());
        transition(kanariv::types::PlaybackState::Playing, "repeat one");
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
        return {};
    }
    // Monitor autoNext passes 0 (fail-fast; the next tick retries), user
    // next() passes the wait budget so colliding skips execute.
    auto qlk =
        lockWaitMs > 0 ? tryLockQueueFor(std::chrono::milliseconds(lockWaitMs)) : tryLockQueue();
    if (!qlk) {
        // Monitor autoNext fail-fast is routine (next tick retries) -- stay
        // silent; only user-wait collisions are diagnostic.
        if (lockWaitMs > 0)
            return queueBusy("next", "busy");
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "busy"));
    }
    if (expectedEpoch && transitionEpoch_.load(std::memory_order_acquire) != *expectedEpoch) {
        // Another driver already transitioned the track this decision was
        // taken for; advancing again would skip an entry.
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
            if (!cnt)
                return std::unexpected(cnt.error());
            atEnd = queue_.cursor >= *cnt;
        }
        if (atEnd) {
            // Natural end with repeat off: clear resume position so play()
            // doesn't seek to end-of-track and loop forever.
            state_.positionMs.store(0, std::memory_order_relaxed);
            state_.currentTrackId.store(0, std::memory_order_release);
            state_.currentPath.clear();
            hasCurrent_.store(false, std::memory_order_release);
            saveStateBestEffort();
            const int64_t endedId = currentTrack_.id;
            const int64_t qid = queue_.queue_id.load(std::memory_order_relaxed);
            double endedDuration = 0.0;
            {
                std::lock_guard<std::mutex> lk(decodeMtx_);
                endedDuration = duration_;
            }
            transition(kanariv::types::PlaybackState::Stopped, "end of queue");
            // Release the sink and the buffered tail; the decoder stays (play()
            // rewinds it through the pipeline). The graph's clock stops, so the
            // published position is 0 after this.
            if (auto r = pipeline_->stop(); !r && cfg_.logger)
                cfg_.logger->warn("audio stop failed: {}", r.error());
            qlk.release();
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
    if (!r)
        return std::unexpected(r.error());
    auto pr = doPlayTrack(t);
    qlk.release();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::ExpectedVoid Engine::Impl::prev() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("prev", "busy");
    kanariv::types::TrackRow t;
    auto r = queuePrevLocked(t);
    if (!r)
        return std::unexpected(r.error());
    auto pr = doPlayTrack(t);
    qlk.release();
    if (!pr)
        return std::unexpected(pr.error());
    pushEvent(*pr);
    return {};
}

Engine::Impl::QueueGuard Engine::Impl::tryLockQueue() noexcept {
    return queueMutex_.try_lock() ? QueueGuard{this} : QueueGuard{};
}

Engine::Impl::QueueGuard Engine::Impl::tryLockQueueFor(std::chrono::milliseconds wait) noexcept {
    return queueMutex_.try_lock_for(wait) ? QueueGuard{this} : QueueGuard{};
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
        return static_cast<double>(state_.positionMs.load(std::memory_order_relaxed)) / 1000.0;
    // The graph is closed while the decoder is released for a tag write; the
    // frozen offset is what the reopen resumes from.
    if (suspended_.load(std::memory_order_acquire))
        return suspendPos_;
    // One clock for device and simulated playback: the shared pipeline reports
    // the base position plus the elapsed time while playing, and a stopped or
    // closed graph reports 0 (stop() and the end-of-queue path clear
    // hasCurrent_ first, so no published track ever reads that 0).
    double pos = pipeline_ ? pipeline_->position().count() : 0.0;
    if (pos > duration_)
        pos = duration_;
    if (pos < 0)
        pos = 0;
    return pos;
}

kanariv::utils::Expected<kanariv::types::TrackRow>
Engine::Impl::fetchTrackByPosLocked(int64_t qid, int64_t pos) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    return storage_.queues->queueTrackAt(qid, pos);
}

kanariv::utils::Expected<void> Engine::Impl::setShuffleLocked(bool on) {
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

kanariv::utils::Expected<Engine::Impl::PeekedNext> Engine::Impl::peekNextLocked() {
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
        saveStateBestEffort(livePositionMs);
        return true;
    }
    if (queue_.cursor == next.plan.cursor)
        return true; // RepeatMode::One replay: the cursor must not move.
    queue_.cursor = next.plan.cursor;
    saveStateBestEffort(livePositionMs);
    return true;
}

kanariv::utils::Expected<void> Engine::Impl::queueNextLocked(kanariv::types::TrackRow& out) {
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

kanariv::utils::Expected<void> Engine::Impl::queuePrevLocked(kanariv::types::TrackRow& out) {
    const int64_t qid = queue_.queue_id.load(std::memory_order_relaxed);
    const bool shuffle = queue_.shuffle.load(std::memory_order_relaxed);
    // planPrev performs no I/O; resolve the one item count it may need here.
    std::size_t count = 0;
    if (!shuffle) {
        auto counted = storage_.queues->queueCount(qid);
        if (!counted)
            return std::unexpected(counted.error());
        count = *counted;
    }
    auto plan = detail::planPrev(shuffle, queue_.cursor,
                                 std::span<const int64_t>(queue_.perm.data(), queue_.perm.size()),
                                 [count] { return count; });
    if (!plan)
        return std::unexpected(plan.error());
    auto tr = fetchTrackByPosLocked(qid, plan->position);
    if (!tr)
        return std::unexpected(tr.error());
    out = tr.value();
    queue_.cursor = plan->cursor;
    saveStateBestEffort();
    return {};
}

} // namespace kanariv::engine
