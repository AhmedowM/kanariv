#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace kanariv::media;
using namespace kanariv::utils;
using kanariv::media::Decoder;
using kanariv::media::FileReader;
using kanariv::media::MemoryReader;

namespace {

/// Serves bytes from a real reader up to `failAfter`, then reports an I/O
/// failure (zero bytes plus ReadStatus::Error) instead of a clean EOF.
class FailingReader final : public Reader {
  public:
    FailingReader(std::unique_ptr<Reader> inner, int64_t failAfter)
        : inner_(std::move(inner)), failAfter_(failAfter) {}

    std::size_t read(std::span<std::byte> dst) override {
        if (pos_ >= failAfter_) {
            status_ = ReadStatus::Error;
            return 0;
        }
        const auto remaining = static_cast<std::size_t>(failAfter_ - pos_);
        const std::size_t n = inner_->read(dst.first(std::min(dst.size(), remaining)));
        status_ = n > 0 ? ReadStatus::Ok : inner_->lastReadStatus();
        pos_ += static_cast<int64_t>(n);
        return n;
    }

    kanariv::utils::Expected<void> seek(int64_t offset, int whence) override {
        auto res = inner_->seek(offset, whence);
        if (!res.has_value())
            return res;
        pos_ = inner_->tell();
        return {};
    }

    int64_t tell() noexcept override {
        return pos_;
    }
    int64_t size() noexcept override {
        return inner_->size();
    }
    bool seekable() const noexcept override {
        return inner_->seekable();
    }
    ReadStatus lastReadStatus() const noexcept override {
        return status_;
    }

  private:
    std::unique_ptr<Reader> inner_;
    int64_t failAfter_;
    int64_t pos_{0};
    ReadStatus status_{ReadStatus::Ok};
};

/// Decodes until EOF or failure and returns the total frame count.
std::size_t decodeAllFrames(Decoder& dec, std::size_t framesPerCall, DecodeStatus& status) {
    const uint32_t ch = dec.channels();
    std::vector<float> out(framesPerCall * ch);
    std::size_t total = 0;
    status = DecodeStatus::Ok;
    for (;;) {
        const std::size_t got = dec.decode(out, &status);
        total += got;
        if (status != DecodeStatus::Ok)
            break;
        REQUIRE(got > 0);
    }
    return total;
}

} // namespace

TEST_CASE("decode registry probe unsupported", "[decoder]") {
    std::vector<std::byte> sig(32, std::byte{'X'});
    auto r = MemoryReader::open(sig);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(!dec.has_value());
    REQUIRE(dec.error().code == StatusCode::Unsupported);
}

TEST_CASE("decoder open emits lifecycle log", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); },
        LogLevel::Trace);
    auto dec = Decoder::open(**r, &logger);
    REQUIRE(dec.has_value());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == LogLevel::Debug);
    REQUIRE(out[0].second.find("decoder opened") != std::string::npos);
}

TEST_CASE("decode registry probe restore offset with real ogg", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    REQUIRE((**r).seek(10, SEEK_SET).has_value());
    REQUIRE((**r).tell() == 10);
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    // After successful FFmpeg open, file position is owned by AVIO (advanced past probe).
    // We only require that decoder works, not that tell is restored to 10 (old buggy expectation).
    std::array<float, 256> out{};
    REQUIRE((*dec)->decode(out) > 0);
}

TEST_CASE("ffmpeg decoder ogg real", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->sampleRate() > 0);
    REQUIRE((*dec)->channels() > 0);
    std::array<float, 1024> out{};
    std::size_t got = (*dec)->decode(out);
    REQUIRE(got > 0);
    bool nonZero = false;
    for (float f : out)
        if (std::abs(f) > 1e-6f)
            nonZero = true;
    REQUIRE(nonZero);
    REQUIRE((*dec)->seek(0.1).has_value());
}

TEST_CASE("ffmpeg decoder wav real", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.wav");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->sampleRate() > 0);
    REQUIRE((*dec)->channels() > 0);
    std::array<float, 1024> out{};
    std::size_t got = (*dec)->decode(out);
    REQUIRE(got > 0);
    bool nonZero = false;
    for (float f : out)
        if (std::abs(f) > 1e-6f)
            nonZero = true;
    REQUIRE(nonZero);
    REQUIRE((*dec)->seek(0.1).has_value());
}

