#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <optional>
#include <player/playback_policy.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pipeline.hpp"

namespace kanariv::player::detail {

namespace {

int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Same rule as AudioOutput: a bad gain is neutral, never a mute.
float sanitizedGain(float g) noexcept {
    return std::isfinite(g) && g >= 0.0f ? g : 1.0f;
}

kanariv::utils::Error stateError(std::string_view message) {
    return kanariv::utils::makeError(kanariv::utils::StatusCode::State, message);
}

} // namespace

Pipeline::Pipeline(Config cfg) : cfg_(std::move(cfg)) {
    volume_ = kanariv::utils::clampVolume(cfg_.volume);
    gain_ = sanitizedGain(cfg_.gain);
    producer_ = std::jthread([this](std::stop_token st) { producerLoop(st); });
}

Pipeline::~Pipeline() {
    producer_.request_stop();
    cv_.notify_all();
    if (producer_.joinable())
        producer_.join();
    close();
}

Pipeline::ExpectedVoid Pipeline::open(std::uint32_t sampleRate, std::uint32_t channels,
                                      std::shared_ptr<Source> source) {
    if (sampleRate == 0)
        sampleRate = 48000;
    if (channels == 0)
        channels = 2;
    std::lock_guard<std::mutex> ctl(controlMutex_);

    // Quiesce the old graph first: the generation bump parks the producer, and
    // a chunk that is already decoding lands in the old ring (still alive via
    // the shared ring and the parked pointer) and is discarded.
    std::unique_ptr<AudioOutput> oldOutput;
    std::shared_ptr<Ring> oldRing;
    std::shared_ptr<Source> oldSource;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gen_.fetch_add(1, std::memory_order_acq_rel);
        state_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
        opened_ = false;
        needsRewind_ = false;
        baseSec_ = 0.0;
        startMs_ = 0;
        oldOutput = std::move(output_);
        oldRing = std::move(ring_);
        oldSource = std::move(source_);
    }
    cv_.notify_all();
    // Device shutdown outside the lock, and while oldRing is still alive: the
    // audio callback holds a raw ring pointer and must be joined first.
    oldOutput.reset();
    oldRing.reset();
    oldSource.reset();

    std::shared_ptr<Ring> ring;
    std::unique_ptr<AudioOutput> output;
    if (cfg_.ringFrames > 0) {
        ring = std::make_shared<Ring>(cfg_.ringFrames, channels);
        AudioOutput::Config outCfg;
        outCfg.sampleRate = sampleRate;
        outCfg.channels = channels;
        outCfg.ring = ring.get();
        outCfg.volume = volume_;
        outCfg.gain = gain_;
        outCfg.logger = cfg_.logger;
        outCfg.nullBackend = cfg_.nullBackend;
        outCfg.deviceId = cfg_.deviceId;
        outCfg.deviceName = cfg_.deviceName;
        auto outResult = AudioOutput::create(outCfg);
        if (!outResult) {
            std::lock_guard<std::mutex> lk(mtx_);
            lastErr_ = outResult.error().message;
            return std::unexpected(outResult.error());
        }
        output = std::move(outResult.value());
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        sampleRate_ = sampleRate;
        channels_ = channels;
        ring_ = std::move(ring);
        output_ = std::move(output);
        source_ = std::move(source);
        opened_ = true;
        lastErr_.clear();
        state_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
    }
    cv_.notify_all();
    return {};
}

void Pipeline::close() {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::unique_ptr<AudioOutput> output;
    std::shared_ptr<Ring> ring;
    std::shared_ptr<Source> source;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gen_.fetch_add(1, std::memory_order_acq_rel);
        state_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
        opened_ = false;
        needsRewind_ = false;
        baseSec_ = 0.0;
        startMs_ = 0;
        output = std::move(output_);
        ring = std::move(ring_);
        source = std::move(source_);
    }
    cv_.notify_all();
    // Joined device first: its callback holds a raw ring pointer, so the ring
    // must outlive it.
    output.reset();
    ring.reset();
    source.reset();
}

