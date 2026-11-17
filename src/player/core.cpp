#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/ring.hpp>
#include <kanariv/utils/status_format.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <player/playback_policy.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kanariv::player {

/// Player state and playback components, hidden from the public header.
///
/// @par Lock protocol
/// Two mutexes guard the playback graph. They must never be taken in the
/// opposite order:
///
/// 1. `controlMutex_` serializes control operations (open, openReader, play,
///    pause, resume, stop, seek, setVolume) for their whole duration, so a
///    graph teardown can never interleave with a device start or a seek.
/// 2. `graphMutex_` guards the playback graph itself: `decoder_`, `ring_`,
///    `output_`, and every call that mutates them (Decoder::decode/seek,
///    SpscRing::reset, AudioOutput::start/stop), plus the decode thread's
///    condition-variable wait.
///
/// Ordering is `controlMutex_` -> `graphMutex_`. The decode thread only ever
/// takes `graphMutex_` (never `controlMutex_`), and it holds it for at most
/// one bounded decode chunk (<= 1024 frames), so control operations waiting on
/// it cannot stall for long.
///
/// The miniaudio data callback takes no Player mutex: AudioOutput::stop()
/// joins the callback before the ring is reset or the graph replaced, so the
/// consumer is quiescent while `graphMutex_` is held for those operations.
/// A Decoder::decode() call binds the decoder contract (decode and seek must
/// not overlap), so `graphMutex_` is held for one such call; when the
/// underlying Reader blocks, control operations wait for that call to return,
/// but the audio callback and the device keep running and shutdown is never
/// blocked by a CV wait (stop_token-aware).
struct Player::Impl {
    // Configuration
    uint32_t sampleRate_{48000};
    uint32_t channels_{2};

    // Playback components
    std::unique_ptr<media::Reader> reader_;
    std::unique_ptr<media::Decoder> decoder_;
    // Shared so the decode thread can keep a parked ring alive while it waits
    // after releasing graphMutex_ (P8).
    std::shared_ptr<kanariv::utils::SpscRing<float>> ring_;
    std::unique_ptr<AudioOutput> output_;

    // Threading
    std::jthread decodeThread_;
    /// Serializes control operations; see @par Lock protocol above.
    mutable std::mutex controlMutex_;
    /// Guards the playback graph; see @par Lock protocol above.
    mutable std::mutex graphMutex_;
    std::condition_variable cv_;

    // Atomic state (C ca_player.c atomic fields)
    std::atomic<State> state_{State::Stopped};
    std::atomic<uint64_t> posBase_{0};
    std::atomic<int64_t> posStartMs_{0};
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> openGate_{false};
    std::atomic<bool> decodeBusy_{false};
    std::atomic<float> volume_{1.0f};

    /// True once the decoder reported clean EOS for the current position.
    bool eosReached_{false};
    /// True when stop() left the decoder mid-stream, so play() must rewind.
    bool rewindOnPlay_{false};
    /// True when the audio output uses miniaudio's silent null backend.
    bool nullOutput_{false};

    std::string lastError_;
    kanariv::utils::Logger* logger_{nullptr};

    bool init(const PlayerOptions& opts);
    Player::ExpectedVoid open(std::string_view path);
    Player::ExpectedVoid openReader(std::unique_ptr<media::Reader> reader);
    Player::ExpectedVoid play();
    Player::ExpectedVoid pause();
    Player::ExpectedVoid resume();
    Player::ExpectedVoid stop();
    Player::ExpectedVoid seek(double seconds);
    Player::ExpectedVoid setVolume(float volume);
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] std::chrono::duration<double> position() const noexcept;
    [[nodiscard]] std::string lastError() const;

    /// @pre `controlMutex_` is held by the caller.
    Player::ExpectedVoid stopInternalLocked();
    static media::DecodeStatus preroll(media::Decoder& decoder,
                                       kanariv::utils::SpscRing<float>& ring);
    void decodeLoop(std::stop_token st);
    static int64_t nowMs() noexcept;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}