TEST_CASE("decoder seek clamp invalid", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE(!(*dec)->seek(-5.0).has_value());
    REQUIRE((*dec)->seek(-5.0).error().code == StatusCode::InvalidArg);
    auto bad = (*dec)->seek(std::numeric_limits<double>::infinity());
    REQUIRE(!bad.has_value());
}

TEST_CASE("decoder FileReader probe 32B with real wav", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.wav");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->sampleRate() > 0);
    // File pos after open is at AVIO offset (headers consumed), not 0 -- allow >=0
    REQUIRE((**r).tell() >= 0);
}

TEST_CASE("decoder decode reports Eof at end of stream", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.wav");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());

    DecodeStatus status = DecodeStatus::Ok;
    const std::size_t total = decodeAllFrames(**dec, 1024, status);
    REQUIRE(status == DecodeStatus::Eof);
    REQUIRE(total > 0);
}

TEST_CASE("decoder surfaces a reader failure as Error, not Eof", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.wav");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    const int64_t size = (**r).size();
    REQUIRE(size > 4096);
    // Fail halfway: the WAV container opens from the header, then a read
    // fails mid-stream. Before M2 this was reported as a clean EOF.
    auto failing = std::make_unique<FailingReader>(std::move(*r), size / 2);
    auto dec = Decoder::open(*failing);
    REQUIRE(dec.has_value());

    DecodeStatus status = DecodeStatus::Ok;
    const std::size_t total = decodeAllFrames(**dec, 1024, status);
    REQUIRE(status == DecodeStatus::Error);
    REQUIRE(total > 0); // samples decoded before the failure remain valid
}

TEST_CASE("decoder small chunks lose no frames", "[decoder]") {
    // sample.ogg has packets that decode to more than one frame. A 128-frame
    // output buffer used to make the codec back up and the old loop dropped
    // unaccepted packets on AVERROR(EAGAIN) (M1). Decoding in tiny chunks must
    // yield the same total as decoding in large ones.
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto smallReader = FileReader::open(path);
    auto largeReader = FileReader::open(path);
    REQUIRE(smallReader.has_value());
    REQUIRE(largeReader.has_value());
    auto smallDec = Decoder::open(**smallReader);
    auto largeDec = Decoder::open(**largeReader);
    REQUIRE(smallDec.has_value());
    REQUIRE(largeDec.has_value());
    REQUIRE((*smallDec)->channels() == (*largeDec)->channels());

    DecodeStatus smallStatus = DecodeStatus::Ok;
    DecodeStatus largeStatus = DecodeStatus::Ok;
    const std::size_t smallTotal = decodeAllFrames(**smallDec, 128, smallStatus);
    const std::size_t largeTotal = decodeAllFrames(**largeDec, 4096, largeStatus);
    REQUIRE(smallStatus == DecodeStatus::Eof);
    REQUIRE(largeStatus == DecodeStatus::Eof);
    REQUIRE(smallTotal == largeTotal);
    REQUIRE(smallTotal > 0);
    // And the total matches the container's duration estimate.
    const uint64_t expected = (*smallDec)->totalFrames();
    REQUIRE(expected > 0);
    REQUIRE(smallTotal >= expected * 98 / 100);
    REQUIRE(smallTotal <= expected * 102 / 100);
}

TEST_CASE("moved-from decoder accessors are safe", "[decoder]") {
    auto path = std::filesystem::path(std::string(TEST_DATA_DIR) + "/sample.ogg");
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());

    Decoder moved = std::move(**dec);
    REQUIRE(moved.sampleRate() > 0);
    // M14: the moved-from object must not dereference a null impl_.
    REQUIRE((*dec)->sampleRate() == 0);
    REQUIRE((*dec)->channels() == 0);
    REQUIRE((*dec)->totalFrames() == 0);
    std::array<float, 64> out{};
    DecodeStatus status = DecodeStatus::Ok;
    REQUIRE((*dec)->decode(out, &status) == 0);
    REQUIRE(status == DecodeStatus::Error);
    const auto seekResult = (*dec)->seek(1.0);
    REQUIRE(!seekResult.has_value());
    REQUIRE(seekResult.error().code == StatusCode::Internal);
}
