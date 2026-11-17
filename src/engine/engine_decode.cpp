#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <engine/fade.hpp>
#include <engine/history.hpp>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/ring.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <player/playback_policy.hpp>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "engine_impl.hpp"

namespace kanariv::engine {

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
    auto qlk = tryLockQueue();
    if (!qlk)
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
    qlk.release();

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
        transition(kanariv::types::PlaybackState::Playing, "crossfade published");
        // Fade envelope for the newly published track (decodeMtx_ state): no
        // fade-in (we are mid-track), normal fade-out near its end.
        fadeSample_.store(0, std::memory_order_release);
        fadeInSamples_ = 0;
        fadeOutSamples_ = 0;
        fadeOutStart_ = 0;
        if (cfg_.fadeOutMs > 0 && sampleRate > 0.0 && trackDuration > 0.0) {
            fadeOutSamples_ =
                static_cast<uint64_t>(cfg_.fadeOutMs) * static_cast<uint64_t>(sampleRate) / 1000;
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
        saveStateBestEffort();
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
                        crossfadeSamples_ = static_cast<uint64_t>(cfg_.crossfadeMs) *
                                            static_cast<uint64_t>(overlapRate) / 1000;
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