Player::ExpectedPlayer Player::create(const PlayerOptions& opts) {
    auto p = std::unique_ptr<Player>(new Player());
    if (!p->impl_->init(opts)) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Device,
                                                         std::string_view("player init failed")));
    }
    return std::unique_ptr<Player>(std::move(p));
}

Player::~Player() {
    (void)impl_->stop();
    if (impl_->decodeThread_.joinable()) {
        impl_->decodeThread_.request_stop();
        impl_->cv_.notify_all();
        impl_->decodeThread_.join();
    }
}

Player::ExpectedVoid Player::open(std::string_view path) {
    return impl_->open(path);
}

Player::ExpectedVoid Player::openReader(std::unique_ptr<media::Reader> reader) {
    return impl_->openReader(std::move(reader));
}

Player::ExpectedVoid Player::play() {
    return impl_->play();
}

Player::ExpectedVoid Player::pause() {
    return impl_->pause();
}

Player::ExpectedVoid Player::resume() {
    return impl_->resume();
}

Player::ExpectedVoid Player::stop() {
    return impl_->stop();
}

Player::ExpectedVoid Player::seek(double seconds) {
    return impl_->seek(seconds);
}

Player::ExpectedVoid Player::setVolume(float volume) {
    return impl_->setVolume(volume);
}

State Player::state() const noexcept {
    return impl_->state();
}

std::chrono::duration<double> Player::position() const noexcept {
    return impl_->position();
}

std::string Player::lastError() const {
    return impl_->lastError();
}

bool Player::Impl::init(const PlayerOptions& opts) {
    sampleRate_ = opts.sampleRate ? opts.sampleRate : 48000;
    channels_ = opts.channels ? opts.channels : 2;
    logger_ = opts.logger;
    nullOutput_ = opts.nullOutput;
    volume_.store(1.0f, std::memory_order_relaxed);
    state_.store(State::Stopped, std::memory_order_relaxed);

    // Start decode thread
    decodeThread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });

    return true;
}

Player::ExpectedVoid Player::Impl::open(std::string_view path) {
    if (path.empty()) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("empty path")));
    }
    auto readerResult = kanariv::media::FileReader::open(path);
    if (!readerResult) {
        // Log once, here; openReader() logs its own failures (P11).
        {
            std::lock_guard<std::mutex> control(controlMutex_);
            std::lock_guard<std::mutex> graph(graphMutex_);
            lastError_ = readerResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            if (logger_)
                logger_->warn("player open failed file={} err={}", path, lastError_);
        }
        return std::unexpected(readerResult.error());
    }
    return openReader(std::move(readerResult.value()));
}

