#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <engine/fade.hpp>
#include <engine/history.hpp>
#include <engine/transition_policy.hpp>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <player/playback_policy.hpp>
#include <span>
#include <string>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "engine_impl.hpp"
#include "replay_gain.hpp"

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
        snap.crossfadeRefused = crossfadeRefused_;
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
        detail::NearEndInput in;
        in.remainingSec = snap.duration - snap.position;
        in.hasCurrent = snap.hasCurrent;
        in.crossfadeMs = cfg_.crossfadeMs;
        in.gaplessMs = cfg_.gaplessMs;
        in.interTrackGapMs = cfg_.interTrackGapMs;
        in.crossfadeActive = crossfadeActive_.load(std::memory_order_acquire);
        in.crossfadePending = crossfadePending_.load(std::memory_order_acquire);
        in.crossfadeRefused = snap.crossfadeRefused;
        switch (detail::nearEndAction(in)) {
        case detail::NearEndAction::ArmCrossfade:
            // The producer's next request loads the following track into
            // crossfadeDecoder_ and mixes it with the outgoing track.
            crossfadePending_.store(true, std::memory_order_release);
            break;
        case detail::NearEndAction::AdvanceGapless:
            // gaplessArmed 0->1 CAS: exactly one advance per track.
            {
                bool expected = false;
                if (gaplessArmed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
                    if (!autoNextIfEpoch(snap.transitionEpoch))
                        gaplessArmed_.store(false, std::memory_order_release);
                }
            }
            break;
        case detail::NearEndAction::None:
            break;
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
        // Gain of the incoming track, before the move empties crossfadeTrack_.
        // The sink already runs on the outgoing track's gain; the incoming one
        // must take over on publication (user volume is untouched).
        const float gain = detail::replayGainLinear(cfg_.replayGain, crossfadeTrack_);

        decoder_ = std::move(crossfadeDecoder_);
        reader_ = std::move(crossfadeReader_);
        currentTrack_ = std::move(crossfadeTrack_);
        crossfadeTrack_ = {};
        duration_ = trackDuration;
        // Gain of the incoming track takes over on the live sink (user volume
        // is untouched), and the new track starts its own transport window at
        // position 0; the decoder itself continues where the overlap stopped.
        pipeline_->setGain(gain);
        pipeline_->rebase(0.0);
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
    monCv_.notify_all();
    if (cfg_.logger)
        cfg_.logger->debug("crossfade complete track={}", trackId);
    return true;
}

