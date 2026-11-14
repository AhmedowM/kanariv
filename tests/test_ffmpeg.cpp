#include <array>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

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

int openReadOnly(const std::filesystem::path& path) {
#if defined(_WIN32)
    return ::_wopen(path.wstring().c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
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

TEST_CASE("ffmpeg decodes a non-seekable descriptor from byte zero", "[ffmpeg]") {
    const auto path = std::filesystem::path(TEST_DATA_DIR) / "sample.ogg";
    if (!std::filesystem::exists(path)) {
        SKIP("no fixture sample.ogg");
    }
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    auto reader = DescriptorReader::open(FileDescriptor{fd, true, false, true});
    REQUIRE(reader.has_value());
    REQUIRE_FALSE((*reader)->seekable());
    REQUIRE((*reader)->tell() == 0);

    // M3: the old 32-byte read-ahead probe consumed the Ogg header of a
    // non-seekable source, so init() opened the stream at offset 32.
    auto dec = Decoder::open(**reader);
    REQUIRE(dec.has_value());
    std::array<float, 4096> out{};
    REQUIRE((*dec)->decode(out) > 0);
}

TEST_CASE("extractMetadata accepts a non-NUL-terminated path view", "[ffmpeg]") {
    const auto path = std::filesystem::path(TEST_DATA_DIR) / "sample.mp3";
    if (!std::filesystem::exists(path)) {
        SKIP("no fixture sample.mp3");
    }
    const std::string raw = path.string();
    // The view ends before the string's terminator: passing data() straight to
    // FFmpeg (M5) would have read the trailing garbage as part of the path.
    const std::string padded = raw + "garbage";
    const std::string_view view(padded.data(), raw.size());
    auto meta = extractMetadata(view);
    REQUIRE(meta.has_value());
    REQUIRE(meta->sample_rate > 0);
}
