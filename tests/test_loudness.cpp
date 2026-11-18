#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <kanariv/dsp/loudness.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

using namespace kanariv::dsp::loudness;

namespace {

std::vector<float> makeSine(double seconds, double sampleRate, unsigned channels, double freqHz,
                            double amplitude, double phase = 0.0) {
    const auto frames = static_cast<std::size_t>(std::llround(seconds * sampleRate));
    std::vector<float> out(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double value =
            amplitude *
            std::sin(2.0 * std::numbers::pi * freqHz * static_cast<double>(i) / sampleRate + phase);
        for (unsigned c = 0; c < channels; ++c)
            out[i * channels + c] = static_cast<float>(value);
    }
    return out;
}

std::vector<float> makeSilence(double seconds, double sampleRate, unsigned channels) {
    const auto frames = static_cast<std::size_t>(std::llround(seconds * sampleRate));
    return std::vector<float>(frames * channels, 0.0f);
}

std::unique_ptr<LoudnessMeter> openMeter(double sampleRate, unsigned channels) {
    auto meter = LoudnessMeter::create(sampleRate, channels);
    REQUIRE(meter.has_value());
    return std::move(*meter);
}

LoudnessResult measure(std::unique_ptr<LoudnessMeter>& meter, std::span<const float> frames) {
    auto added = meter->addFrames(frames);
    REQUIRE(added.has_value());
    return meter->result();
}

} // namespace

TEST_CASE("loudness stereo 1 kHz sine reads its level", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto meter = openMeter(rate, 2);
    auto frames = makeSine(5.0, rate, 2, 1000.0, 0.1);
    const auto result = measure(meter, frames);

    // 5 s = 50 hops, one block per hop after the first four.
    CHECK(meter->blockEnergies().size() == 47);
    // The K-weighting boost at 1 kHz (+0.69 dB) cancels the -0.691 offset, so a
    // stereo sine reads 20*log10(amplitude).
    CHECK(result.integratedLufs == Catch::Approx(-20.0).margin(0.05));
    CHECK(result.truePeakDbtp == Catch::Approx(-20.0).margin(0.05));
    CHECK(replayGainDb(result) == Catch::Approx(2.0).margin(0.05));
}

TEST_CASE("loudness mono sine is 3 LU below stereo", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto meter = openMeter(rate, 1);
    auto frames = makeSine(5.0, rate, 1, 1000.0, 0.1);
    const auto result = measure(meter, frames);

    CHECK(result.integratedLufs == Catch::Approx(-23.01).margin(0.05));
    CHECK(result.truePeakDbtp == Catch::Approx(-20.0).margin(0.05));
    CHECK(replayGainDb(result) == Catch::Approx(5.01).margin(0.05));
}

TEST_CASE("loudness silence measures nothing", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto meter = openMeter(rate, 2);
    auto frames = makeSilence(2.0, rate, 2);
    const auto result = measure(meter, frames);

    CHECK(std::isinf(result.integratedLufs));
    CHECK(result.integratedLufs < 0.0);
    CHECK(std::isinf(result.truePeakDbtp));
    CHECK(result.truePeakDbtp < 0.0);
    CHECK(meter->truePeakLinear() == 0.0);
    CHECK(replayGainDb(result) == 0.0);
}

TEST_CASE("loudness needs a complete 400 ms block", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto meter = openMeter(rate, 2);

    auto partial = makeSine(0.39, rate, 2, 1000.0, 0.5);
    const auto result = measure(meter, partial);
    CHECK(meter->blockEnergies().empty());
    CHECK(std::isinf(result.integratedLufs));
    CHECK(std::isfinite(result.truePeakDbtp));

    auto remainder = makeSine(0.01, rate, 2, 1000.0, 0.5);
    auto added = meter->addFrames(remainder);
    REQUIRE(added.has_value());
    CHECK(meter->blockEnergies().size() == 1);
    CHECK(std::isfinite(meter->result().integratedLufs));
}

TEST_CASE("loudness relative gate drops quiet blocks", "[dsp][loudness]") {
    // Block energy 0.01 reads -20.691 LUFS; 1e-6 reads -60.691 LUFS.
    std::vector<double> energies(100, 0.01);
    CHECK(integratedLoudness(energies) == Catch::Approx(-20.691).margin(0.001));

    energies.insert(energies.end(), 100, 1e-6);
    // An ungated mean over all 200 blocks would read about -23.7 LUFS; the
    // relative gate (10 LU below the absolute-gated mean) keeps the loud half
    // only.
    CHECK(integratedLoudness(energies) == Catch::Approx(-20.691).margin(0.001));
}

TEST_CASE("loudness absolute gate drops near-silence", "[dsp][loudness]") {
    // Block energy 1e-8 reads about -80.7 LUFS, below the -70 LUFS gate.
    std::vector<double> quiet(100, 1e-8);
    CHECK(std::isinf(integratedLoudness(quiet)));

    quiet.push_back(0.01);
    CHECK(integratedLoudness(quiet) == Catch::Approx(-20.691).margin(0.001));
}

