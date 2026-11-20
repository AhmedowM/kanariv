#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/ring.hpp>
#include <limits>
#include <span>
#include <string_view>
#include <support/ring_fill.hpp>
#include <vector>

#include "player/device_id.hpp"
#include "player/playback_policy.hpp"

TEST_CASE("output callback no alloc") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{8192, 2};
    auto out = kanariv::player::AudioOutput::create({48000, 2, &ring, 1.0f});
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
    auto outRes = kanariv::player::AudioOutput::create({48000, 2, &ring, 0.5f});
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
    auto outRes = kanariv::player::AudioOutput::create({48000, 1, &ring, 1.0f});
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

TEST_CASE("output gain is a separate factor from volume", "[output]") {
    using kanariv::player::AudioOutput;
    kanariv::utils::SpscRing<float> ring{1024, 2};
    AudioOutput::Config cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.ring = &ring;
    cfg.nullBackend = true;

    auto outRes = AudioOutput::create(cfg);
    REQUIRE(outRes.has_value());
    auto& out = *outRes.value();
    REQUIRE(out.gain() == 1.0f); // config default is neutral

    out.setGain(2.0f);
    REQUIRE(out.gain() == 2.0f);
    out.setVolume(0.25f);
    REQUIRE(out.volume() == 0.25f);
    REQUIRE(out.gain() == 2.0f); // a volume change never discards the gain
    out.setVolume(1.0f);
    REQUIRE(out.gain() == 2.0f);
}