void Engine::Impl::resetCrossfadeLocked() noexcept {
    crossfadeActive_.store(false, std::memory_order_release);
    crossfadePending_.store(false, std::memory_order_release);
    crossfadeRefused_ = false;
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

void Engine::Impl::preloadCrossfade() {
    // @pre called on the pipeline's producer thread with no mutex held.
    // One transaction: resolve the next entry without touching the cursor, open
    // its reader+decoder unlocked, then commit the cursor advance and arm the
    // crossfade under the queue lock. A failure (unreadable entry, concurrent
    // queue change, teardown) must leave the cursor on the unplayed entry --
    // advancing first and dropping the decoder skipped one unreadable file
    // permanently (persisted across restarts).
    if (!crossfadePending_.load(std::memory_order_acquire) ||
        crossfadeActive_.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        if (crossfadeRefused_) {
            // This outgoing track already met a layout-mismatched neighbour;
            // engineTick re-arms the request every tick inside the window.
            crossfadePending_.store(false, std::memory_order_release);
            return;
        }
    }
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
        return;
    }
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
    bool refused = false;
    std::uint32_t liveCh = 0;
    std::uint32_t liveRate = 0;
    std::uint32_t inCh = 0;
    std::uint32_t inRate = 0;
    if (xfReader && xfDecoder) {
        // Commit and arm atomically against both state machines: queueMutex_
        // keeps doPlayTrack()/stop() out, decodeMtx_ keeps pause()/seek() out,
        // so the state checked here still holds when the crossfade becomes
        // active.
        std::unique_lock<std::timed_mutex> qlk(queueMutex_);
        std::unique_lock<std::mutex> lk(decodeMtx_);
        // decodeMtx_ is held here, so hand the position down instead of letting
        // the persist path relock it.
        const int64_t livePositionMs = static_cast<int64_t>(currentPositionLocked() * 1000.0);
        liveCh = decoder_ ? decoder_->channels() : 0;
        liveRate = decoder_ ? decoder_->sampleRate() : 0;
        inCh = xfDecoder->channels();
        inRate = xfDecoder->sampleRate();
        // The overlap may mix two layouts, but publication swaps the decoder
        // under a ring and a device opened for the outgoing stride/rate.
        // Refusing keeps the entry queued for the normal end-of-track advance.
        const bool layoutMatches = liveCh > 0 && liveRate > 0 && inCh > 0 && inRate > 0 &&
                                   liveCh == inCh && liveRate == inRate;
        if (playbackState_.load(std::memory_order_acquire) ==
                kanariv::types::PlaybackState::Playing &&
            !crossfadeActive_.load(std::memory_order_relaxed) && !layoutMatches) {
            crossfadeRefused_ = true;
            crossfadePending_.store(false, std::memory_order_release);
            refused = true;
        } else if (playbackState_.load(std::memory_order_acquire) ==
                       kanariv::types::PlaybackState::Playing &&
                   !crossfadeActive_.load(std::memory_order_relaxed) &&
                   commitNextLocked(fingerprint, std::move(next), livePositionMs)) {
            crossfadeReader_ = std::move(xfReader);
            crossfadeDecoder_ = std::move(xfDecoder);
            crossfadeTrack_ = std::move(next.track);
            crossfadeActive_.store(true, std::memory_order_release);
            crossfadePending_.store(false, std::memory_order_release);
            // crossfadeSample_ advances by decoded *outgoing* frames, so the
            // overlap length must be expressed in the outgoing clock. The
            // incoming rate only matters once this track is published.
            uint32_t overlapRate = decoder_ ? decoder_->sampleRate() : 0;
            if (overlapRate == 0)
                overlapRate = crossfadeDecoder_->sampleRate();
            crossfadeSamples_ = static_cast<uint64_t>(cfg_.crossfadeMs) *
                                static_cast<uint64_t>(overlapRate) / 1000;
            crossfadeSample_ = 0;
            armed = true;
        }
    }
    if (refused) {
        if (cfg_.logger)
            cfg_.logger->debug("crossfade refused track={} outgoing={}ch@{}Hz incoming={}ch@{}Hz",
                               next.track.id, liveCh, liveRate, inCh, inRate);
    } else if (!armed) {
        // Keep the cursor on the failed entry: the next attempt (engineTick
        // re-arms, or an explicit next()/play()) targets it again instead of
        // skipping past it.
        crossfadePending_.store(false, std::memory_order_release);
        if (cfg_.logger)
            cfg_.logger->debug("crossfade preload failed track={} file={}", next.track.id,
                               next.track.path);
    } else if (cfg_.logger) {
        cfg_.logger->debug("crossfade armed track={}", crossfadeTrack_.id);
    }
}

