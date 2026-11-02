#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>
using namespace kanariv::media;
using namespace kanariv::utils;
using kanariv::media::FileReader;
using kanariv::media::MemoryReader;

// Helper to create a temp file with RIFF header
static std::filesystem::path makeTempFile(const std::string& name, std::vector<std::byte> data) {
    // Use test-specific subdir to avoid parallel test race on shared directory
    static std::atomic<int> testId{0};
    auto dir = std::filesystem::temp_directory_path() / "kanariv_reader_tests" /
               std::to_string(testId.fetch_add(1));
    std::filesystem::create_directories(dir);
    auto p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();
    return p;
}

TEST_CASE("FileReader open not found", "[reader]") {
    auto r = FileReader::open("no_such_file_xyz_12345.wav");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::NotFound);
}

TEST_CASE("FileReader open empty path", "[reader]") {
    auto r = FileReader::open("");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::InvalidArg);
}

TEST_CASE("reader 64-bit seek clamp", "[reader]") {
    // create a small file with known content
    std::vector<std::byte> data(64, std::byte{0});
    auto path = makeTempFile("seek_clamp.wav", data);
    {
        auto r = FileReader::open(path);
        REQUIRE(r.has_value());
        auto& reader = **r;
        // negative seek from start should fail InvalidArg
        auto bad = reader.seek(-1, SEEK_SET);
        REQUIRE(!bad.has_value());
        REQUIRE(bad.error().code == StatusCode::InvalidArg);
        // seek beyond size should fail
        auto beyond = reader.seek(1000, SEEK_SET);
        REQUIRE(!beyond.has_value());
        REQUIRE(beyond.error().code == StatusCode::InvalidArg);
        // bad whence
        auto badWhence = reader.seek(0, 999);
        REQUIRE(!badWhence.has_value());
        REQUIRE(badWhence.error().code == StatusCode::InvalidArg);
        // valid seek
        auto ok = reader.seek(10, SEEK_SET);
        REQUIRE(ok.has_value());
        REQUIRE(reader.tell() == 10);
        // size should be 64 via save/restore
        REQUIRE(reader.size() == 64);
        // tell after seek
        REQUIRE(reader.tell() == 10);
    }
    std::filesystem::remove(path);
}

TEST_CASE("FileReader size save restore", "[reader]") {
    std::vector<std::byte> data(128, std::byte{0xAA});
    auto path = makeTempFile("size_save_restore.wav", data);
    {
        auto r = FileReader::open(path);
        REQUIRE(r.has_value());
        auto& reader = **r;
        REQUIRE(reader.seek(20, SEEK_SET).has_value());
        int64_t sz = reader.size();
        REQUIRE(sz == 128);
        // after size(), position should be restored to 20
        REQUIRE(reader.tell() == 20);
        // read some bytes
        std::array<std::byte, 4> out{};
        std::size_t n = reader.read(out);
        REQUIRE(n == 4);
        REQUIRE(reader.tell() == 24);
    }
    std::filesystem::remove(path);
}

TEST_CASE("MemoryReader seek clamp and read", "[reader]") {
    std::vector<std::byte> src(32, std::byte{0x11});
    auto r = MemoryReader::open(src);
    REQUIRE(r.has_value());
    auto& reader = **r;
    REQUIRE(reader.size() == 32);
    REQUIRE(reader.tell() == 0);
    // seek negative
    REQUIRE(!reader.seek(-1, SEEK_SET).has_value());
    // seek beyond
    REQUIRE(!reader.seek(100, SEEK_SET).has_value());
    // bad whence
    REQUIRE(!reader.seek(0, 12345).has_value());
    // valid
    REQUIRE(reader.seek(10, SEEK_SET).has_value());
    REQUIRE(reader.tell() == 10);
    // SEEK_CUR
    REQUIRE(reader.seek(5, SEEK_CUR).has_value());
    REQUIRE(reader.tell() == 15);
    // SEEK_END
    REQUIRE(reader.seek(-5, SEEK_END).has_value());
    REQUIRE(reader.tell() == 27);
    // read
    std::array<std::byte, 8> buf{};
    std::size_t got = reader.read(buf);
    REQUIRE(got == 5); // only 5 left (32-27)
    REQUIRE(reader.tell() == 32);
    // read beyond EOF returns 0
    std::array<std::byte, 4> buf2{};
    REQUIRE(reader.read(buf2) == 0);
    // clamp still fails
    REQUIRE(!reader.seek(1, SEEK_CUR).has_value());
}

TEST_CASE("MemoryReader read write boundary", "[reader]") {
    std::array<std::byte, 4> srcBytes{std::byte{'R'}, std::byte{'I'}, std::byte{'F'},
                                      std::byte{'F'}};
    auto r = MemoryReader::open(std::span<const std::byte>(srcBytes.data(), srcBytes.size()));
    REQUIRE(r.has_value());
    auto& reader = **r;
    std::array<std::byte, 2> out{};
    REQUIRE(reader.read(out) == 2);
    REQUIRE(out[0] == std::byte{'R'});
    REQUIRE(reader.tell() == 2);
}

TEST_CASE("FileReader tell and read", "[reader]") {
    std::vector<std::byte> data;
    for (int i = 0; i < 16; ++i)
        data.push_back(static_cast<std::byte>(i));
    auto path = makeTempFile("tell_read.wav", data);
    {
        auto r = FileReader::open(path);
        REQUIRE(r.has_value());
        auto& reader = **r;
        REQUIRE(reader.tell() == 0);
        std::array<std::byte, 4> buf{};
        REQUIRE(reader.read(buf) == 4);
        REQUIRE(reader.tell() == 4);
        REQUIRE(static_cast<unsigned char>(buf[0]) == 0);
        REQUIRE(static_cast<unsigned char>(buf[3]) == 3);
    }
    std::filesystem::remove(path);
}
