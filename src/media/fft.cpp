#include <kanariv/media/fft.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace kanariv::media::fft {

namespace {

bool isPowerOfTwo(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

int log2OfPowerOfTwo(std::size_t n) noexcept {
    int bits = 0;
    while (n > 1) {
        n >>= 1;
        ++bits;
    }
    return bits;
}

// Bit-reverse the low `bits` bits of `idx`.
std::size_t bitReverse(std::size_t idx, int bits) noexcept {
    std::size_t rev = 0;
    for (int i = 0; i < bits; ++i) {
        rev = (rev << 1) | (idx & 1u);
        idx >>= 1;
    }
    return rev;
}

// Iterative radix-2 decimation-in-time Cooley-Tukey.
// `z` holds `n` complex samples in normal order; on return it holds the
// transform in normal order. `inverse` applies a negative exponent and the
// caller is responsible for the 1/n scaling.
void fftRadix2(std::vector<std::complex<float>>& z, bool inverse) noexcept {
    const std::size_t n = z.size();
    const int bits = log2OfPowerOfTwo(n);

    // Bit-reversal permutation.
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = bitReverse(i, bits);
        if (i < j)
            std::swap(z[i], z[j]);
    }

    // Butterflies.
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float sign = inverse ? 1.0f : -1.0f;
        const float ang = sign * 2.0f * static_cast<float>(std::numbers::pi) /
                          static_cast<float>(len);
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f);
            const std::size_t half = len / 2;
            for (std::size_t j = 0; j < half; ++j) {
                std::complex<float> u = z[i + j];
                std::complex<float> v = z[i + j + half] * w;
                z[i + j] = u + v;
                z[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace

struct FftPlan::Impl {
    std::size_t length{0};
    std::size_t outputBins{0};
    bool r2c{false};
};

FftPlan::FftPlan(FftPlan&& other) noexcept
    : impl_(std::move(other.impl_)) {}

FftPlan& FftPlan::operator=(FftPlan&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

FftPlan::~FftPlan() = default;

FftPlan::FftPlan(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<FftPlan>, kanariv::utils::Error>
FftPlan::createR2C(std::size_t n) {
    if (!isPowerOfTwo(n) || n < 2)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "fft length must be a power of two >= 2")};

    auto impl = std::make_unique<Impl>();
    impl->length = n;
    impl->outputBins = n / 2 + 1;
    impl->r2c = true;
    return std::unique_ptr<FftPlan>(new FftPlan(std::move(impl)));
}

std::expected<std::unique_ptr<FftPlan>, kanariv::utils::Error>
FftPlan::createC2C(std::size_t n) {
    if (!isPowerOfTwo(n) || n < 2)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "fft length must be a power of two >= 2")};

    auto impl = std::make_unique<Impl>();
    impl->length = n;
    impl->outputBins = n;
    impl->r2c = false;
    return std::unique_ptr<FftPlan>(new FftPlan(std::move(impl)));
}

std::expected<void, kanariv::utils::Error>
FftPlan::executeR2C(std::span<const float> input,
                    std::span<std::complex<float>> output) const {
    if (!impl_)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "fft plan is empty")};
    if (input.size() != impl_->length || output.size() != impl_->outputBins)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "span size mismatch")};

    // Pack real input into a full complex array (imag = 0), run a complex
    // FFT, and keep the first n/2 + 1 bins (DC..Nyquist).
    std::vector<std::complex<float>> z(impl_->length);
    for (std::size_t i = 0; i < input.size(); ++i)
        z[i] = std::complex<float>{input[i], 0.0f};

    fftRadix2(z, false);

    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = z[i];
    return {};
}

std::expected<void, kanariv::utils::Error>
FftPlan::executeC2C(std::span<const std::complex<float>> input,
                    std::span<std::complex<float>> output,
                    bool inverse) const {
    if (!impl_)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "fft plan is empty")};
    if (input.size() != impl_->length || output.size() != impl_->length)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "span size mismatch")};

    std::vector<std::complex<float>> z(input.begin(), input.end());

    fftRadix2(z, inverse);

    if (inverse) {
        const float scale = 1.0f / static_cast<float>(impl_->length);
        for (auto& c : z)
            c *= scale;
    }

    std::copy(z.begin(), z.end(), output.begin());
    return {};
}

std::size_t FftPlan::length() const noexcept {
    return impl_ ? impl_->length : 0;
}

std::size_t FftPlan::outputBins() const noexcept {
    return impl_ ? impl_->outputBins : 0;
}

std::vector<float> window::hann(std::size_t n) {
    std::vector<float> w(n);
    if (n == 0)
        return w;
    if (n == 1) {
        w[0] = 1.0f;
        return w;
    }
    const float denom = static_cast<float>(n - 1);
    for (std::size_t i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(std::numbers::pi) *
                                        static_cast<float>(i) / denom));
    return w;
}

std::vector<float> window::hamming(std::size_t n) {
    std::vector<float> w(n);
    if (n == 0)
        return w;
    if (n == 1) {
        w[0] = 1.0f;
        return w;
    }
    const float denom = static_cast<float>(n - 1);
    for (std::size_t i = 0; i < n; ++i)
        w[i] = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(std::numbers::pi) *
                                         static_cast<float>(i) / denom);
    return w;
}

std::expected<void, kanariv::utils::Error>
window::applyInPlace(std::span<float> samples, std::span<const float> window) {
    if (samples.size() != window.size())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "window length must match sample length")};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] *= window[i];
    return {};
}

std::expected<std::vector<std::complex<float>>, kanariv::utils::Error>
fftReal(std::span<const float> samples) {
    if (samples.empty())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "samples must be non-empty")};

    auto plan = FftPlan::createR2C(samples.size());
    if (!plan)
        return std::unexpected{plan.error()};

    std::vector<std::complex<float>> bins((*plan)->outputBins());
    auto res = (*plan)->executeR2C(samples, bins);
    if (!res)
        return std::unexpected{res.error()};
    return bins;
}

std::expected<std::vector<float>, kanariv::utils::Error>
ifftReal(std::span<const std::complex<float>> bins) {
    if (bins.empty())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "bins must be non-empty")};

    // bins count = n/2 + 1  =>  n = 2 * (bins.size() - 1)
    const std::size_t half = bins.size() - 1;
    const std::size_t n = 2 * half;
    if (!isPowerOfTwo(n))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "bin count must be n/2 + 1 for a power-of-two n")};

    auto plan = FftPlan::createC2C(n);
    if (!plan)
        return std::unexpected{plan.error()};

    // Reconstruct the full conjugate-symmetric spectrum:
    // bins[0..n/2] as given, then mirror bins[n-1] = conj(bins[1]), ...,
    // bins[n/2+1] = conj(bins[n/2 - 1]).
    std::vector<std::complex<float>> spectrum(n);
    for (std::size_t i = 0; i <= half; ++i)
        spectrum[i] = bins[i];
    for (std::size_t i = half + 1; i < n; ++i)
        spectrum[i] = std::conj(bins[n - i]);

    std::vector<std::complex<float>> out(n);
    auto res = (*plan)->executeC2C(spectrum, out, true);
    if (!res)
        return std::unexpected{res.error()};

    std::vector<float> real(n);
    for (std::size_t i = 0; i < n; ++i)
        real[i] = out[i].real();
    return real;
}

} // namespace kanariv::media::fft
