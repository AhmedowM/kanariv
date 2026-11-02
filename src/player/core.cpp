#include <atomic>
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
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kanariv::player {

/// Player state and playback components, hidden from the public header.
struct Player::Impl {
    // Configuration
    uint32_t sampleRate_{48000};
    uint32_t channels_{2};

    // Playback components
    std::unique_ptr<media::Reader> reader_;
    std::unique_ptr<media::Decoder> decoder_;
    std::unique_ptr<kanariv::utils::SpscRing<float>> ring_;
    std::unique_ptr<AudioOutput> output_;

    // Threading
    std::jthread decodeThread_;
    std::mutex cvMutex_;
    std::condition_variable cv_;
    std::mutex openMutex_;

    // Atomic state (C ca_player.c atomic fields)
    std::atomic<State> state_{State::Stopped};
    std::atomic<uint64_t> posBase_{0};
    std::atomic<int64_t> posStartMs_{0};
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> openGate_{false};
    std::atomic<bool> decodeBusy_{false};
    std::atomic<float> volume_{1.0f};

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
    [[nodiscard]] std::string_view lastError() const noexcept;

    Player::ExpectedVoid stopInternal();
    void preroll();
    void decodeLoop(std::stop_token st);
    static int64_t nowMs() noexcept;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}

Player::ExpectedPlayer Player::create(const PlayerOptions& opts) {
    auto p = std::unique_ptr<Player>(new Player());
    if (!p->impl_->init(opts)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Device,
                                                    std::string_view("player init failed")});
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

bool Player::Impl::init(const PlayerOptions& opts) {
    sampleRate_ = opts.sampleRate ? opts.sampleRate : 48000;
    channels_ = opts.channels ? opts.channels : 2;
    logger_ = opts.logger;
    volume_.store(1.0f, std::memory_order_relaxed);
    state_.store(State::Stopped, std::memory_order_relaxed);

    // Start decode thread
    decodeThread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });

    return true;
}

Player::ExpectedVoid Player::Impl::open(std::string_view path) {
    if (path.empty()) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("empty path")});
    }
    auto readerResult = kanariv::media::FileReader::open(path);
    if (!readerResult) {
        lastError_ = readerResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        if (logger_)
            logger_->warn("player open failed file={} err={}", path, lastError_);
        return std::unexpected(readerResult.error());
    }
    auto rRes = openReader(std::move(readerResult.value()));
    if (!rRes && logger_)
        logger_->warn("player open failed file={} err={}", path, lastError_);
    return rRes;
}

Player::ExpectedVoid Player::Impl::openReader(std::unique_ptr<media::Reader> reader) {
    if (!reader) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("null reader")});
    }

    std::lock_guard<std::mutex> lk(openMutex_);

    // Stop any existing playback
    if (state_.load(std::memory_order_acquire) != State::Stopped) {
        (void)stopInternal();
    }

    // Open decoder
    auto decResult = media::Decoder::open(*reader, logger_);
    if (!decResult) {
        lastError_ = decResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        return std::unexpected(decResult.error());
    }

    decoder_ = std::move(decResult.value());
    reader_ = std::move(reader);

    // Create ring buffer: 8192 frames in all layouts (P-3). Capacity is in
    // frames; buf_ holds cap_*channels floats. The old 8192*ch passed frames
    // but scaled by channels twice (8192*ch*ch floats, ~1.1 MB at 5.1).
    // NOTE: engine ring intentionally NOT flattened (see engine.cpp note) --
    // Player has no daemon playback path, so this is inert there.
    uint32_t sr = decoder_->sampleRate();
    uint32_t ch = decoder_->channels();
    if (sr == 0)
        sr = 48000;
    if (ch == 0)
        ch = 2;

    ring_ = std::make_unique<kanariv::utils::SpscRing<float>>(8192, ch);

    // Create audio output
    AudioOutput::Config cfg;
    cfg.sampleRate = sr;
    cfg.channels = ch;
    cfg.ring = ring_.get();
    cfg.volume = volume_.load(std::memory_order_relaxed);
    cfg.logger = logger_;

    auto outResult = AudioOutput::create(cfg);
    if (!outResult) {
        lastError_ = outResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        if (logger_)
            logger_->warn("audio output create failed: {}", lastError_);
        return std::unexpected(outResult.error());
    }

    output_ = std::move(outResult.value());

    // Preroll: decode cap/2 samples before play returns
    preroll();

    posBase_.store(0, std::memory_order_relaxed);
    posStartMs_.store(0, std::memory_order_relaxed);
    isPlaying_.store(false, std::memory_order_relaxed);
    decodeBusy_.store(false, std::memory_order_relaxed);
    openGate_.store(true, std::memory_order_release);
    lastError_.clear();
    state_.store(State::Stopped, std::memory_order_release);

    return {};
}