Pipeline::ExpectedVoid Pipeline::preroll() {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::shared_ptr<Source> source;
    std::shared_ptr<Ring> ring;
    std::uint32_t ch = 0;
    SourceToken token;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!source_ || !ring_)
            return {};
        source = source_;
        ring = ring_;
        ch = channels_;
        token = SourceToken{&gen_, gen_.load(std::memory_order_acquire)};
    }
    if (ch == 0)
        return {};

    // Shared policy: have the ring filled before the device starts. The space
    // check compares availableWrite() (frames) against frames*channels, the
    // conservative form both call sites used before the policy was shared.
    const std::size_t target = kanariv::playback::policy::prerollFrames(ring->capacity());
    const std::size_t chunkFrames = kanariv::playback::policy::chunkCapFrames(ch);
    std::vector<float> buffer(chunkFrames * ch);
    std::size_t filled = 0;
    while (filled < target && ring->availableWrite() >= chunkFrames * ch) {
        const Source::Result r =
            source->produce(std::span<float>(buffer.data(), buffer.size()), token);
        if (!r.samples.empty()) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ring_ == ring && token.current())
                (void)ring->write(r.samples);
        }
        filled += r.samples.size() / ch;
        if (r.status == Source::Status::Failed) {
            std::lock_guard<std::mutex> lk(mtx_);
            failLocked(r.error.empty() ? std::string_view("preroll decode failed") : r.error);
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                             std::string_view("preroll decode failed")));
        }
        if (r.samples.empty() || r.status == Source::Status::EndOfStream)
            break;
    }
    return {};
}

Pipeline::ExpectedVoid Pipeline::play() {
    std::lock_guard<std::mutex> ctl(controlMutex_);

    std::shared_ptr<Source> source;
    bool rewind = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto s = state_.load(std::memory_order_acquire);
        if (s == kanariv::types::PlaybackState::Playing)
            return std::unexpected(stateError("already playing"));
        if (s == kanariv::types::PlaybackState::Error)
            return std::unexpected(stateError("error state; reopen to play"));
        if (!opened_)
            return std::unexpected(stateError("not opened"));
        rewind = needsRewind_;
        source = source_;
    }

    if (rewind && source) {
        // Invalidate before the source moves: a producer that already holds the
        // decoder lock must not decode the new position into a stale chunk.
        gen_.fetch_add(1, std::memory_order_acq_rel);
        if (auto seekResult = source->seek(0.0); !seekResult) {
            std::lock_guard<std::mutex> lk(mtx_);
            failLocked(seekResult.error().message);
            return std::unexpected(seekResult.error());
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (ring_)
            ring_->reset();
        baseSec_ = 0.0;
        startMs_ = 0;
        needsRewind_ = false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (output_) {
            if (auto startResult = output_->start(); !startResult) {
                lastErr_ = startResult.error().message;
                state_.store(kanariv::types::PlaybackState::Error, std::memory_order_release);
                return std::unexpected(startResult.error());
            }
        }
        startMs_ = nowMs();
        state_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    }
    cv_.notify_all();
    return {};
}

Pipeline::ExpectedVoid Pipeline::pause() {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_.load(std::memory_order_acquire) != kanariv::types::PlaybackState::Playing)
        return std::unexpected(stateError("not playing"));

    if (output_) {
        if (auto stopResult = output_->stop(); !stopResult) {
            // Never report Paused when the device did not stop.
            startMs_ = 0;
            lastErr_ = stopResult.error().message;
            state_.store(kanariv::types::PlaybackState::Error, std::memory_order_release);
            return std::unexpected(stopResult.error());
        }
    }

    // Fold elapsed playback into the base position.
    const int64_t now = nowMs();
    if (startMs_ > 0 && now > startMs_)
        baseSec_ += static_cast<double>(now - startMs_) / 1000.0;
    startMs_ = 0;
    state_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
    cv_.notify_all();
    return {};
}

Pipeline::ExpectedVoid Pipeline::resume() {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_.load(std::memory_order_acquire) != kanariv::types::PlaybackState::Paused)
        return std::unexpected(stateError("not paused"));

    if (output_) {
        if (auto startResult = output_->start(); !startResult) {
            startMs_ = 0;
            lastErr_ = startResult.error().message;
            state_.store(kanariv::types::PlaybackState::Error, std::memory_order_release);
            return std::unexpected(startResult.error());
        }
    }
    startMs_ = nowMs();
    state_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
    cv_.notify_all();
    return {};
}

