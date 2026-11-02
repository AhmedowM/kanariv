#include <array>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
using namespace kanariv::media;
using namespace kanariv::utils;
using kanariv::media::Decoder;
using kanariv::media::FileReader;
using kanariv::media::MemoryReader;

namespace {

void requireDecodes(const std::filesystem::path& path) {
    auto r = FileReader::open(path);
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->sampleRate() > 0);
    REQUIRE((*dec)->channels() > 0);
    std::array<float, 1024> out{};
    REQUIRE((*dec)->decode(out) > 0);
}

void checkFixture(const char* name) {
    auto path = std::filesystem::path(TEST_DATA_DIR) / name;
    if (!std::filesystem::exists(path)) {
        SKIP("no fixture " + std::string(name));
    }
    requireDecodes(path);
}

} // namespace

TEST_CASE("ffmpeg whitelist decodes mp3", "[ffmpeg]") {
    checkFixture("sample.mp3");
}

TEST_CASE("ffmpeg whitelist decodes flac", "[ffmpeg]") {
    checkFixture("sample.flac");
}

TEST_CASE("ffmpeg whitelist decodes m4a", "[ffmpeg]") {
    checkFixture("sample.m4a");
}

TEST_CASE("ffmpeg whitelist decodes opus", "[ffmpeg]") {
    checkFixture("sample.opus");
}

TEST_CASE("ffmpeg whitelist decodes wma", "[ffmpeg]") {
    checkFixture("sample.wma");
}

TEST_CASE("ffmpeg whitelist decodes ogg", "[ffmpeg]") {
    checkFixture("sample.ogg");
}

TEST_CASE("ffmpeg whitelist decodes wav", "[ffmpeg]") {
    checkFixture("sample.wav");
}

TEST_CASE("ffmpeg whitelist decodes tta", "[ffmpeg]") {
    checkFixture("sample.tta");
}

TEST_CASE("ffmpeg probe returns true for any data when available", "[ffmpeg]") {
    // FFmpeg probe is permissive (returns true for any data >= 4 bytes)
    std::vector<std::byte> probeData(32, std::byte{0});
    probeData[0] = std::byte{'R'};
    probeData[1] = std::byte{'I'};
    probeData[2] = std::byte{'F'};
    probeData[3] = std::byte{'F'};

    auto r = MemoryReader::open(probeData);
    REQUIRE(r.has_value());

    // With fake data, FFmpeg probe returns true but create fails -> unsupported (FFmpeg always on)
    auto dec = Decoder::open(**r);
    REQUIRE(!dec.has_value());
    REQUIRE(dec.error().code == StatusCode::Unsupported);
}