Player::ExpectedVoid Player::Impl::openReader(std::unique_ptr<media::Reader> reader) {
    if (!reader) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("null reader")));
    }

    std::lock_guard<std::mutex> control(controlMutex_);

    // Stop playback first: the audio callback is joined and the ring quiesced
    // before the old graph is replaced.
    if (auto stopped = stopInternalLocked(); !stopped)
        return stopped;

    // Build the replacement graph privately; the old graph is only discarded
    // once the new one is complete.
    auto decResult = media::Decoder::open(*reader, logger_);
    if (!decResult) {
        {
            std::lock_guard<std::mutex> graph(graphMutex_);
            lastError_ = decResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            if (logger_)
                logger_->warn("player open failed err={}", lastError_);
        }
        return std::unexpected(decResult.error());
    }

    auto decoder = std::move(decResult.value());

    uint32_t sr = decoder->sampleRate();
    uint32_t ch = decoder->channels();
    if (sr == 0)
        sr = 48000;
    if (ch == 0)
        ch = 2;

    // Ring capacity is shared policy (see player/playback_policy.hpp); the
    // Engine keeps its own documented capacity there too.
    auto ring =
        std::make_shared<kanariv::utils::SpscRing<float>>(playback::policy::playerRingFrames, ch);

    // Create audio output
    AudioOutput::Config cfg;
    cfg.sampleRate = sr;
    cfg.channels = ch;
    cfg.ring = ring.get();
    cfg.volume = volume_.load(std::memory_order_relaxed);
    cfg.logger = logger_;
    cfg.nullBackend = nullOutput_;

    auto outResult = AudioOutput::create(cfg);
    if (!outResult) {
        {
            std::lock_guard<std::mutex> graph(graphMutex_);
            lastError_ = outResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            if (logger_)
                logger_->warn("audio output create failed: {}", lastError_);
        }
        return std::unexpected(outResult.error());
    }

    auto output = std::move(outResult.value());

    // Preroll: decode cap/2 samples before open returns.
    auto prerollStatus = preroll(*decoder, *ring);

    // Publish the new graph. State is committed only after every component is
    // ready and the preroll has run (P12).
    {
        std::lock_guard<std::mutex> graph(graphMutex_);
        output_.reset();
        ring_.reset();
        decoder_.reset();
        reader_.reset();

        decoder_ = std::move(decoder);
        ring_ = std::move(ring);
        output_ = std::move(output);
        reader_ = std::move(reader);

        posBase_.store(0, std::memory_order_relaxed);
        posStartMs_.store(0, std::memory_order_relaxed);
        eosReached_ = false;
        rewindOnPlay_ = false;
        if (prerollStatus == media::DecodeStatus::Error) {
            lastError_ = "preroll decode failed";
            state_.store(State::Error, std::memory_order_release);
            openGate_.store(true, std::memory_order_release);
            cv_.notify_all();
            return std::unexpected(kanariv::utils::makeError(
                kanariv::utils::StatusCode::Io, std::string_view("preroll decode failed")));
        }
        lastError_.clear();
        state_.store(State::Stopped, std::memory_order_release);
        openGate_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    return {};
}

Player::ExpectedVoid Player::Impl::play() {
    std::lock_guard<std::mutex> control(controlMutex_);
    std::lock_guard<std::mutex> graph(graphMutex_);

    if (!decoder_ || !ring_ || !output_) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                         std::string_view("not opened")));
    }

    State s = state_.load(std::memory_order_acquire);
    if (s == State::Playing) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                         std::string_view("already playing")));
    }
    if (s == State::Error) {
        return std::unexpected(kanariv::utils::makeError(
            kanariv::utils::StatusCode::State, std::string_view("error state; reopen to play")));
    }

    // Replay after a clean end of stream, or after stop() left the decoder
    // mid-stream, rewinds to the start (P2).
    if (eosReached_ || rewindOnPlay_) {
        auto seekResult = decoder_->seek(0.0);
        if (!seekResult) {
            lastError_ = seekResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            cv_.notify_all();
            return std::unexpected(seekResult.error());
        }
        ring_->reset();
        posBase_.store(0, std::memory_order_relaxed);
        eosReached_ = false;
        rewindOnPlay_ = false;
    }

    // Set the subordinate fields before publishing the state (P12).
    openGate_.store(true, std::memory_order_release);
    isPlaying_.store(true, std::memory_order_release);
    posStartMs_.store(nowMs(), std::memory_order_release);
    decodeBusy_.store(true, std::memory_order_release);

    if (auto startResult = output_->start(); !startResult) {
        // Never report Playing if the device did not start (P3).
        isPlaying_.store(false, std::memory_order_release);
        decodeBusy_.store(false, std::memory_order_release);
        posStartMs_.store(0, std::memory_order_relaxed);
        lastError_ = startResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        cv_.notify_all();
        return std::unexpected(startResult.error());
    }

    state_.store(State::Playing, std::memory_order_release);
    cv_.notify_all();
    return {};
}