Pipeline::ExpectedVoid Pipeline::stop() {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::optional<kanariv::utils::Error> stopError;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gen_.fetch_add(1, std::memory_order_acq_rel);
        if (output_) {
            if (auto stopResult = output_->stop(); !stopResult) {
                stopError = stopResult.error();
                lastErr_ = stopError->message;
            }
        }
        if (ring_)
            ring_->reset();
        baseSec_ = 0.0;
        startMs_ = 0;
        // The source stays where playback stopped; play() rewinds it so the
        // "Stopped == initial state" contract still holds.
        needsRewind_ = opened_ && source_ != nullptr;
        state_.store(stopError ? kanariv::types::PlaybackState::Error
                               : kanariv::types::PlaybackState::Stopped,
                     std::memory_order_release);
    }
    cv_.notify_all();
    if (stopError)
        return std::unexpected(*stopError);
    return {};
}

Pipeline::ExpectedVoid Pipeline::seek(double seconds) {
    std::lock_guard<std::mutex> ctl(controlMutex_);
    std::shared_ptr<Source> source;
    bool wasPlaying = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!opened_)
            return std::unexpected(stateError("not opened"));
        source = source_;
        wasPlaying = state_.load(std::memory_order_acquire) == kanariv::types::PlaybackState::Playing;
        // Generation and state move together: a producer can sample
        // (Playing, gen_old) or (not Playing, gen_new), never (Playing, new),
        // so no chunk can be decoded for the new position before the seek.
        gen_.fetch_add(1, std::memory_order_acq_rel);
        if (wasPlaying) {
            state_.store(kanariv::types::PlaybackState::Paused, std::memory_order_release);
            if (output_) {
                if (auto stopResult = output_->stop(); !stopResult) {
                    startMs_ = 0;
                    lastErr_ = stopResult.error().message;
                    state_.store(kanariv::types::PlaybackState::Error,
                                 std::memory_order_release);
                    return std::unexpected(stopResult.error());
                }
            }
        }
    }
    cv_.notify_all();

    if (source) {
        if (auto seekResult = source->seek(seconds); !seekResult) {
            std::lock_guard<std::mutex> lk(mtx_);
            failLocked(seekResult.error().message);
            return std::unexpected(seekResult.error());
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (ring_)
            ring_->reset();
        baseSec_ = seconds;
        needsRewind_ = false;
        startMs_ = 0;
        if (wasPlaying) {
            // Restart from the new position, publishing Playing last.
            if (output_) {
                if (auto startResult = output_->start(); !startResult) {
                    lastErr_ = startResult.error().message;
                    state_.store(kanariv::types::PlaybackState::Error,
                                 std::memory_order_release);
                    return std::unexpected(startResult.error());
                }
            }
            startMs_ = nowMs();
            state_.store(kanariv::types::PlaybackState::Playing, std::memory_order_release);
        }
    }
    cv_.notify_all();
    return {};
}

void Pipeline::rebase(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    baseSec_ = seconds;
    startMs_ = state_.load(std::memory_order_acquire) == kanariv::types::PlaybackState::Playing
                   ? nowMs()
                   : 0;
}

void Pipeline::setVolume(float volume) {
    std::lock_guard<std::mutex> lk(mtx_);
    volume_ = kanariv::utils::clampVolume(volume);
    if (output_)
        output_->setVolume(volume_);
}

void Pipeline::setGain(float gain) {
    std::lock_guard<std::mutex> lk(mtx_);
    gain_ = sanitizedGain(gain);
    if (output_)
        output_->setGain(gain_);
}

kanariv::types::PlaybackState Pipeline::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

void Pipeline::fail(std::string_view reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    failLocked(reason);
}

std::string Pipeline::lastError() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lastErr_;
}

std::chrono::duration<double> Pipeline::position() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto s = state_.load(std::memory_order_acquire);
    if (s == kanariv::types::PlaybackState::Stopped ||
        s == kanariv::types::PlaybackState::Error)
        return std::chrono::duration<double>(0.0);
    double pos = baseSec_;
    if (s == kanariv::types::PlaybackState::Playing && startMs_ > 0) {
        const int64_t now = nowMs();
        if (now > startMs_)
            pos += static_cast<double>(now - startMs_) / 1000.0;
    }
    return std::chrono::duration<double>(pos);
}

