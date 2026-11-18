#pragma once

/**
 * @file fft.hpp
 * @brief Real and complex FFT plans with windowing helpers.
 * @ingroup kanariv_dsp
 * @defgroup kanariv_dsp_fft kanariv.dsp.fft
 *
 * KissFFT-backed transforms over float samples (vendored kissfft 131.1.0,
 * BSD-3-Clause). The vendored C core stays private to `kanariv::dsp`; callers
 * see only the plan/window API below.
 *
 * Transform lengths must be powers of two. KissFFT itself also factorizes
 * other sizes, but the fixed-frame consumers (spectrum analysis) never ask for
 * anything else, so the restriction keeps one predictable code path.
 *
 * Error codes (via `kanariv::utils::Expected<T>`):
 * - InvalidArg: zero-length or non-power-of-two `n`, mismatched span sizes,
 *   or a plan used for the transform kind it was not created for
 * - Internal: the plan was moved from (empty) or allocation failed
 *
 * @par Thread safety
 * - `FftPlan` is a *plan*, not a pure function: the KissFFT configuration owns
 *   the temporary buffer used while transforming, so a single plan is **not
 *   reentrant**. Executing one from two threads at once (or recursively) is a
 *   data race; use one plan per thread that transforms concurrently. Same
 *   contract caveat as an `av_fft`/FFTW plan.
 * - Window helpers are pure functions and thread-safe.
 */

#include <complex>
#include <cstddef>
#include <expected>
#include <kanariv/fwd.hpp>
#include <memory>
#include <span>
#include <vector>

namespace kanariv::dsp::fft {

/**
 * @brief Opaque reusable FFT plan for one power-of-two length.
 * @ingroup kanariv_dsp_fft
 * @details Move-only value that owns the transform length, the KissFFT
 * configuration and the scratch buffer every execution reuses, so transforming
 * many blocks of the same length allocates only once. Executions serialise on
 * that buffer, which is what makes the object a plan (see the header's
 * thread-safety note).
 */
class KANARIV_API FftPlan {
  public:
    FftPlan() noexcept;
    FftPlan(FftPlan&& other) noexcept;
    FftPlan& operator=(FftPlan&& other) noexcept;
    ~FftPlan();
    FftPlan(const FftPlan&) = delete;
    FftPlan& operator=(const FftPlan&) = delete;

    /**
     * @brief Create a real-input plan for length `n`.
     * @ingroup kanariv_dsp_fft
     * @param n Transform length; must be a power of two and >= 2.
     * @return Plan on success, or `Error` with `InvalidArg` / `Internal`.
     * @details `executeR2C` consumes `n` real samples and produces `n/2 + 1`
     * complex bins (DC through Nyquist).
     */
    static kanariv::utils::Expected<std::unique_ptr<FftPlan>> createR2C(std::size_t n);

    /**
     * @brief Create a complex-input plan for length `n`.
     * @ingroup kanariv_dsp_fft
     * @param n Transform length; must be a power of two and >= 2.
     * @return Plan on success, or `Error` with `InvalidArg` / `Internal`.
     * @details `executeC2C` consumes `n` complex samples and produces `n`
     * complex bins. Set `inverse=true` on `executeC2C` for an inverse FFT.
     */
    static kanariv::utils::Expected<std::unique_ptr<FftPlan>> createC2C(std::size_t n);

    /**
     * @brief Execute a real-input FFT.
     * @ingroup kanariv_dsp_fft
     * @param input  `n` real samples (`n == length()`).
     * @param output `n/2 + 1` complex bins (`outputBins()`).
     * @return `Error::Ok` on success, `InvalidArg` on size mismatch or when the
     * plan was created with `createC2C`, or `Internal` when the plan is empty.
     * @warning Not reentrant: the plan's scratch buffer is used in place.
     */
    kanariv::utils::Expected<void> executeR2C(std::span<const float> input,
                                              std::span<std::complex<float>> output) const;

    /**
     * @brief Execute a complex-input FFT (forward or inverse).
     * @ingroup kanariv_dsp_fft
     * @param input   `n` complex samples (`n == length()`).
     * @param output  `n` complex bins (`n == length()`).
     * @param inverse When true, computes the inverse FFT (scales by 1/n).
     * @return `Error::Ok` on success, `InvalidArg` on size mismatch or when the
     * plan was created with `createR2C`, or `Internal` when the plan is empty.
     * @warning Not reentrant: the plan's scratch buffer is used in place.
     */
    kanariv::utils::Expected<void> executeC2C(std::span<const std::complex<float>> input,
                                              std::span<std::complex<float>> output,
                                              bool inverse = false) const;

    /**
     * @brief Transform length (power of two).
     * @ingroup kanariv_dsp_fft
     */
    std::size_t length() const noexcept;

    /**
     * @brief Number of real-output bins for an R2C plan (`n/2 + 1`).
     * @ingroup kanariv_dsp_fft
     */
    std::size_t outputBins() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FftPlan(std::unique_ptr<Impl> impl) noexcept;
};

namespace window {

/**
 * @brief Hann window of length `n`.
 * @ingroup kanariv_dsp_fft
 * @param n Window length; must be >= 1.
 * @return `n` samples of `0.5f * (1 - cos(2*pi*i/(n-1)))`.
 * @details For `n == 1` returns `{1.0f}`. Symmetric (analysis) form.
 */
KANARIV_API std::vector<float> hann(std::size_t n);

/**
 * @brief Hamming window of length `n`.
 * @ingroup kanariv_dsp_fft
 * @param n Window length; must be >= 1.
 * @return `n` samples of `0.54f - 0.46f * cos(2*pi*i/(n-1))`.
 * @details For `n == 1` returns `{1.0f}`. Symmetric (analysis) form.
 */
KANARIV_API std::vector<float> hamming(std::size_t n);

/**
 * @brief Apply a window in place to `samples`.
 * @ingroup kanariv_dsp_fft
 * @param samples Audio block to window (modified in place).
 * @param window Window coefficients; must equal `samples.size()`.
 * @return `Error::Ok` on success, or `InvalidArg` on size mismatch.
 * @details `samples[i] *= window[i]` for every `i`.
 */
KANARIV_API kanariv::utils::Expected<void> applyInPlace(std::span<float> samples,
                                                        std::span<const float> window);

} // namespace window

/**
 * @brief One-shot real FFT: plan + execute in a single call.
 * @ingroup kanariv_dsp_fft
 * @param samples `n` real samples (`n` must be a power of two).
 * @return `n/2 + 1` complex bins (DC through Nyquist), or `Error`.
 * @details Convenience wrapper; prefer `FftPlan` when transforming many
 * blocks of the same length to reuse the plan.
 */
KANARIV_API kanariv::utils::Expected<std::vector<std::complex<float>>>
fftReal(std::span<const float> samples);

/**
 * @brief One-shot inverse real FFT: plan + execute in a single call.
 * @ingroup kanariv_dsp_fft
 * @param bins `n/2 + 1` complex bins (DC through Nyquist).
 * @return `n` real samples, or `Error`.
 * @details Reconstructs the full conjugate-symmetric spectrum, runs an
 * inverse complex FFT, scales by 1/n, and returns the real part.
 */
KANARIV_API kanariv::utils::Expected<std::vector<float>>
ifftReal(std::span<const std::complex<float>> bins);

} // namespace kanariv::dsp::fft