TEST_CASE("output gain comes from config and is sanitized", "[output]") {
    using kanariv::player::AudioOutput;
    kanariv::utils::SpscRing<float> ring{1024, 2};
    AudioOutput::Config cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.ring = &ring;
    cfg.nullBackend = true;
    cfg.gain = 0.25f;

    auto outRes = AudioOutput::create(cfg);
    REQUIRE(outRes.has_value());
    auto& out = *outRes.value();
    REQUIRE(out.gain() == 0.25f);

    out.setGain(-1.0f);
    REQUIRE(out.gain() == 1.0f);
    out.setGain(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(out.gain() == 1.0f);
    out.setGain(std::numeric_limits<float>::infinity());
    REQUIRE(out.gain() == 1.0f);
    out.setGain(0.0f); // a zero gain is legal: silence for this track
    REQUIRE(out.gain() == 0.0f);
}

TEST_CASE("output fillFromRing volume 1.0 passthrough", "[output]") {
    KANARIV_SKIP_IF_NOAUDIO();
    kanariv::utils::SpscRing<float> ring{8192, 1};
    auto outRes = kanariv::player::AudioOutput::create({48000, 1, &ring, 1.0f});
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

TEST_CASE("output create rejects invalid config", "[output][validation]") {
    // Validation happens before any device is touched, so this runs headless
    // without KANARIV_SKIP_IF_NOAUDIO and without beeping.
    using kanariv::player::AudioOutput;
    using kanariv::utils::StatusCode;
    kanariv::utils::SpscRing<float> ring{1024, 2};

    SECTION("null ring") {
        auto res = AudioOutput::create({48000, 2, nullptr, 1.0f});
        REQUIRE(!res.has_value());
        REQUIRE(res.error().code == StatusCode::InvalidArg);
    }
    SECTION("zero sample rate") {
        auto res = AudioOutput::create({0, 2, &ring, 1.0f});
        REQUIRE(!res.has_value());
        REQUIRE(res.error().code == StatusCode::InvalidArg);
    }
    SECTION("zero channels") {
        auto res = AudioOutput::create({48000, 0, &ring, 1.0f});
        REQUIRE(!res.has_value());
        REQUIRE(res.error().code == StatusCode::InvalidArg);
    }
    SECTION("unsupported channel count") {
        kanariv::utils::SpscRing<float> wide{1024, 33};
        auto res = AudioOutput::create({48000, 33, &wide, 1.0f});
        REQUIRE(!res.has_value());
        REQUIRE(res.error().code == StatusCode::Unsupported);
    }
}

TEST_CASE("output rejects double init", "[output][validation]") {
    using kanariv::player::AudioOutput;
    using kanariv::utils::StatusCode;
    kanariv::utils::SpscRing<float> ring{1024, 2};
    AudioOutput::Config cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.ring = &ring;
    cfg.nullBackend = true;

    auto res = AudioOutput::create(cfg);
    REQUIRE(res.has_value());

    auto again = (*res)->init(cfg);
    REQUIRE(!again.has_value());
    REQUIRE(again.error().code == StatusCode::InvalidArg);
}

TEST_CASE("output null backend start stop lifecycle", "[output][null]") {
    using kanariv::player::AudioOutput;
    using kanariv::utils::StatusCode;
    kanariv::utils::SpscRing<float> ring{4096, 2};
    AudioOutput::Config cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.ring = &ring;
    cfg.nullBackend = true;

    auto res = AudioOutput::create(cfg);
    REQUIRE(res.has_value());
    auto& out = *res.value();

    REQUIRE(out.start().has_value());
    REQUIRE(out.start().has_value()); // idempotent
    REQUIRE(out.stop().has_value());
    REQUIRE(out.stop().has_value()); // idempotent

    out.shutdown();
    auto afterShutdown = out.start();
    REQUIRE(!afterShutdown.has_value());
    REQUIRE(afterShutdown.error().code == StatusCode::State);
}

TEST_CASE("output null backend drives the callback", "[output][null]") {
    using kanariv::player::AudioOutput;
    kanariv::utils::SpscRing<float> ring{4096, 2};
    AudioOutput::Config cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.ring = &ring;
    cfg.nullBackend = true;

    auto res = AudioOutput::create(cfg);
    REQUIRE(res.has_value());
    auto& out = *res.value();

    std::vector<float> frames(2 * 1000, 0.25f);
    REQUIRE(ring.write(std::span<const float>(frames.data(), frames.size())) == 1000);
    REQUIRE(ring.availableRead() == 1000);

    REQUIRE(out.start().has_value());
    kanariv::test_helpers::busyWaitUntil([&] { return ring.availableRead() == 0; },
                                         std::chrono::milliseconds(2000),
                                         std::chrono::milliseconds(5));
    REQUIRE(ring.availableRead() == 0);
    REQUIRE(out.stop().has_value());
}

TEST_CASE("device ids are opaque backend-qualified hex", "[output][devices]") {
    using kanariv::player::detail::deviceIdBackend;
    using kanariv::player::detail::formatDeviceId;
    const std::array<std::uint8_t, 3> raw{0x68, 0x77, 0x3a};
    REQUIRE(formatDeviceId("alsa", raw) == "alsa:68773a");
    REQUIRE(formatDeviceId("alsa", {}) == "alsa:");
    REQUIRE(deviceIdBackend("alsa:68773a") == std::string_view{"alsa"});
    REQUIRE(deviceIdBackend("alsa:") == std::string_view{"alsa"});
    REQUIRE_FALSE(deviceIdBackend("auto"));
    REQUIRE_FALSE(deviceIdBackend("2"));
    REQUIRE_FALSE(deviceIdBackend("Built-in Audio"));
    REQUIRE_FALSE(deviceIdBackend(":00"));
    REQUIRE_FALSE(deviceIdBackend("alsa:6"));  // odd payload
    REQUIRE_FALSE(deviceIdBackend("alsa:zz")); // non-hex
    REQUIRE_FALSE(deviceIdBackend("alsa:AB")); // the encoding is lowercase
}

TEST_CASE("legacy device indices parse decimal digits only", "[output][devices]") {
    using kanariv::player::legacyDeviceIndex;
    REQUIRE(legacyDeviceIndex("0") == 0u);
    REQUIRE(legacyDeviceIndex("12") == 12u);
    REQUIRE(legacyDeviceIndex("4294967295") == 4294967295u);
    REQUIRE_FALSE(legacyDeviceIndex(""));
    REQUIRE_FALSE(legacyDeviceIndex("4294967296")); // exceeds uint32
    REQUIRE_FALSE(legacyDeviceIndex("99999999999")); // longer than any index
    REQUIRE_FALSE(legacyDeviceIndex("-1"));
    REQUIRE_FALSE(legacyDeviceIndex(" 1"));
    REQUIRE_FALSE(legacyDeviceIndex("1a"));
    REQUIRE_FALSE(legacyDeviceIndex("auto"));
    REQUIRE_FALSE(legacyDeviceIndex("alsa:00"));
}

TEST_CASE("playback policy keeps the two ring capacities distinct", "[player][policy]") {
    using namespace kanariv::playback::policy;
    STATIC_REQUIRE(playerRingFrames == 8192);
    STATIC_REQUIRE(engineRingFrames(1) == 8192);
    STATIC_REQUIRE(engineRingFrames(2) == 16384);
    STATIC_REQUIRE(engineRingFrames(6) == 49152);
    // Both paths buffer half their ring before the device starts.
    STATIC_REQUIRE(prerollFrames(playerRingFrames) == 4096);
    STATIC_REQUIRE(prerollFrames(engineRingFrames(2)) == 8192);
    STATIC_REQUIRE(prerollFrames(1) == 0);
}

TEST_CASE("playback policy bounds decode chunks by frames and samples", "[player][policy]") {
    using namespace kanariv::playback::policy;
    STATIC_REQUIRE(chunkCapFrames(0) == 1);
    STATIC_REQUIRE(chunkCapFrames(1) == 1024);
    STATIC_REQUIRE(chunkCapFrames(2) == 1024);
    STATIC_REQUIRE(chunkCapFrames(4) == 512);
    STATIC_REQUIRE(chunkCapFrames(6) == 341);
    STATIC_REQUIRE(chunkCapFrames(8) == 256);
    STATIC_REQUIRE(chunkCapFrames(4096) == 1);
    for (std::uint32_t ch = 1; ch <= 8; ++ch)
        REQUIRE(chunkCapFrames(ch) * ch <= maxChunkSamples);

    STATIC_REQUIRE(chunkFrames(2, 0) == 0); // full ring: the caller idles
    STATIC_REQUIRE(chunkFrames(0, 512) == 0);
    STATIC_REQUIRE(chunkFrames(2, 3) == 1); // less than one frame fits
    STATIC_REQUIRE(chunkFrames(2, 100) == 50);
    STATIC_REQUIRE(chunkFrames(2, 4096) == 1024);
    STATIC_REQUIRE(chunkFrames(6, 3) == 1);
    STATIC_REQUIRE(chunkFrames(6, 3000) == 341);
}

TEST_CASE("playback policy classifies decode outcomes", "[player][policy]") {
    using kanariv::media::DecodeStatus;
    using kanariv::playback::policy::classifyDecode;
    using kanariv::playback::policy::DecodeOutcome;
    REQUIRE(classifyDecode(DecodeStatus::Ok, 512) == DecodeOutcome::Continue);
    REQUIRE(classifyDecode(DecodeStatus::Ok, 0) == DecodeOutcome::Continue);
    REQUIRE(classifyDecode(DecodeStatus::Eof, 512) == DecodeOutcome::Continue);
    REQUIRE(classifyDecode(DecodeStatus::Eof, 0) == DecodeOutcome::EndOfStream);
    REQUIRE(classifyDecode(DecodeStatus::Error, 0) == DecodeOutcome::Failed);
    REQUIRE(classifyDecode(DecodeStatus::Error, 512) == DecodeOutcome::Failed);
}
