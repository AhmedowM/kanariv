#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <engine/history.hpp>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <memory>
#include <optional>
#include <player/playback_policy.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "engine_impl.hpp"
#include "replay_gain.hpp"

namespace kanariv::engine {

void Engine::Impl::transition(kanariv::types::PlaybackState to, std::string_view reason) {
    const auto from = playbackState_.exchange(to, std::memory_order_acq_rel);
    if (from != to && cfg_.logger)
        cfg_.logger->debug("playback {} -> {} ({})", kanariv::types::toString(from),
                           kanariv::types::toString(to), reason);
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
                startOutputBestEffort();
        }
        lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
        transition(kanariv::types::PlaybackState::Playing, "play resume");
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
            startOutputBestEffort();
        lk.unlock();
        decodeCv_.notify_all();
        monCv_.notify_all();
        return {};
    } // end if Playing

    // Stopped -> resume persisted track if config enabled, else start new via cursor
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
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
                    qlk.release();
                    pushEvent(*pr);
                    return {};
                }
            }
        }
    }
    // Fall back to next track from cursor
    auto r = queueNextLocked(t);
    if (!r)
        return std::unexpected(r.error());
    // A restart can land on the persisted track again (single-track queue, or
    // a cursor parked on it). Start at the persisted offset instead of letting
    // the device play the head until a client-side seek arrives; an explicit
    // stop zeroes positionMs, so this never resurrects a stopped position.
    double from = 0.0;
    if (t.id == state_.currentTrackId && state_.positionMs > 0)
        from = static_cast<double>(state_.positionMs) / 1000.0;
    auto pr = doPlayTrack(t, from);
    qlk.release();
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
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("playTrack", "queue busy");
    auto idx = storage_.queues->queueIndexOf(queue_.queue_id, trackId);
    if (!idx)
        return std::unexpected{idx.error()};
    if (!*idx)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "track not in active queue")};
    size_t cursor = **idx;
    if (queue_.shuffle) {
        if (queue_.perm.empty()) {
            auto sr = setShuffleLocked(true);
            if (!sr)
                return std::unexpected(sr.error());
        }
        bool found = false;
        for (size_t i = 0; i < queue_.perm.size(); ++i) {
            if (queue_.perm[i] == static_cast<int64_t>(**idx)) {
                cursor = i;
                found = true;
                break;
            }
        }
        if (!found)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                             "track not in shuffle order")};
    }
    queue_.cursor = cursor;
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

Engine::ExpectedVoid Engine::Impl::pause() {
    // pausePos_/output_ live under decodeMtx_ while the persisted state lives
    // under queueMutex_: take the queue lock first (documented order) so the
    // pause snapshot and saveStateLocked() cannot interleave with a concurrent
    // transport command or the monitor checkpoint.
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("pause", "queue busy");
    return pauseLocked();
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
            stopOutputBestEffort();
    }
    accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    transition(kanariv::types::PlaybackState::Paused, "pause");
    if (hasDb())
        saveStateBestEffort();
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
            startOutputBestEffort();
    }
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    transition(kanariv::types::PlaybackState::Playing, "resume");
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
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("stop", "queue busy");
    if (s == kanariv::types::PlaybackState::Playing)
        accountPlaybackTime();
    lastPlaybackSampleMs_.store(0, std::memory_order_release);
    transition(kanariv::types::PlaybackState::Stopped, "stop");
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
            stopOutputBestEffort();
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
        saveStateBestEffort();
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
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("seek", "queue busy");
    // Pause decode thread to safely seek + reset ring (prevents race with decodeLoop)
    // A seek repositions the transport; it must not start one. The state it
    // found is restored on both the success and the failure path.
    const auto previousState = playbackState_.load(std::memory_order_acquire);
    {
        std::unique_lock<std::mutex> lk(decodeMtx_);
        transition(kanariv::types::PlaybackState::Paused, "seek");
        // snapshot for restore on error (Task 1: seek hardening)
        double savedPause = pausePos_;
        auto savedStart = playStart_;

        if (decoder_) {
            auto res = decoder_->seek(seconds);
            if (!res) {
                pausePos_ = savedPause;
                playStart_ = savedStart;
                transition(previousState, "seek restore");
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
        transition(previousState, "seek restore");
    }
    decodeCv_.notify_all();
    if (hasDb())
        saveStateBestEffort();
    return {};
}

Engine::ExpectedVoid Engine::Impl::suspendCurrentTrack() {
    if (!hasCurrent_.load(std::memory_order_acquire)) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no track"));
    }
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("suspend", "queue busy");
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
    transition(kanariv::types::PlaybackState::Paused, "suspend");
    if (output_)
        stopOutputBestEffort();
    if (ring_)
        ring_->reset();
    decoder_.reset();
    reader_.reset();
    resetCrossfadeLocked();
    lk.unlock();
    qlk.release();
    decodeCv_.notify_all();
    monCv_.notify_all();
    return {};
}

Engine::ExpectedVoid Engine::Impl::resumeAfterTagWrite(const kanariv::types::TrackRow& t,
                                                       double pos, bool wasPaused) {
    auto qlk = tryLockQueueFor(queueLockWait);
    if (!qlk)
        return queueBusy("resume", "queue busy");
    // The user may have moved on while suspended (next/prev/play from
    // another client): the tag/db update already landed, so there is
    // nothing to reopen. Success, not an error.
    if (!hasCurrent_.load(std::memory_order_acquire) || state_.currentTrackId != t.id)
        return {};
    bool marked = markedPlayed_.load(std::memory_order_acquire);
    uint64_t accumulated = accumulatedPlaybackMs_.load(std::memory_order_acquire);
    kanariv::types::TrackRow reopen = t;
    auto r = doPlayTrack(reopen, pos);
    markedPlayed_.store(marked, std::memory_order_release);
    accumulatedPlaybackMs_.store(accumulated, std::memory_order_release);
    lastPlaybackSampleMs_.store(detail::nowMs(), std::memory_order_release);
    if (!r)
        return std::unexpected(r.error());
    kanariv::types::EngineEvent started = *r;
    // doPlayTrack seeks before preroll and starts Playing at `pos`; only the
    // prior paused state is left to restore. The queue lock is already held,
    // so use the *Locked body instead of pause() (which would deadlock).
    if (wasPaused) {
        auto pr = pauseLocked();
        if (!pr)
            return std::unexpected(pr.error());
    }
    qlk.release();
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
            saveStateBestEffort();
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
        stopOutputBestEffort();
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
                                ? static_cast<double>(decoder_->totalFrames()) /
                                      static_cast<double>(decoder_->sampleRate())
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
                // ReplayGain: apply the mode-selected gain (dB) as a volume
                // multiplier on top of the user volume. dB -> linear:
                // 10^(dB/20). Gains come from the explicit analysis pass.
                float gain = volume_.load(std::memory_order_relaxed);
                const double gainDb = detail::replayGainDbFor(cfg_.replayGain, t);
                if (gainDb != 0.0)
                    gain = static_cast<float>((gain * std::pow(10.0, gainDb / 20.0)));
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
                    startOutputBestEffort();
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
    transition(kanariv::types::PlaybackState::Playing, "track started");
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
        saveStateBestEffort();
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

} // namespace kanariv::engine
