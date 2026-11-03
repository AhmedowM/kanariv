#include <algorithm>
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
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
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

constexpr uint32_t defaultSampleRate = 48000;
constexpr uint32_t defaultChannels = 2;
constexpr uint32_t defaultRingFrames = 8192;
constexpr double msToSecDivisor = 1000.0;
constexpr int ringBufferSize = 1024;
constexpr int volumeStepMs = 5;
constexpr int ringBufferSize2048 = 2048;
constexpr int scratchChannels = 8;

/// Player state and playback components, hidden from the public header.
struct Player::Impl {
    // Configuration
    uint32_t sampleRate_{defaultSampleRate};
    uint32_t channels_{defaultChannels};

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

    auto init(const PlayerOptions& opts) -> bool;
    auto open(std::string_view path) -> Player::ExpectedVoid;
    auto openReader(std::unique_ptr<media::Reader> reader) -> Player::ExpectedVoid;
    auto play() -> Player::ExpectedVoid;
    auto pause() -> Player::ExpectedVoid;
    auto resume() -> Player::ExpectedVoid;
    auto stop() -> Player::ExpectedVoid;
    auto seek(double seconds) -> Player::ExpectedVoid;
    auto setVolume(float volume) -> Player::ExpectedVoid;
    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto position() const noexcept -> std::chrono::duration<double>;
    [[nodiscard]] auto lastError() const noexcept -> std::string_view;

    auto stopInternal() -> Player::ExpectedVoid;
    void preroll() const;
    void decodeLoop(std::stop_token st);
    static auto nowMs() noexcept -> int64_t;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}

auto Player::create(const PlayerOptions& opts) -> Player::ExpectedPlayer {
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

auto Player::open(std::string_view path) -> Player::ExpectedVoid {
    return impl_->open(path);
}

auto Player::openReader(std::unique_ptr<media::Reader> reader) -> Player::ExpectedVoid {
    return impl_->openReader(std::move(reader));
}

auto Player::play() -> Player::ExpectedVoid {
    return impl_->play();
}

auto Player::pause() -> Player::ExpectedVoid {
    return impl_->pause();
}

auto Player::resume() -> Player::ExpectedVoid {
    return impl_->resume();
}

auto Player::stop() -> Player::ExpectedVoid {
    return impl_->stop();
}

auto Player::seek(double seconds) -> Player::ExpectedVoid {
    return impl_->seek(seconds);
}

auto Player::setVolume(float volume) -> Player::ExpectedVoid {
    return impl_->setVolume(volume);
}

auto Player::state() const noexcept -> State {
    return impl_->state();
}

auto Player::position() const noexcept -> std::chrono::duration<double> {
    return impl_->position();
}

auto Player::Impl::init(const PlayerOptions& opts) -> bool {
    sampleRate_ = opts.sampleRate != 0 ? opts.sampleRate : defaultSampleRate;
    channels_ = opts.channels != 0 ? opts.channels : defaultChannels;
    logger_ = opts.logger;
    volume_.store(1.0f, std::memory_order_relaxed);
    state_.store(State::Stopped, std::memory_order_relaxed);

    // Start decode thread
    decodeThread_ = std::jthread([this](std::stop_token st) -> void { decodeLoop(st); });

    return true;
}

auto Player::Impl::open(std::string_view path) -> Player::ExpectedVoid {
    if (path.empty()) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("empty path")});
    }
    auto readerResult = kanariv::media::FileReader::open(path);
    if (!readerResult) {
        lastError_ = readerResult.error().message;
        state_.store(State::Error, std::memory_order_release);
        if (logger_ != nullptr) {
            logger_->warn("player open failed file={} err={}", path, lastError_);
        }
        return std::unexpected(readerResult.error());
    }
    auto rRes = openReader(std::move(readerResult.value()));
    if (!rRes && logger_ != nullptr) {
        logger_->warn("player open failed file={} err={}", path, lastError_);
    }
    return rRes;
}

auto Player::Impl::openReader(std::unique_ptr<media::Reader> reader) -> Player::ExpectedVoid {
    if (!reader) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("null reader")});
    }

    std::scoped_lock lk(openMutex_);

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
    if (sr == 0) {
        sr = defaultSampleRate;
    }
    if (ch == 0) {
        ch = defaultChannels;
    }

    ring_ = std::make_unique<kanariv::utils::SpscRing<float>>(defaultRingFrames, ch);

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
        if (logger_ != nullptr) {
            logger_->warn("audio output create failed: {}", lastError_);
        }
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

auto Player::Impl::play() -> Player::ExpectedVoid {
    if (!decoder_ || !output_ || !ring_) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                     std::string_view("not opened")});
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
        std::scoped_lock lk(cvMutex_);
        decodeBusy_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    return {};
}

auto Player::Impl::pause() -> Player::ExpectedVoid {
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
        auto elapsedFrames = static_cast<int64_t>((now - start) * sampleRate_ / msToSecDivisor);
        int64_t currentBase = posBase_.load(std::memory_order_relaxed);
        posBase_.store(currentBase + elapsedFrames, std::memory_order_relaxed);
    }
    posStartMs_.store(0, std::memory_order_relaxed);

    return {};
}

