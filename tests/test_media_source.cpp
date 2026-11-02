#include <array>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/media/source.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace kanariv::media;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

namespace {

std::filesystem::path writeTempFile(const std::string& name, const std::vector<std::byte>& data) {
    auto dir = tempDirPath("kanariv_source_tests");
    auto path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();
    return path;
}

int openReadOnly(const std::filesystem::path& path) {
#if defined(_WIN32)
    return ::_open(path.string().c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}

int closeRaw(int fd) noexcept {
#if defined(_WIN32)
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

std::vector<std::byte> sampleBytes() {
    std::vector<std::byte> data;
    for (int i = 0; i < 64; ++i)
        data.push_back(static_cast<std::byte>(i));
    return data;
}

} // namespace

TEST_CASE("PathSource opens a path repeatedly", "[media][source]") {
    const auto path = writeTempFile("repeat.wav", sampleBytes());
    PathSource source(path);

    REQUIRE(source.kind() == SourceKind::path);
    REQUIRE(source.seekable());
    REQUIRE(source.displayName() == path.string());
    REQUIRE(source.path() == path);

    auto first = source.open();
    REQUIRE(first.has_value());
    std::array<std::byte, 4> out{};
    REQUIRE((*first)->read(out) == 4);
    REQUIRE((*first)->tell() == 4);
    REQUIRE(static_cast<unsigned char>(out[3]) == 3);

    // Path sources may be opened any number of times.
    auto second = source.open();
    REQUIRE(second.has_value());
    REQUIRE((*second)->tell() == 0);
    REQUIRE((*second)->size() == static_cast<int64_t>(sampleBytes().size()));
}

TEST_CASE("DescriptorSource rejects negative descriptor", "[media][source]") {
    DescriptorSource source(FileDescriptor{-1, true, false, true}, "bad");
    REQUIRE(source.kind() == SourceKind::descriptor);
    REQUIRE_FALSE(source.seekable());
    auto reader = source.open();
    REQUIRE(!reader.has_value());
    REQUIRE(reader.error().code == StatusCode::InvalidArg);
}

TEST_CASE("DescriptorSource rejects unreadable descriptor", "[media][source]") {
    const auto path = writeTempFile("unreadable.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);
    // readable=false: open() must fail even though the underlying fd is valid.
    DescriptorSource source(FileDescriptor{fd, false, true, true}, "unreadable");
    auto reader = source.open();
    REQUIRE(!reader.has_value());
    REQUIRE(reader.error().code == StatusCode::InvalidArg);
    // Seekability flag is independent of readability.
    REQUIRE(source.seekable());
    // closeOnDestroy=true: the source destructor closes fd; do not close again.
}

TEST_CASE("DescriptorSource single-use transfers ownership", "[media][source]") {
    const auto path = writeTempFile("owned.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    DescriptorSource source(FileDescriptor{fd, true, true, true}, "owned fixture");
    REQUIRE(source.kind() == SourceKind::descriptor);
    REQUIRE(source.seekable());
    REQUIRE(source.displayName() == "owned fixture");
    REQUIRE_FALSE(source.consumed());

    auto reader = source.open();
    REQUIRE(reader.has_value());
    REQUIRE(source.consumed());

    // Second open must fail: ownership moved to the first reader.
    auto again = source.open();
    REQUIRE(!again.has_value());
    REQUIRE(again.error().code == StatusCode::State);

    // Read from the transferred reader.
    std::array<std::byte, 4> out{};
    REQUIRE((*reader)->read(out) == 4);
    REQUIRE((*reader)->tell() == 4);
    REQUIRE(static_cast<unsigned char>(out[3]) == 3);
    REQUIRE((*reader)->seekable());
}

TEST_CASE("DescriptorSource non-seekable flags and read", "[media][source]") {
    const auto path = writeTempFile("stream.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    DescriptorSource source(FileDescriptor{fd, true, false, true}, "stream");
    REQUIRE_FALSE(source.seekable());

    auto reader = source.open();
    REQUIRE(reader.has_value());
    std::array<std::byte, 8> out{};
    REQUIRE((*reader)->read(out) == 8);
    REQUIRE((*reader)->tell() == 8);
    REQUIRE_FALSE((*reader)->seekable());
    // seek on a non-seekable reader returns Unsupported.
    REQUIRE(!(*reader)->seek(0, SEEK_SET).has_value());
    REQUIRE((*reader)->seek(0, SEEK_SET).error().code == StatusCode::Unsupported);
}

TEST_CASE("Borrowed descriptor not closed by source destructor", "[media][source]") {
    const auto path = writeTempFile("borrowed.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    // closeOnDestroy = false: caller retains ownership.
    DescriptorSource source(FileDescriptor{fd, true, true, false}, "borrowed");
    REQUIRE_FALSE(source.consumed());
    auto borrowedResult = source.open();
    REQUIRE(borrowedResult.has_value());
    REQUIRE(source.consumed());
    // Destructor of a borrowed-descriptor source must not close fd.
    {
        DescriptorSource tmp(FileDescriptor{fd, true, true, false}, "borrowed tmp");
    }
    // The fd remains valid for the caller to close.
    REQUIRE(closeRaw(fd) == 0);
}

TEST_CASE("DescriptorReader cached size and position", "[media][source]") {
    const auto path = writeTempFile("sized.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    FileDescriptor desc{fd, true, true, true};
    auto readerRes = DescriptorReader::open(desc);
    REQUIRE(readerRes.has_value());
    auto& reader = **readerRes;
    REQUIRE(reader.seekable());
    REQUIRE(reader.size() == static_cast<int64_t>(sampleBytes().size()));
    REQUIRE(reader.tell() == 0);
    REQUIRE((*readerRes)->seek(16, SEEK_SET).has_value());
    REQUIRE(reader.tell() == 16);
    REQUIRE((*readerRes)->seek(-8, SEEK_CUR).has_value());
    REQUIRE(reader.tell() == 8);
}

TEST_CASE("DescriptorReader rejects out-of-range seek", "[media][source]") {
    const auto path = writeTempFile("range.wav", sampleBytes());
    const int fd = openReadOnly(path);
    REQUIRE(fd >= 0);

    auto readerRes = DescriptorReader::open(FileDescriptor{fd, true, true, true});
    REQUIRE(readerRes.has_value());
    REQUIRE(!(*readerRes)->seek(-1, SEEK_SET).has_value());
    REQUIRE((*readerRes)->seek(-1, SEEK_SET).error().code == StatusCode::InvalidArg);
    REQUIRE((*readerRes)->seek(sampleBytes().size() + 1, SEEK_SET).error().code ==
            StatusCode::InvalidArg);
    REQUIRE(!(*readerRes)->seek(0, 999).has_value());
    REQUIRE((*readerRes)->seek(0, 999).error().code == StatusCode::InvalidArg);
}