Player::ExpectedVoid Player::Impl::play() {
    if (!decoder_ || !output_ || !ring_) {
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::State, std::string_view("not opened")});
    }

    State expected = State::Stopped;
    if (!state_.compare_exchange_strong(expected, State::Playing, std::memory_order_acq_rel)) {
        expected = State::Paused;
        if (!state_.compare_exchange_strong(expected, State::Playing, std::memory_order_acq_rel)) {
            return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                        std::string_view("already playing")});
        }
    }

    isPlaying_.store(true, std::memory_order_release);
    posStartMs_.store(nowMs(), std::memory_order_release);

    // Start audio device
    output_->start();

    // Wake decode thread
    {
        std::lock_guard<std::mutex> lk(cvMutex_);
        decodeBusy_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    return {};
}

Player::ExpectedVoid Player::Impl::pause() {
    State expected = State::Playing;
    if (!state_.compare_exchange_strong(expected, State::Paused, std::memory_order_acq_rel)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                    std::string_view("not playing")});
    }

    isPlaying_.store(false, std::memory_order_release);
    output_->stop();

    // Update posBase with elapsed time
    int64_t now = nowMs();
    int64_t start = posStartMs_.load(std::memory_order_acquire);
    if (start > 0 && now > start) {
        int64_t elapsedFrames = static_cast<int64_t>((now - start) * sampleRate_ / 1000.0);
        int64_t currentBase = posBase_.load(std::memory_order_relaxed);
        posBase_.store(currentBase + elapsedFrames, std::memory_order_relaxed);
    }
    posStartMs_.store(0, std::memory_order_relaxed);

    return {};
}

Player::ExpectedVoid Player::Impl::resume() {
    State expected = State::Paused;
    if (!state_.compare_exchange_strong(expected, State::Playing, std::memory_order_acq_rel)) {
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::State, std::string_view("not paused")});
    }

    isPlaying_.store(true, std::memory_order_release);
    posStartMs_.store(nowMs(), std::memory_order_release);
    output_->start();

    {
        std::lock_guard<std::mutex> lk(cvMutex_);
        decodeBusy_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    return {};
}

Player::ExpectedVoid Player::Impl::stop() {
    std::lock_guard<std::mutex> lk(openMutex_);
    return stopInternal();
}

