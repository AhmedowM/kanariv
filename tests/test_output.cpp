#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <common.hpp>
#include <cstddef>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/ring.hpp>
#include <span>
#include <support/ring_fill.hpp>

TEST_CASE("output callback no alloc") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{8192, 2};
    auto out = kanariv::player::AudioOutput::create(
        {.sampleRate = 48000, .channels = 2, .ring = &ring, .volume = 1.0f});
    REQUIRE(out);
    std::array<float, 512> buf{};
    kanariv::player::detail::fillFromRing(buf, nullptr, 1, 1.0f);
    REQUIRE(buf[0] == 0.0f);
    REQUIRE(buf[511] == 0.0f);
}

TEST_CASE("output fillFromRing pattern * volume and tail zero", "[output]") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{8192, 2};
    // volume 0.5
    auto outRes = kanariv::player::AudioOutput::create(
        {.sampleRate = 48000, .channels = 2, .ring = &ring, .volume = 0.5f});
    REQUIRE(outRes.has_value());
    auto& out = *outRes.value();
    REQUIRE(out.volume() == 0.5f);
    // known pattern: 8 samples (4 frames stereo)
    std::array<float, 8> pattern{1.0f, -1.0f, 0.5f, -0.5f, 0.25f, -0.25f, 2.0f, -2.0f};
    // write pattern to ring (need to write frames: samples/channels = 4)
    size_t written = ring.write(std::span<const float>(pattern.data(), pattern.size()));
    REQUIRE(written == 4); // 4 frames

    std::array<float, 16> buf{};
    // fill with sentinel
    for (auto& v : buf)
        v = 99.0f;
    kanariv::player::detail::fillFromRing(buf, &ring, 2, out.volume());

    // first 8 samples should be pattern * 0.5
    for (size_t i = 0; i < pattern.size(); ++i) {
        float expected = pattern[i] * 0.5f;
        REQUIRE(std::abs(buf[i] - expected) < 1e-6f);
    }
    // tail (8 samples) should be zero-filled then multiplied by volume (still 0)
    for (size_t i = pattern.size(); i < buf.size(); ++i) {
        REQUIRE(buf[i] == 0.0f);
    }

    // second fill with empty ring should be all zeros
    std::array<float, 8> buf2{};
    for (auto& v : buf2)
        v = 5.0f;
    kanariv::player::detail::fillFromRing(buf2, &ring, 2, out.volume());
    for (auto v : buf2)
        REQUIRE(v == 0.0f);
}

TEST_CASE("output setVolume clamp", "[output]") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{1024, 1};
    auto outRes = kanariv::player::AudioOutput::create(
        {.sampleRate = 48000, .channels = 1, .ring = &ring, .volume = 1.0f});
    REQUIRE(outRes.has_value());
    auto& out = *outRes.value();
    out.setVolume(-1.0f);
    REQUIRE(out.volume() == 0.0f);
    out.setVolume(2.0f);
    REQUIRE(out.volume() == 1.0f);
    out.setVolume(0.7f);
    REQUIRE(std::abs(out.volume() - 0.7f) < 1e-6f);
    out.setVolume(-0.001f);
    REQUIRE(out.volume() == 0.0f);
    out.setVolume(1.001f);
    REQUIRE(out.volume() == 1.0f);
}

TEST_CASE("output fillFromRing volume 1.0 passthrough", "[output]") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{8192, 1};
    auto outRes = kanariv::player::AudioOutput::create(
        {.sampleRate = 48000, .channels = 1, .ring = &ring, .volume = 1.0f});
    REQUIRE(outRes.has_value());
    auto& out = *outRes.value();
    std::array<float, 4> pat{0.1f, 0.2f, 0.3f, 0.4f};
    ring.write(pat);
    std::array<float, 8> buf{};
    kanariv::player::detail::fillFromRing(buf, &ring, 1, out.volume());
    for (size_t i = 0; i < 4; ++i)
        REQUIRE(std::abs(buf[i] - pat[i]) < 1e-6f);
    for (size_t i = 4; i < 8; ++i)
        REQUIRE(buf[i] == 0.0f);
}