void Pipeline::failLocked(std::string_view reason) {
    // Generation and state move together (see seek()); a parked or in-flight
    // producer can no longer write once the graph is in Error.
    gen_.fetch_add(1, std::memory_order_acq_rel);
    if (output_)
        (void)output_->stop();
    lastErr_.assign(reason);
    state_.store(kanariv::types::PlaybackState::Error, std::memory_order_release);
    cv_.notify_all();
}

void Pipeline::producerLoop(std::stop_token st) {
    // Reusable scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    while (!st.stop_requested()) {
        std::shared_ptr<Source> source;
        std::shared_ptr<Ring> ring;
        std::uint32_t ch = 0;
        SourceToken token;
        std::uint64_t consumed = 0;
        std::size_t freeFrames = 0;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            // Woken immediately by every transport change; the bound only
            // covers a notify that raced the predicate (and keeps a parked
            // thread responsive to stop()).
            cv_.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return st.stop_requested() ||
                       (state_.load(std::memory_order_acquire) ==
                            kanariv::types::PlaybackState::Playing &&
                        source_ && ring_);
            });
            if (st.stop_requested())
                break;
            if (state_.load(std::memory_order_acquire) != kanariv::types::PlaybackState::Playing ||
                !source_ || !ring_)
                continue;
            source = source_;
            ring = ring_;
            ch = channels_;
            // Sample the read counter before the space check so a drain landing
            // in between is visible to waitForRead() (P8).
            consumed = ring->readSequence();
            freeFrames = ring->availableWrite();
            token = SourceToken{&gen_, gen_.load(std::memory_order_acquire)};
        }
        if (ch == 0)
            continue;
        if (freeFrames == 0) {
            // Park until the audio callback drains instead of polling; the
            // bound covers a stopped consumer.
            (void)ring->waitForRead(consumed, kanariv::playback::policy::ringWakeBudget);
            continue;
        }
        if (!token.current())
            continue;
        const std::size_t maxFrames = kanariv::playback::policy::chunkFrames(ch, freeFrames);
        if (maxFrames == 0)
            continue;
        if (scratch.size() < maxFrames * ch)
            scratch.resize(maxFrames * ch);

        const Source::Result r =
            source->produce(std::span<float>(scratch.data(), maxFrames * ch), token);

        bool drained = false;
        bool failed = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ring_ == ring &&
                state_.load(std::memory_order_acquire) ==
                    kanariv::types::PlaybackState::Playing &&
                token.current()) {
                if (!r.samples.empty())
                    (void)ring->write(r.samples);
                if (r.status == Source::Status::EndOfStream)
                    drained = ring->availableRead() == 0;
                failed = r.status == Source::Status::Failed;
            }
        }
        if (failed) {
            finishFailed(token, *ring, r.error);
            continue;
        }
        if (r.status == Source::Status::EndOfStream) {
            if (!drained) {
                // Drain the tail before ending so the last samples are audible;
                // the consumer signal also covers "the tail moved".
                (void)ring->waitForRead(consumed, kanariv::playback::policy::ringWakeBudget);
                continue;
            }
            finishEndOfStream(token, *ring);
            continue;
        }
        if (r.samples.empty())
            std::this_thread::sleep_for(kanariv::playback::policy::decodeStallPoll);
    }
}

void Pipeline::finishEndOfStream(const SourceToken& token, const Ring& ring) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_.load(std::memory_order_acquire) != kanariv::types::PlaybackState::Playing ||
        ring_.get() != &ring || !token.current())
        return; // superseded by a control operation; that operation decides
    gen_.fetch_add(1, std::memory_order_acq_rel);
    needsRewind_ = true;
    if (output_)
        (void)output_->stop();
    state_.store(kanariv::types::PlaybackState::Stopped, std::memory_order_release);
    cv_.notify_all();
}

void Pipeline::finishFailed(const SourceToken& token, const Ring& ring, std::string_view why) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_.load(std::memory_order_acquire) != kanariv::types::PlaybackState::Playing ||
        ring_.get() != &ring || !token.current())
        return;
    // A decode failure is not a normal end of stream (P2).
    gen_.fetch_add(1, std::memory_order_acq_rel);
    if (output_)
        (void)output_->stop();
    lastErr_.assign(why.empty() ? std::string_view("decode error") : why);
    state_.store(kanariv::types::PlaybackState::Error, std::memory_order_release);
    cv_.notify_all();
}

} // namespace kanariv::player::detail
