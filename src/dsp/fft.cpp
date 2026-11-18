#include <kiss_fft.h>
#include <kiss_fftr.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <expected>
#include <kanariv/dsp/fft.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace kanariv::dsp::fft {

namespace {

bool isPowerOfTwo(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// KissFFT sizes are ints; a power-of-two length that does not fit is rejected
// rather than narrowed.
bool fitsKissFftLength(std::size_t n) noexcept {
    return n <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

} // namespace

struct FftPlan::Impl {
    std::size_t length{0};
    std::size_t outputBins{0};
    kiss_fftr_cfg realForward{nullptr};
    kiss_fft_cfg complexForward{nullptr};
    kiss_fft_cfg complexInverse{nullptr};
    // Reused across executions and resized only when the length changes, so a
    // steady stream of equal-length blocks allocates nothing. Two buffers keep
    // the in-place path out of kiss_fft (which would allocate a temporary per
    // call). `mutable` because execute* is const; sharing them is exactly why a
    // plan is not reentrant (documented in fft.hpp).
    mutable std::vector<kiss_fft_cpx> input;
    mutable std::vector<kiss_fft_cpx> result;

    ~Impl() {
        if (realForward)
            kiss_fftr_free(realForward);
        if (complexForward)
            kiss_fft_free(complexForward);
        if (complexInverse)
            kiss_fft_free(complexInverse);
    }
};

FftPlan::FftPlan() noexcept = default;

FftPlan::FftPlan(FftPlan&& other) noexcept : impl_(std::move(other.impl_)) {}

FftPlan& FftPlan::operator=(FftPlan&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

FftPlan::~FftPlan() = default;

FftPlan::FftPlan(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

kanariv::utils::Expected<std::unique_ptr<FftPlan>> FftPlan::createR2C(std::size_t n) {
    if (!isPowerOfTwo(n) || n < 2)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "fft length must be a power of two >= 2")};
    if (!fitsKissFftLength(n))
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "fft length exceeds the plan limit")};

    auto impl = std::make_unique<Impl>();
    impl->length = n;
    impl->outputBins = n / 2 + 1;
    impl->realForward = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
    if (!impl->realForward)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "fft plan allocation failed")};
    return std::unique_ptr<FftPlan>(new FftPlan(std::move(impl)));
}

kanariv::utils::Expected<std::unique_ptr<FftPlan>> FftPlan::createC2C(std::size_t n) {
    if (!isPowerOfTwo(n) || n < 2)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "fft length must be a power of two >= 2")};
    if (!fitsKissFftLength(n))
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "fft length exceeds the plan limit")};

    auto impl = std::make_unique<Impl>();
    impl->length = n;
    impl->outputBins = n;
    impl->complexForward = kiss_fft_alloc(static_cast<int>(n), 0, nullptr, nullptr);
    impl->complexInverse = kiss_fft_alloc(static_cast<int>(n), 1, nullptr, nullptr);
    if (!impl->complexForward || !impl->complexInverse)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "fft plan allocation failed")};
    return std::unique_ptr<FftPlan>(new FftPlan(std::move(impl)));
}

kanariv::utils::Expected<void> FftPlan::executeR2C(std::span<const float> inputSpan,
                                                   std::span<std::complex<float>> output) const {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "fft plan is empty")};
    if (!impl_->realForward)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "fft plan was not created for real input")};
    if (inputSpan.size() != impl_->length || output.size() != impl_->outputBins)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "span size mismatch")};

    auto& result = impl_->result;
    if (result.size() != impl_->outputBins)
        result.resize(impl_->outputBins);

    kiss_fftr(impl_->realForward, inputSpan.data(), result.data());

    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = std::complex<float>{result[i].r, result[i].i};
    return {};
}

kanariv::utils::Expected<void> FftPlan::executeC2C(std::span<const std::complex<float>> inputSpan,
                                                   std::span<std::complex<float>> output,
                                                   bool inverse) const {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "fft plan is empty")};
    if (!impl_->complexForward)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "fft plan was not created for complex input")};
    if (inputSpan.size() != impl_->length || output.size() != impl_->length)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "span size mismatch")};

    auto& scratch = impl_->input;
    auto& result = impl_->result;
    if (scratch.size() != impl_->length)
        scratch.resize(impl_->length);
    if (result.size() != impl_->length)
        result.resize(impl_->length);

    for (std::size_t i = 0; i < inputSpan.size(); ++i) {
        scratch[i].r = inputSpan[i].real();
        scratch[i].i = inputSpan[i].imag();
    }

    kiss_fft(inverse ? impl_->complexInverse : impl_->complexForward, scratch.data(),
             result.data());

    // KissFFT leaves the inverse unnormalized; both directions keep the same
    // scale contract the plan documented before the kernel swap.
    const float scale = inverse ? 1.0f / static_cast<float>(impl_->length) : 1.0f;
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = std::complex<float>{result[i].r * scale, result[i].i * scale};
    }
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

kanariv::utils::Expected<void> window::applyInPlace(std::span<float> samples,
                                                    std::span<const float> window) {
    if (samples.size() != window.size())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "window length must match sample length")};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] *= window[i];
    return {};
}

kanariv::utils::Expected<std::vector<std::complex<float>>> fftReal(std::span<const float> samples) {
    if (samples.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "samples must be non-empty")};

    auto plan = FftPlan::createR2C(samples.size());
    if (!plan)
        return std::unexpected{plan.error()};

    std::vector<std::complex<float>> bins((*plan)->outputBins());
    auto res = (*plan)->executeR2C(samples, bins);
    if (!res)
        return std::unexpected{res.error()};
    return bins;
}

kanariv::utils::Expected<std::vector<float>> ifftReal(std::span<const std::complex<float>> bins) {
    if (bins.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "bins must be non-empty")};

    // bins count = n/2 + 1  =>  n = 2 * (bins.size() - 1)
    const std::size_t half = bins.size() - 1;
    const std::size_t n = 2 * half;
    if (!isPowerOfTwo(n))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
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

} // namespace kanariv::dsp::fft