auto Player::Impl::resume() -> Player::ExpectedVoid {
    State expected = State::Paused;
    if (!state_.compare_exchange_strong(expected, State::Playing, std::memory_order_acq_rel)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                     std::string_view("not paused")});
    }

    isPlaying_.store(true, std::memory_order_release);
    posStartMs_.store(nowMs(), std::memory_order_release);
    output_->start();

    {
        std::scoped_lock lk(cvMutex_);
        decodeBusy_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    return {};
}

auto Player::Impl::stop() -> Player::ExpectedVoid {
    std::scoped_lock lk(openMutex_);
    return stopInternal();
}

auto Player::Impl::seek(double seconds) -> Player::ExpectedVoid {
    if (!decoder_ || !ring_) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                     std::string_view("not opened")});
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

    auto newPosBase = static_cast<uint64_t>(seconds * sampleRate_);
    posBase_.store(newPosBase, std::memory_order_relaxed);
    ring_->reset();

    // If playing, update posStartMs and restart decode thread
    State s = state_.load(std::memory_order_acquire);
    if (s == State::Playing) {
        posStartMs_.store(nowMs(), std::memory_order_release);
        {
            std::scoped_lock lk(cvMutex_);
            decodeBusy_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    } else if (s == State::Paused) {
        posStartMs_.store(0, std::memory_order_relaxed);
    }

    return {};
}

auto Player::Impl::setVolume(float volume) -> Player::ExpectedVoid {
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

auto Player::Impl::state() const noexcept -> State {
    return state_.load(std::memory_order_acquire);
}

auto Player::Impl::position() const noexcept -> std::chrono::duration<double> {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::Stopped || s == State::Error) {
        return std::chrono::duration<double>(0.0);
    }
    uint64_t base = posBase_.load(std::memory_order_relaxed);
    int64_t start = posStartMs_.load(std::memory_order_acquire);
    if (s == State::Playing && start > 0) {
        int64_t now = nowMs();
        if (now > start) {
            double elapsedSec = static_cast<double>(now - start) / msToSecDivisor;
            auto elapsedFrames = static_cast<uint64_t>(elapsedSec * sampleRate_);
            return std::chrono::duration<double>(static_cast<double>(base + elapsedFrames) /
                                                 sampleRate_);
        }
    }
    return std::chrono::duration<double>(static_cast<double>(base) / sampleRate_);
}

auto Player::Impl::lastError() const noexcept -> std::string_view {
    return lastError_;
}

auto Player::Impl::stopInternal() -> Player::ExpectedVoid {
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

void Player::Impl::preroll() const {
    if (!decoder_ || !ring_) {
        return;
    }

    // Preroll cap/2 samples (C ca_player.c:462)
    std::size_t ringCap = ring_->capacity();
    std::size_t prerollSamples = ringCap / 2;
    uint32_t ch = decoder_->channels();
    std::size_t prerollFrames = prerollSamples / ch;

    // Use fixed chunk size: min(avail, 1024, 2048/ch) like C ca_player.c:156
    std::size_t chunkFrames = ringBufferSize;
    std::size_t maxChunk = ringBufferSize2048 / ch;
    chunkFrames = std::min(chunkFrames, maxChunk);

    std::vector<float> buffer(chunkFrames * ch);
    std::size_t filled = 0;

    while (filled < prerollFrames && ring_->availableWrite() >= chunkFrames * ch) {
        std::size_t frames = decoder_->decode(std::span<float>(buffer.data(), buffer.size()));
        if (frames == 0) {
            break;
        }
        std::size_t samples = frames * ch;
        ring_->write(std::span<float>(buffer.data(), samples));
        filled += frames;
    }
}

void Player::Impl::decodeLoop(std::stop_token st) {
    constexpr std::size_t maxChunkFrames = 1024;
    // Reusable decode scratch (P-2): grown only, never reallocated per chunk.
    std::vector<float> scratch;
    scratch.reserve(maxChunkFrames * scratchChannels);

    while (!st.stop_requested()) {
        // Wait for openGate and decodeBusy
        {
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait(lk, [this, &st]() -> bool {
                return st.stop_requested() || (openGate_.load(std::memory_order_acquire) &&
                                               decodeBusy_.load(std::memory_order_acquire));
            });
            if (st.stop_requested()) {
                break;
            }
        }

        if (!decoder_ || !ring_ || !output_) {
            continue;
        }

        // Check if still playing
        if (!isPlaying_.load(std::memory_order_acquire)) {
            decodeBusy_.store(false, std::memory_order_release);
            continue;
        }

        // Check available space in ring
        std::size_t avail = ring_->availableWrite();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(volumeStepMs));
            continue;
        }

        uint32_t ch = decoder_->channels();
        std::size_t maxFrames = avail / ch;
        maxFrames = std::min(maxFrames, maxChunkFrames);
        std::size_t maxChunkByCh = ringBufferSize2048 / ch;
        maxFrames = std::min(maxFrames, maxChunkByCh);
        if (maxFrames == 0) {
            maxFrames = 1;
        }

        if (scratch.size() < maxFrames * ch) {
            scratch.resize(maxFrames * ch);
        }
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

auto Player::Impl::nowMs() noexcept -> int64_t {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace kanariv::player