Player::ExpectedVoid Player::Impl::pause() {
    std::lock_guard<std::mutex> control(controlMutex_);
    std::lock_guard<std::mutex> graph(graphMutex_);

    if (state_.load(std::memory_order_acquire) != State::Playing) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                         std::string_view("not playing")));
    }

    isPlaying_.store(false, std::memory_order_release);
    decodeBusy_.store(false, std::memory_order_release);

    if (auto stopResult = output_->stop(); !stopResult) {
        // Never report Paused if the device did not stop (P3).
        posStartMs_.store(0, std::memory_order_relaxed);
        lastError_ = stopResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        cv_.notify_all();
        return std::unexpected(stopResult.error());
    }

    // Fold elapsed playback into the base position.
    int64_t now = nowMs();
    int64_t start = posStartMs_.load(std::memory_order_acquire);
    if (start > 0 && now > start) {
        int64_t elapsedFrames = static_cast<int64_t>((now - start) * sampleRate_ / 1000.0);
        int64_t currentBase = posBase_.load(std::memory_order_relaxed);
        posBase_.store(currentBase + elapsedFrames, std::memory_order_relaxed);
    }
    posStartMs_.store(0, std::memory_order_relaxed);

    state_.store(State::Paused, std::memory_order_release);
    cv_.notify_all();
    return {};
}

Player::ExpectedVoid Player::Impl::resume() {
    std::lock_guard<std::mutex> control(controlMutex_);
    std::lock_guard<std::mutex> graph(graphMutex_);

    if (state_.load(std::memory_order_acquire) != State::Paused) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                         std::string_view("not paused")));
    }

    openGate_.store(true, std::memory_order_release);
    isPlaying_.store(true, std::memory_order_release);
    posStartMs_.store(nowMs(), std::memory_order_release);
    decodeBusy_.store(true, std::memory_order_release);

    if (auto startResult = output_->start(); !startResult) {
        isPlaying_.store(false, std::memory_order_release);
        decodeBusy_.store(false, std::memory_order_release);
        posStartMs_.store(0, std::memory_order_relaxed);
        lastError_ = startResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        cv_.notify_all();
        return std::unexpected(startResult.error());
    }

    state_.store(State::Playing, std::memory_order_release);
    cv_.notify_all();
    return {};
}

Player::ExpectedVoid Player::Impl::stop() {
    std::lock_guard<std::mutex> control(controlMutex_);
    return stopInternalLocked();
}

Player::ExpectedVoid Player::Impl::seek(double seconds) {
    std::lock_guard<std::mutex> control(controlMutex_);
    std::lock_guard<std::mutex> graph(graphMutex_);

    if (!decoder_ || !ring_ || !output_) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                         std::string_view("not opened")));
    }
    if (seconds < 0.0 || std::isnan(seconds) || std::isinf(seconds)) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("bad seconds")));
    }

    State s = state_.load(std::memory_order_acquire);
    const bool wasPlaying = (s == State::Playing);

    // AudioOutput::stop() joins the data callback, so the consumer is quiescent
    // before the ring is reset and the decoder is repositioned.
    if (wasPlaying) {
        isPlaying_.store(false, std::memory_order_release);
        decodeBusy_.store(false, std::memory_order_release);
        if (auto stopResult = output_->stop(); !stopResult) {
            posStartMs_.store(0, std::memory_order_relaxed);
            lastError_ = stopResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            cv_.notify_all();
            return std::unexpected(stopResult.error());
        }
    }

    auto seekResult = decoder_->seek(seconds);
    if (!seekResult) {
        lastError_ = seekResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        cv_.notify_all();
        return std::unexpected(seekResult.error());
    }

    ring_->reset();
    posBase_.store(static_cast<uint64_t>(seconds * sampleRate_), std::memory_order_relaxed);
    eosReached_ = false;
    rewindOnPlay_ = false;

    if (wasPlaying) {
        // Restart from the new position, publishing Playing last (P12).
        isPlaying_.store(true, std::memory_order_release);
        posStartMs_.store(nowMs(), std::memory_order_release);
        decodeBusy_.store(true, std::memory_order_release);
        if (auto startResult = output_->start(); !startResult) {
            isPlaying_.store(false, std::memory_order_release);
            decodeBusy_.store(false, std::memory_order_release);
            posStartMs_.store(0, std::memory_order_relaxed);
            lastError_ = startResult.error().message;
            state_.store(State::Error, std::memory_order_release);
            cv_.notify_all();
            return std::unexpected(startResult.error());
        }
        state_.store(State::Playing, std::memory_order_release);
    } else if (s == State::Paused) {
        posStartMs_.store(0, std::memory_order_relaxed);
    }

    cv_.notify_all();
    return {};
}