kanariv::player::detail::Source::Result
Engine::Impl::EngineSource::produce(std::span<float> out,
                                    kanariv::player::detail::SourceToken token,
                                    kanariv::player::detail::ProduceMode mode) {
    using kanariv::player::detail::ProduceMode;
    Impl& e = impl_;
    // A chunk that straddles a track change belongs to a graph this source no
    // longer feeds: discard it without consuming decoder frames.
    if (!token.current())
        return {};
    // Preroll() fills the ring before the device starts (control thread): it
    // decodes only, so a short file stops the initial fill instead of running
    // the transition ladder before playback begins.
    const bool playback = mode == ProduceMode::Playback;
    if (playback)
        e.preloadCrossfade();

    std::uint32_t ch = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t fadeIn = 0;
    std::uint64_t fadeOut = 0;
    double fadeOutStart = 0.0;
    std::size_t frames = 0;
    std::uint64_t eofEpoch = 0;
    bool refused = false;
    media::DecodeStatus decodeStatus = media::DecodeStatus::Ok;
    std::size_t xfFrames = 0;
    std::uint32_t xfCh = 0;
    double xfProgress = 0.0;
    bool xfEarlyEof = false;
    bool xfComplete = false;
    std::vector<float> xfBuf;
    {
        std::lock_guard<std::mutex> lk(e.decodeMtx_);
        // The pipeline may have parked this call while doPlayTrack() replaced
        // the decoder; the token is the epoch check for that case.
        if (!token.current() || !e.decoder_)
            return {};
        ch = e.decoder_->channels();
        if (ch == 0)
            ch = 2;
        sampleRate = e.decoder_->sampleRate();
        fadeIn = e.fadeInSamples_;
        fadeOut = e.fadeOutSamples_;
        fadeOutStart = e.fadeOutStart_;
        frames = e.decoder_->decode(out, &decodeStatus);
        // Epoch of the transition this decoder was opened for. The end-of-
        // stream ladder below runs after decodeMtx_ is released, so this is
        // what tells it whether the track it decoded is still the current one.
        eofEpoch = e.transitionEpoch_.load(std::memory_order_acquire);
        refused = e.crossfadeRefused_;
        // During crossfade, decode the incoming track into a second buffer.
        if (playback && frames > 0 &&
            e.crossfadeActive_.load(std::memory_order_relaxed) && e.crossfadeDecoder_) {
            // The incoming decoder keeps its own channel layout; sizing the
            // buffer from the outgoing channels decoded the wrong number of
            // frames and mixed interleaved data with the wrong stride.
            xfCh = e.crossfadeDecoder_->channels();
            if (xfCh == 0)
                xfCh = ch;
            xfBuf.resize(frames * xfCh);
            xfFrames = e.crossfadeDecoder_->decode(std::span<float>(xfBuf.data(), xfBuf.size()));
            if (xfFrames == 0) {
                // Incoming track reached EOF before the overlap did: it has
                // become the playing track, so the transition completes on this
                // iteration. Dropping the crossfade here without publishing
                // left currentTrack_ and the persisted state describing the
                // outgoing track.
                xfEarlyEof = true;
            } else if (e.crossfadeSamples_ > 0) {
                xfProgress = static_cast<double>(e.crossfadeSample_) /
                             static_cast<double>(e.crossfadeSamples_);
                if (xfProgress > 1.0)
                    xfProgress = 1.0;
                e.crossfadeSample_ += frames;
                if (e.crossfadeSample_ >= e.crossfadeSamples_)
                    xfComplete = true;
            }
        }
    }
    // Apply the fade-in/fade-out envelope and the crossfade mix (producer
    // thread only, real-time safe). The preroll fill stays raw, exactly as the
    // engine's own preroll decoded it before the shared pipeline.
    if (playback && frames > 0 &&
        (fadeIn > 0 || fadeOut > 0 || e.crossfadeActive_.load(std::memory_order_relaxed))) {
        const uint64_t fadeStart = e.fadeSample_.load(std::memory_order_relaxed);
        kanariv::engine::detail::FadeParams fade;
        fade.startFrame = fadeStart;
        fade.fadeInFrames = fadeIn;
        fade.fadeOutFrames = fadeOut;
        fade.fadeOutStartSec = fadeOutStart;
        fade.sampleRate = static_cast<double>(sampleRate); // snapped under decodeMtx_
        std::span<const float> incoming;
        if (xfFrames > 0) {
            // The two buffers are interleaved independently, so the mix needs
            // each side's own channel count.
            incoming = std::span<const float>(xfBuf.data(), xfFrames * xfCh);
        }
        (void)kanariv::engine::detail::applyFadeEnvelope(out.first(frames * ch), frames, ch, fade,
                                                         incoming, xfProgress, xfCh);
        e.fadeSample_.store(fadeStart + frames, std::memory_order_release);
    }
    if (frames == 0) {
        const auto outcome = playback::policy::classifyDecode(decodeStatus, frames);
        if (outcome == playback::policy::DecodeOutcome::Continue) {
            // No frames and no final status: not an end of track. The pipeline
            // repeats the request after its stall backoff.
            return {};
        }
        if (!playback)
            return {out.first(0), Status::EndOfStream}; // short file: stop prerolling
        if (outcome == playback::policy::DecodeOutcome::Failed) {
            // A decode failure is not a clean end (P2). Nothing more can come
            // from this decoder, so report it and let the configured transition
            // move on; silently advancing hid the failure.
            if (e.cfg_.logger)
                e.cfg_.logger->warn("decode failed track_id={}",
                                    e.state_.currentTrackId.load(std::memory_order_relaxed));
            kanariv::types::EngineEvent ev;
            ev.type = kanariv::types::EngineEventType::Error;
            ev.msg = "decode error";
            e.pushEvent(ev);
        }
        // EOF reached: the policy names who owns the transition; the caller
        // gathers the gap sub-state and applies the action.
        detail::EofInput eof;
        eof.crossfadeActive = e.crossfadeActive_.load(std::memory_order_acquire);
        eof.crossfadeRefused = refused;
        eof.crossfadeMs = e.cfg_.crossfadeMs;
        eof.interTrackGapMs = e.cfg_.interTrackGapMs;
        if (!eof.crossfadeActive && eof.crossfadeMs == 0 && eof.interTrackGapMs > 0) {
            eof.gapArmed = e.interTrackGapArmed_.load(std::memory_order_acquire);
            if (!eof.gapArmed) {
                // The ring being empty is the audible end: the callback
                // zero-fills on underrun.
                eof.gapTailDrained = e.pipeline_->bufferedFrames() == 0;
            } else {
                eof.gapElapsed = detail::nowMs() - e.interTrackGapStartMs_ >=
                                 static_cast<uint64_t>(e.cfg_.interTrackGapMs);
            }
        }
        switch (detail::eofAction(eof)) {
        case detail::EofAction::PublishCrossfade:
            // The outgoing track is done while the incoming one is already in
            // use: waiting for the overlap to run out would leave nothing to
            // write (the outgoing decoder is at EOF) and stall the ring.
            (void)e.tryPublishCrossfade();
            break;
        case detail::EofAction::HoldForCrossfade:
            // Nothing was armed yet (the preload failed or is still pending):
            // the cursor stays, engineTick re-arms.
            break;
        case detail::EofAction::ArmGap:
            e.interTrackGapStartMs_ = detail::nowMs();
            e.interTrackGapArmed_.store(true, std::memory_order_release);
            break;
        case detail::EofAction::WaitDrain:
        case detail::EofAction::WaitGap:
            // doPlayTrack() clears the armed flag on success; a failed advance
            // (busy queue) is retried on the next pass with the gap already
            // elapsed.
            break;
        case detail::EofAction::Advance:
            if (e.playbackState_.load(std::memory_order_acquire) ==
                kanariv::types::PlaybackState::Playing)
                (void)e.autoNextIfEpoch(eofEpoch);
            break;
        }
        // An empty Continue: the pipeline repeats the request after its stall
        // backoff, which paces both the gap wait and the advance retry.
        return {};
    }
    if (xfComplete || xfEarlyEof) {
        // The overlap is finished (or the incoming track ran out first):
        // publish the incoming track so currentTrack_/duration_/history/
        // persistence describe what is now playing. When the queue lock is
        // contended the crossfade stays active and the next request (or the EOF
        // branch above) retries.
        if (token.current())
            (void)e.tryPublishCrossfade();
    }
    return {out.first(frames * ch), Status::Continue};
}

kanariv::utils::Expected<void> Engine::Impl::EngineSource::seek(double seconds) {
    std::lock_guard<std::mutex> lk(impl_.decodeMtx_);
    // A clock-only graph (simulated playback) has no source to move; the
    // pipeline already rebased its clock.
    if (!impl_.decoder_)
        return {};
    if (auto r = impl_.decoder_->seek(seconds); !r)
        return std::unexpected(r.error());
    return {};
}

} // namespace kanariv::engine