TEST_CASE("loudness true peak sees between samples", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    // 12 kHz at 48 kHz is four samples per cycle: with a pi/4 phase every
    // sample lands on sin(+-pi/4) = +-0.7071 (-3.01 dBFS) while the waveform
    // itself reaches full scale between samples. A short fade-in keeps the
    // onset from adding its own interpolator ringing to the measurement.
    auto frames = makeSine(1.0, rate, 2, 12000.0, 1.0, std::numbers::pi / 4.0);
    const auto fadeFrames = static_cast<std::size_t>(0.005 * rate);
    for (std::size_t i = 0; i < fadeFrames; ++i) {
        const float ramp = 0.5f - 0.5f * std::cos(std::numbers::pi * static_cast<float>(i) /
                                                  static_cast<float>(fadeFrames));
        frames[i * 2] *= ramp;
        frames[i * 2 + 1] *= ramp;
    }

    float samplePeak = 0.0f;
    for (float sample : frames)
        samplePeak = std::max(samplePeak, std::abs(sample));
    CHECK(linearToDbtp(samplePeak) == Catch::Approx(-3.0103).margin(0.01));

    auto meter = openMeter(rate, 2);
    const auto result = measure(meter, frames);
    // The 4x grid lands exactly on the waveform peak, so the meter must recover
    // full scale from the -3 dBFS samples (BS.1770 allows +/-0.5 dB).
    CHECK(result.truePeakDbtp > -0.5);
    CHECK(result.truePeakDbtp <= 0.5);
}

TEST_CASE("loudness replay gain never pushes the true peak over full scale", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto frames = makeSilence(2.0, rate, 2);
    // One full-scale impulse in an otherwise silent track: loudness alone would
    // ask for a large boost, the true peak limits the gain to unity.
    const auto midFrame = static_cast<std::size_t>(rate);
    frames[midFrame * 2] = 1.0f;
    frames[midFrame * 2 + 1] = 1.0f;

    auto meter = openMeter(rate, 2);
    const auto result = measure(meter, frames);
    CHECK(result.integratedLufs < -30.0);
    CHECK(result.truePeakDbtp == Catch::Approx(0.0).margin(0.05));
    CHECK(replayGainDb(result) == 0.0);
}

TEST_CASE("loudness album merges block energies", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto loudFrames = makeSine(2.0, rate, 2, 1000.0, 0.1);    // about -20 LUFS
    auto quietFrames = makeSine(2.0, rate, 2, 1000.0, 0.001); // about -60 LUFS

    auto loudMeter = openMeter(rate, 2);
    auto quietMeter = openMeter(rate, 2);
    const auto loud = measure(loudMeter, loudFrames);
    const auto quiet = measure(quietMeter, quietFrames);
    // Each 2 s track covers 20 hops; its first block needs four of them.
    CHECK(loudMeter->blockEnergies().size() == 17);
    CHECK(quietMeter->blockEnergies().size() == 17);
    CHECK(quiet.integratedLufs == Catch::Approx(-60.0).margin(0.05));

    const auto loudEnergies = loudMeter->blockEnergies();
    const auto quietEnergies = quietMeter->blockEnergies();
    std::vector<double> merged(loudEnergies.begin(), loudEnergies.end());
    merged.insert(merged.end(), quietEnergies.begin(), quietEnergies.end());

    // The album's relative gate drops the quiet track's blocks, so the album
    // reads like the loud track instead of settling between the two.
    const double album = integratedLoudness(merged);
    CHECK(album == Catch::Approx(loud.integratedLufs).margin(0.05));
    CHECK(album > -22.0);
    CHECK(replayGainDb({.integratedLufs = album, .truePeakDbtp = loud.truePeakDbtp}) ==
          Catch::Approx(replayGainDb(loud)).margin(0.05));
}

TEST_CASE("loudness reset restarts measurement", "[dsp][loudness]") {
    constexpr double rate = 48000.0;
    auto frames = makeSine(1.0, rate, 2, 1000.0, 0.2);

    auto meter = openMeter(rate, 2);
    const auto first = measure(meter, frames);

    meter->reset();
    CHECK(meter->blockEnergies().empty());
    CHECK(meter->truePeakLinear() == 0.0);

    const auto second = measure(meter, frames);
    CHECK(second.integratedLufs == Catch::Approx(first.integratedLufs).margin(1e-12));
    CHECK(second.truePeakDbtp == Catch::Approx(first.truePeakDbtp).margin(1e-12));
}

TEST_CASE("loudness validates configuration and frames", "[dsp][loudness]") {
    using kanariv::utils::StatusCode;

    const auto lowRate = LoudnessMeter::create(4000.0, 2);
    REQUIRE(!lowRate.has_value());
    CHECK(lowRate.error().code == StatusCode::InvalidArg);
    CHECK(!LoudnessMeter::create(48000.0, 0).has_value());
    CHECK(!LoudnessMeter::create(48000.0, 9).has_value());
    CHECK(!LoudnessMeter::create(std::nan(""), 2).has_value());

    auto meter = openMeter(48000.0, 2);
    std::vector<float> oddFrame(3, 0.0f);
    const auto added = meter->addFrames(oddFrame);
    REQUIRE(!added.has_value());
    CHECK(added.error().code == StatusCode::InvalidArg);

    CHECK(meter->addFrames({}).has_value());
    CHECK(meter->blockEnergies().empty());
    CHECK(std::isinf(meter->result().integratedLufs));
}