Player::ExpectedVoid Player::Impl::setVolume(float volume) {
    if (std::isnan(volume) || std::isinf(volume)) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("bad volume")));
    }
    float vol = kanariv::utils::clampVolume(volume);

    // volume_ and output_ are both touched under controlMutex_ so a concurrent
    // openReader() cannot replace the output between the store and apply.
    std::lock_guard<std::mutex> control(controlMutex_);
    volume_.store(vol, std::memory_order_relaxed);
    if (output_) {
        output_->setVolume(vol);
    }
    return {};
}

State Player::Impl::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

std::string Player::Impl::lastError() const {
    // Control calls write under controlMutex_, the decode thread under
    // graphMutex_ (control-then-graph is the documented order).
    std::lock_guard<std::mutex> control(controlMutex_);
    std::lock_guard<std::mutex> graph(graphMutex_);
    return lastError_;
}

std::chrono::duration<double> Player::Impl::position() const noexcept {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::Stopped || s == State::Error) {
        return std::chrono::duration<double>(0.0);
    }
    uint64_t base = posBase_.load(std::memory_order_relaxed);
    int64_t start = posStartMs_.load(std::memory_order_acquire);
    if (s == State::Playing && start > 0) {
        int64_t now = nowMs();
        if (now > start) {
            double elapsedSec = static_cast<double>(now - start) / 1000.0;
            uint64_t elapsedFrames = static_cast<uint64_t>(elapsedSec * sampleRate_);
            return std::chrono::duration<double>(static_cast<double>(base + elapsedFrames) /
                                                 sampleRate_);
        }
    }
    return std::chrono::duration<double>(static_cast<double>(base) / sampleRate_);
}

Player::ExpectedVoid Player::Impl::stopInternalLocked() {
    std::lock_guard<std::mutex> graph(graphMutex_);

    State s = state_.load(std::memory_order_acquire);
    if (s == State::Stopped && !decoder_) {
        return {};
    }

    // Close the gate and drop the flags before touching the graph so the
    // decode thread cannot start a new chunk on the old position.
    openGate_.store(false, std::memory_order_release);
    isPlaying_.store(false, std::memory_order_release);
    decodeBusy_.store(false, std::memory_order_release);

    std::optional<kanariv::utils::Error> stopError;
    if (output_) {
        if (auto stopResult = output_->stop(); !stopResult) {
            stopError = stopResult.error();
            lastError_ = stopError->message;
        }
    }
    if (ring_)
        ring_->reset();

    posBase_.store(0, std::memory_order_relaxed);
    posStartMs_.store(0, std::memory_order_relaxed);
    eosReached_ = false;
    // The decoder stays where playback stopped; play() rewinds it so the
    // "Stopped == initial state" contract still holds.
    rewindOnPlay_ = (decoder_ != nullptr);
    state_.store(stopError ? State::Error : State::Stopped, std::memory_order_release);
    cv_.notify_all();

    if (stopError)
        return std::unexpected(*stopError);
    return {};
}

media::DecodeStatus Player::Impl::preroll(media::Decoder& decoder,
                                          kanariv::utils::SpscRing<float>& ring) {
    uint32_t ch = decoder.channels();
    if (ch == 0)
        return media::DecodeStatus::Error;
    // Shared policy: have the ring before the device starts. The previous
    // prerollSamples/ch form buffered a quarter ring in stereo while Engine
    // buffered half; both now use prerollFrames().
    std::size_t prerollTarget = playback::policy::prerollFrames(ring.capacity());

    std::size_t chunkFrames = playback::policy::chunkCapFrames(ch);
    std::vector<float> buffer(chunkFrames * ch);
    std::size_t filled = 0;
    media::DecodeStatus status = media::DecodeStatus::Ok;

    while (filled < prerollTarget && ring.availableWrite() >= chunkFrames * ch) {
        std::size_t frames =
            decoder.decode(std::span<float>(buffer.data(), buffer.size()), &status);
        if (status == media::DecodeStatus::Error)
            return status;
        if (frames == 0)
            break;
        (void)ring.write(std::span<const float>(buffer.data(), frames * ch));
        filled += frames;
        if (status == media::DecodeStatus::Eof)
            break;
    }
    return status;
}

