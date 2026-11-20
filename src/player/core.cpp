#include <cmath>
#include <cstdint>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <player/playback_policy.hpp>
#include "pipeline.hpp"

namespace kanariv::player {

namespace {

/**
 * @brief Decoder-backed producer for the playback pipeline.
 * @details Owns the reader and decoder of one opened track. A parked producer
 * may still call produce() after the Player stopped or reopened the graph, so
 * the decoder lives here rather than in the Player: a stale call decodes the
 * old decoder (and its frames are discarded by the generation token) instead
 * of consuming the first chunk of the replacement decoder.
 */
class DecoderSource final : public detail::Source {
  public:
    DecoderSource(std::unique_ptr<kanariv::media::Reader> reader,
                  std::unique_ptr<kanariv::media::Decoder> decoder)
        : reader_(std::move(reader)), decoder_(std::move(decoder)),
          channels_(decoder_->channels()) {}

    [[nodiscard]] std::uint32_t sampleRate() const noexcept {
        return decoder_->sampleRate();
    }
    [[nodiscard]] std::uint32_t channels() const noexcept {
        return channels_;
    }

    Result produce(std::span<float> out, detail::SourceToken token) override {
        std::lock_guard<std::mutex> lk(mtx_);
        // A stale epoch must not consume frames: they belong to a graph that
        // no longer reads this source.
        if (!token.current())
            return {};
        const std::uint32_t ch = decoder_->channels();
        if (ch == 0)
            return {{}, Status::Failed, "decoder reports zero channels"};
        if (ch != channels_)
            return {{}, Status::Failed, "decoder channel layout changed"};
        media::DecodeStatus status = media::DecodeStatus::Ok;
        const std::size_t frames = decoder_->decode(out, &status);
        switch (kanariv::playback::policy::classifyDecode(status, frames)) {
        case kanariv::playback::policy::DecodeOutcome::Failed:
            // A decode failure is not a normal end of stream (P2).
            return {{}, Status::Failed, "decode error"};
        case kanariv::playback::policy::DecodeOutcome::EndOfStream:
            return {{}, Status::EndOfStream};
        case kanariv::playback::policy::DecodeOutcome::Continue:
            break;
        }
        return {out.first(frames * ch), Status::Continue};
    }

    kanariv::utils::Expected<void> seek(double seconds) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto result = decoder_->seek(seconds);
        if (!result)
            return std::unexpected(result.error());
        return {};
    }

  private:
    std::unique_ptr<kanariv::media::Reader> reader_;
    std::unique_ptr<kanariv::media::Decoder> decoder_;
    std::uint32_t channels_{2};
    /// Serializes produce() (pipeline thread) with seek() (control thread).
    std::mutex mtx_;
};

} // namespace

/// Player control surface over the shared playback pipeline.
///
/// @par Lock protocol
/// `controlMutex_` serializes whole control sequences (openReader stops the old
/// graph, builds the decoder, opens the pipeline and prerolls), so no other
/// control call can observe a half-replaced graph. Everything below it -- the
/// ring, sink, producer thread, clock and transport state -- belongs to
/// detail::Pipeline, which serializes its own operations. A decode error or a
/// clean end of stream is committed by the pipeline's producer thread, so the
/// Player keeps no asynchronous state of its own.
struct Player::Impl {
    std::unique_ptr<detail::Pipeline> pipeline_;
    mutable std::mutex controlMutex_;
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

Player::~Player() = default;

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
    logger_ = opts.logger;
    detail::Pipeline::Config cfg;
    cfg.ringFrames = kanariv::playback::policy::playerRingFrames;
    cfg.logger = opts.logger;
    cfg.nullBackend = opts.nullOutput;
    pipeline_ = std::make_unique<detail::Pipeline>(std::move(cfg));
    return pipeline_ != nullptr;
}

Player::ExpectedVoid Player::Impl::open(std::string_view path) {
    if (path.empty()) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("empty path")));
    }
    auto readerResult = kanariv::media::FileReader::open(path);
    if (!readerResult) {
        // Log once, here; openReader() logs its own failures (P11).
        std::lock_guard<std::mutex> control(controlMutex_);
        pipeline_->fail(readerResult.error().message);
        if (logger_)
            logger_->warn("player open failed file={} err={}", path,
                          readerResult.error().message);
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

    // Stop playback first: the device is quiesced and the ring reset before the
    // graph is replaced.
    if (auto stopped = pipeline_->stop(); !stopped)
        return stopped;

    auto decResult = media::Decoder::open(*reader, logger_);
    if (!decResult) {
        pipeline_->fail(decResult.error().message);
        if (logger_)
            logger_->warn("player open failed err={}", decResult.error().message);
        return std::unexpected(decResult.error());
    }

    auto source = std::make_shared<DecoderSource>(std::move(reader), std::move(decResult.value()));
    const std::uint32_t sampleRate = source->sampleRate() ? source->sampleRate() : 48000;
    const std::uint32_t channels = source->channels() ? source->channels() : 2;

    if (auto opened = pipeline_->open(sampleRate, channels, source); !opened) {
        pipeline_->fail(opened.error().message);
        if (logger_)
            logger_->warn("audio output create failed: {}", opened.error().message);
        return opened;
    }

    // Preroll: the ring is half full before open returns, and a decode failure
    // here surfaces as an open error (state stays Error until the next open).
    if (auto prerolled = pipeline_->preroll(); !prerolled) {
        if (logger_)
            logger_->warn("player open failed err={}", prerolled.error().message);
        return prerolled;
    }
    return {};
}

Player::ExpectedVoid Player::Impl::play() {
    std::lock_guard<std::mutex> control(controlMutex_);
    return pipeline_->play();
}

Player::ExpectedVoid Player::Impl::pause() {
    std::lock_guard<std::mutex> control(controlMutex_);
    return pipeline_->pause();
}

Player::ExpectedVoid Player::Impl::resume() {
    std::lock_guard<std::mutex> control(controlMutex_);
    return pipeline_->resume();
}

Player::ExpectedVoid Player::Impl::stop() {
    std::lock_guard<std::mutex> control(controlMutex_);
    return pipeline_->stop();
}

Player::ExpectedVoid Player::Impl::seek(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("bad seconds")));
    }
    std::lock_guard<std::mutex> control(controlMutex_);
    return pipeline_->seek(seconds);
}

Player::ExpectedVoid Player::Impl::setVolume(float volume) {
    if (std::isnan(volume) || std::isinf(volume)) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("bad volume")));
    }
    std::lock_guard<std::mutex> control(controlMutex_);
    pipeline_->setVolume(volume);
    return {};
}

State Player::Impl::state() const noexcept {
    return pipeline_->state();
}

std::chrono::duration<double> Player::Impl::position() const noexcept {
    return pipeline_->position();
}

std::string Player::Impl::lastError() const {
    return pipeline_->lastError();
}

} // namespace kanariv::player
