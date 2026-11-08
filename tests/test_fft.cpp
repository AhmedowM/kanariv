#include <catch2/catch_test_macros.hpp>
#include <kanariv/media/fft.hpp>
#include <kanariv/utils/error.hpp>
#include <common.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
#include <vector>

using namespace kanariv::media::fft;

namespace {

std::vector<float> makeSine(std::size_t n, double freqHz, double sampleRate) {
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * freqHz *
                                           static_cast<double>(i) / sampleRate));
    return s;
}

bool closeEnough(float a, float b, float tol = 1e-3f) {
    return std::abs(a - b) < tol;
}

} // namespace

TEST_CASE("fft R2C round trip via ifftReal", "[media][fft]") {
    constexpr std::size_t n = 1024;
    auto samples = makeSine(n, 440.0, 44100.0);

    auto bins = fftReal(samples);
    REQUIRE(bins.has_value());
    REQUIRE(bins->size() == n / 2 + 1);

    auto reconstructed = ifftReal(*bins);
    REQUIRE(reconstructed.has_value());
    REQUIRE(reconstructed->size() == n);

    // Round trip should recover the original within float error.
    for (std::size_t i = 0; i < n; ++i)
        CHECK(closeEnough(reconstructed->at(i), samples[i]));
}

TEST_CASE("fft R2C peak bin at sine frequency", "[media][fft]") {
    constexpr std::size_t n = 1024;
    constexpr double sampleRate = 44100.0;
    constexpr double freqHz = 440.0;
    // Bin spacing = sampleRate / n = 43.0664... Hz; 440 Hz lands near bin 10.
    auto samples = makeSine(n, freqHz, sampleRate);

    auto bins = fftReal(samples);
    REQUIRE(bins.has_value());

    // Find the magnitude peak (ignore DC).
    std::size_t peakBin = 1;
    float peakMag = 0.0f;
    for (std::size_t i = 1; i < bins->size(); ++i) {
        float mag = std::abs((*bins)[i]);
        if (mag > peakMag) {
            peakMag = mag;
            peakBin = i;
        }
    }

    // 440 Hz / 43.0664 Hz per bin = 10.216 -> nearest bin 10.
    CHECK(peakBin == 10);
}

TEST_CASE("fft FftPlan executeR2C matches fftReal", "[media][fft]") {
    constexpr std::size_t n = 512;
    auto samples = makeSine(n, 1000.0, 44100.0);

    auto plan = FftPlan::createR2C(n);
    REQUIRE(plan.has_value());
    REQUIRE((*plan)->length() == n);
    REQUIRE((*plan)->outputBins() == n / 2 + 1);

    std::vector<std::complex<float>> bins(n / 2 + 1);
    auto res = (*plan)->executeR2C(samples, bins);
    REQUIRE(res.has_value());

    auto oneShot = fftReal(samples);
    REQUIRE(oneShot.has_value());
    REQUIRE(oneShot->size() == bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i)
        CHECK(closeEnough(oneShot->at(i).real(), bins[i].real()));
    for (std::size_t i = 0; i < bins.size(); ++i)
        CHECK(closeEnough(oneShot->at(i).imag(), bins[i].imag()));
}

TEST_CASE("fft FftPlan executeC2C inverse round trip", "[media][fft]") {
    constexpr std::size_t n = 256;
    std::vector<std::complex<float>> input(n);
    for (std::size_t i = 0; i < n; ++i)
        input[i] = std::complex<float>{static_cast<float>(i % 17),
                                       static_cast<float>((i * 7) % 13)};

    auto plan = FftPlan::createC2C(n);
    REQUIRE(plan.has_value());

    std::vector<std::complex<float>> forward(n);
    auto fwd = (*plan)->executeC2C(input, forward, false);
    REQUIRE(fwd.has_value());

    std::vector<std::complex<float>> backward(n);
    auto inv = (*plan)->executeC2C(forward, backward, true);
    REQUIRE(inv.has_value());

    for (std::size_t i = 0; i < n; ++i)
        CHECK(closeEnough(backward[i].real(), input[i].real()));
    for (std::size_t i = 0; i < n; ++i)
        CHECK(closeEnough(backward[i].imag(), input[i].imag()));
}

TEST_CASE("fft window hann", "[media][fft]") {
    constexpr std::size_t n = 16;
    auto w = window::hann(n);
    REQUIRE(w.size() == n);

    // Endpoints are zero, peak is near one (exact peak falls between
    // samples for even-length symmetric windows).
    CHECK(w.front() == 0.0f);
    CHECK(w.back() == 0.0f);
    float wmax = 0.0f;
    for (float v : w)
        wmax = std::max(wmax, v);
    CHECK(std::abs(wmax - 1.0f) < 0.02f);

    // All values in [0, 1].
    for (float v : w) {
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("fft window hamming", "[media][fft]") {
    constexpr std::size_t n = 16;
    auto w = window::hamming(n);
    REQUIRE(w.size() == n);

    // Endpoints are 0.08, not zero.
    CHECK(std::abs(w.front() - 0.08f) < 1e-6f);
    CHECK(std::abs(w.back() - 0.08f) < 1e-6f);
}

TEST_CASE("fft window applyInPlace", "[media][fft]") {
    std::vector<float> samples(8, 1.0f);
    auto w = window::hann(8);

    auto res = window::applyInPlace(samples, w);
    REQUIRE(res.has_value());

    // First sample is 1.0 * 0 = 0.
    CHECK(samples.front() == 0.0f);
    // Others are scaled by the window.
    for (std::size_t i = 1; i < samples.size(); ++i)
        CHECK(std::abs(samples[i] - w[i]) < 1e-6f);
}

TEST_CASE("fft window applyInPlace size mismatch", "[media][fft]") {
    std::vector<float> samples(4, 1.0f);
    auto w = window::hann(8);
    auto res = window::applyInPlace(samples, w);
    REQUIRE(!res.has_value());
    REQUIRE(res.error().code == kanariv::utils::StatusCode::InvalidArg);
}

TEST_CASE("fft invalid length rejected", "[media][fft]") {
    auto plan = FftPlan::createR2C(3);
    REQUIRE(!plan.has_value());
    REQUIRE(plan.error().code == kanariv::utils::StatusCode::InvalidArg);

    plan = FftPlan::createR2C(1);
    REQUIRE(!plan.has_value());

    auto bad = fftReal(std::span<const float>{});
    REQUIRE(!bad.has_value());
}

TEST_CASE("fft ifftReal invalid bin count rejected", "[media][fft]") {
    // bins.size() must be n/2 + 1 where n is power of two.
    // 6 bins => n = 10, not power of two.
    std::vector<std::complex<float>> bins(6, std::complex<float>{1.0f, 0.0f});
    auto res = ifftReal(bins);
    REQUIRE(!res.has_value());
    REQUIRE(res.error().code == kanariv::utils::StatusCode::InvalidArg);
}