void Player::Impl::decodeLoop(std::stop_token st) {
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(playback::policy::maxChunkFrames * 8);

    while (!st.stop_requested()) {
        std::unique_lock<std::mutex> graph(graphMutex_);
        cv_.wait(graph, [this, &st]() {
            return st.stop_requested() || (openGate_.load(std::memory_order_acquire) &&
                                           decodeBusy_.load(std::memory_order_acquire));
        });
        if (st.stop_requested())
            break;

        if (!decoder_ || !ring_ || !output_) {
            decodeBusy_.store(false, std::memory_order_release);
            continue;
        }

        if (!isPlaying_.load(std::memory_order_acquire)) {
            decodeBusy_.store(false, std::memory_order_release);
            continue;
        }

        // Sample the read counter before the space check: a read landing after
        // the sample is then visible to waitForRead() (never missed). The local
        // copy keeps the ring alive once graphMutex_ is released for the wait.
        auto ring = ring_;
        const std::uint64_t consumed = ring->readSequence();
        std::size_t avail = ring->availableWrite();
        if (avail == 0) {
            graph.unlock();
            // Park until the audio callback drains the ring instead of polling
            // it every few milliseconds; the bound covers a stopped consumer.
            (void)ring->waitForRead(consumed, playback::policy::ringWakeBudget);
            continue;
        }

        uint32_t ch = decoder_->channels();
        if (ch == 0) {
            isPlaying_.store(false, std::memory_order_release);
            decodeBusy_.store(false, std::memory_order_release);
            lastError_ = "decoder reports zero channels";
            (void)output_->stop();
            state_.store(State::Error, std::memory_order_release);
            cv_.notify_all();
            continue;
        }
        std::size_t maxFrames = playback::policy::chunkFrames(ch, avail);

        if (scratch.size() < maxFrames * ch)
            scratch.resize(maxFrames * ch);
        std::span<float> chunk(scratch.data(), maxFrames * ch);

        media::DecodeStatus status = media::DecodeStatus::Ok;
        std::size_t frames = decoder_->decode(chunk, &status);

        if (frames > 0) {
            // availableWrite() bounds the chunk, so the write always fits; no
            // partial-write branch exists to check (P9).
            (void)ring_->write(std::span<const float>(scratch.data(), frames * ch));
        }

        switch (playback::policy::classifyDecode(status, frames)) {
        case playback::policy::DecodeOutcome::Failed:
            // A decode failure is not a normal end of stream (P2). Report it
            // through the error state, never as Stopped.
            isPlaying_.store(false, std::memory_order_release);
            decodeBusy_.store(false, std::memory_order_release);
            lastError_ = "decode error";
            (void)output_->stop();
            state_.store(State::Error, std::memory_order_release);
            cv_.notify_all();
            continue;

        case playback::policy::DecodeOutcome::EndOfStream:
            // Drain the tail before ending so the last samples are audible.
            if (ring->availableRead() > 0) {
                graph.unlock();
                // The consumer signal also covers "the tail moved", so the
                // drain ends as soon as the device takes the last frames.
                (void)ring->waitForRead(consumed, playback::policy::ringWakeBudget);
                continue;
            }
            // Clean end of stream: Stopped, and play() will rewind (P2).
            isPlaying_.store(false, std::memory_order_release);
            decodeBusy_.store(false, std::memory_order_release);
            eosReached_ = true;
            (void)output_->stop();
            state_.store(State::Stopped, std::memory_order_release);
            cv_.notify_all();
            continue;

        case playback::policy::DecodeOutcome::Continue:
            break;
        }
    }
}

int64_t Player::Impl::nowMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace kanariv::player