Player::ExpectedVoid Player::Impl::seek(double seconds) {
    if (!decoder_ || !ring_) {
        return std::unexpected(
            kanariv::utils::Error{kanariv::utils::StatusCode::State, std::string_view("not opened")});
    }
    if (seconds < 0.0 || std::isnan(seconds) || std::isinf(seconds)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad seconds")});
    }

    auto seekRes = decoder_->seek(seconds);
    if (!seekRes) {
        lastError_ = seekRes.error().message;
        state_.store(State::Error, std::memory_order_release);
        return std::unexpected(seekRes.error());
    }

    uint64_t newPosBase = static_cast<uint64_t>(seconds * sampleRate_);
    posBase_.store(newPosBase, std::memory_order_relaxed);
    ring_->reset();

    // If playing, update posStartMs and restart decode thread
    State s = state_.load(std::memory_order_acquire);
    if (s == State::Playing) {
        posStartMs_.store(nowMs(), std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(cvMutex_);
            decodeBusy_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    } else if (s == State::Paused) {
        posStartMs_.store(0, std::memory_order_relaxed);
    }

    return {};
}

Player::ExpectedVoid Player::Impl::setVolume(float volume) {
    if (std::isnan(volume) || std::isinf(volume)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad volume")});
    }
    float vol = kanariv::utils::clampVolume(volume);
    volume_.store(vol, std::memory_order_relaxed);
    if (output_) {
        output_->setVolume(vol);
    }
    return {};
}

State Player::Impl::state() const noexcept {
    return state_.load(std::memory_order_acquire);
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

std::string_view Player::Impl::lastError() const noexcept {
    return lastError_;
}

Player::ExpectedVoid Player::Impl::stopInternal() {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::Stopped && !decoder_) {
        return {};
    }

    isPlaying_.store(false, std::memory_order_release);
    if (output_) {
        output_->stop();
    }
    if (ring_) {
        ring_->reset();
    }
    posBase_.store(0, std::memory_order_relaxed);
    posStartMs_.store(0, std::memory_order_relaxed);
    state_.store(State::Stopped, std::memory_order_release);
    openGate_.store(false, std::memory_order_release);

    // Wake decode thread to exit cleanly
    cv_.notify_all();

    return {};
}

void Player::Impl::preroll() {
    if (!decoder_ || !ring_)
        return;

    // Preroll cap/2 samples (C ca_player.c:462)
    std::size_t ringCap = ring_->capacity();
    std::size_t prerollSamples = ringCap / 2;
    uint32_t ch = decoder_->channels();
    std::size_t prerollFrames = prerollSamples / ch;

    // Use fixed chunk size: min(avail, 1024, 2048/ch) like C ca_player.c:156
    std::size_t chunkFrames = 1024;
    std::size_t maxChunk = 2048 / ch;
    if (chunkFrames > maxChunk)
        chunkFrames = maxChunk;

    std::vector<float> buffer(chunkFrames * ch);
    std::size_t filled = 0;

    while (filled < prerollFrames && ring_->availableWrite() >= chunkFrames * ch) {
        std::size_t frames = decoder_->decode(std::span<float>(buffer.data(), buffer.size()));
        if (frames == 0)
            break;
        std::size_t samples = frames * ch;
        ring_->write(std::span<float>(buffer.data(), samples));
        filled += frames;
    }
}

void Player::Impl::decodeLoop(std::stop_token st) {
    constexpr std::size_t maxChunkFrames = 1024;
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(maxChunkFrames * 8);

    while (!st.stop_requested()) {
        // Wait for openGate and decodeBusy
        {
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait(lk, [this, &st]() {
                return st.stop_requested() || (openGate_.load(std::memory_order_acquire) &&
                                               decodeBusy_.load(std::memory_order_acquire));
            });
            if (st.stop_requested())
                break;
        }

        if (!decoder_ || !ring_ || !output_)
            continue;

        // Check if still playing
        if (!isPlaying_.load(std::memory_order_acquire)) {
            decodeBusy_.store(false, std::memory_order_release);
            continue;
        }

        // Check available space in ring
        std::size_t avail = ring_->availableWrite();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        uint32_t ch = decoder_->channels();
        std::size_t maxFrames = avail / ch;
        if (maxFrames > maxChunkFrames)
            maxFrames = maxChunkFrames;
        std::size_t maxChunkByCh = 2048 / ch;
        if (maxFrames > maxChunkByCh)
            maxFrames = maxChunkByCh;
        if (maxFrames == 0)
            maxFrames = 1;

        if (scratch.size() < maxFrames * ch)
            scratch.resize(maxFrames * ch);
        std::span<float> chunk(scratch.data(), maxFrames * ch);
        std::size_t frames = decoder_->decode(chunk);

        if (frames == 0) {
            // EOF reached
            isPlaying_.store(false, std::memory_order_release);
            output_->stop();
            state_.store(State::Stopped, std::memory_order_release);
            decodeBusy_.store(false, std::memory_order_release);
            continue;
        }

        std::size_t samples = frames * ch;
        std::size_t writtenFrames = ring_->write(std::span<float>(scratch.data(), samples));
        if (writtenFrames < frames) {
            // Ring full, will retry next iteration
        }
    }
}

int64_t Player::Impl::nowMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace kanariv::player
