#pragma once

/**
 * @file loudness.hpp
 * @brief EBU R128 integrated loudness and true peak.
 * @ingroup kanariv_dsp
 * @defgroup kanariv_dsp_loudness kanariv.dsp.loudness
 *
 * Streaming measurement of the ReplayGain 2.0 quantities:
 * - integrated loudness per EBU R128 / ITU-R BS.1770-4: K-weighting, 400 ms
 *   blocks evaluated every 100 ms, a -70 LUFS absolute gate and a -10 LU
 *   relative gate;
 * - true peak from a 4x oversampled signal (polyphase interpolation);
 * - `replayGainDb()`, the gain that brings the material to the ReplayGain 2.0
 *   reference of -18 LUFS without letting the true peak exceed 0 dBTP.
 *
 * Album gain is the loudness of every track's blocks taken together, not an
 * average of track gains: feed all tracks of the album through one meter, or
 * concatenate their `blockEnergies()` and pass that to `integratedLoudness()`.
 *
 * Track boundaries are handled by constructing one meter per track. Frames may
 * be supplied in arbitrary chunk sizes; a hop that is not complete when the
 * track ends is discarded (at most 100 ms of audio), matching the block
 * definition of the standard.
 *
 * @par Channel handling
 * Every channel is weighted 1.0 (the BS.1770 layout for mono and stereo). The
 * 5.1/7.1 weights (1.41 for surrounds, 0 for LFE) need the channel layout,
 * which this API does not take; the player only analyzes mono and stereo.
 *
 * @par Thread safety
 * A meter carries filter state and is not thread-safe; use one per analyzing
 * thread. The free functions are pure and thread-safe.
 */

#include <cstddef>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <memory>
#include <span>

namespace kanariv::dsp::loudness {

/**
 * @brief ReplayGain 2.0 reference loudness in LUFS.
 * @ingroup kanariv_dsp_loudness
 * @details -18 LUFS per the ReplayGain 2.0 specification (EBU R128 based).
 */
inline constexpr double kReplayGain2ReferenceLufs = -18.0;

/**
 * @brief Integrated loudness and true peak of the measured material.
 * @ingroup kanariv_dsp_loudness
 * @details Both fields are negative infinity when nothing was measured
 * (no complete 400 ms block for the loudness, digital silence for the peak).
 */
struct LoudnessResult {
    double integratedLufs{-std::numeric_limits<double>::infinity()};
    double truePeakDbtp{-std::numeric_limits<double>::infinity()};
};

/**
 * @brief Streaming EBU R128 loudness and 4x true-peak meter.
 * @ingroup kanariv_dsp_loudness
 * @details Created once per sample rate and channel count, then fed interleaved
 * float frames until the end of the track. Holds per-channel K-weighting filter
 * state, the four-hop block ring, every completed block energy and the
 * true-peak interpolation history, so nothing is allocated per frame.
 */
class KANARIV_API LoudnessMeter {
  public:
    /**
     * @brief Create a meter for a sample rate and channel count.
     * @ingroup kanariv_dsp_loudness
     * @param sampleRate Sample rate in Hz; finite and in [8000, 384000].
     * @param channels Channel count; 1..8.
     * @return Meter on success, or `Error` with `InvalidArg`.
     * @details A non-integer sample rate is accepted; the 100 ms hop is
     * rounded to the nearest whole sample.
     */
    static std::expected<std::unique_ptr<LoudnessMeter>, kanariv::utils::Error>
    create(double sampleRate, unsigned channels);

    LoudnessMeter(LoudnessMeter&&) noexcept;
    LoudnessMeter& operator=(LoudnessMeter&&) noexcept;
    ~LoudnessMeter();
    LoudnessMeter(const LoudnessMeter&) = delete;
    LoudnessMeter& operator=(const LoudnessMeter&) = delete;

    /**
     * @brief Feed interleaved float frames.
     * @ingroup kanariv_dsp_loudness
     * @param interleaved Frames in channel order (L, R, L, R, ...); the length
     * must be a multiple of the channel count.
     * @return `Error::Ok` on success, or `InvalidArg` when the frame size is
     * not a whole number of frames.
     */
    std::expected<void, kanariv::utils::Error> addFrames(std::span<const float> interleaved);

    /**
     * @brief Weighted mean-square of every completed 400 ms block.
     * @ingroup kanariv_dsp_loudness
     * @return Block energies in presentation order; empty until the first
     * complete block. The span stays valid until the next `addFrames()` or
     * `reset()`.
     * @details This is the ungated quantity BS.1770 gates: pass the
     * concatenation of several meters' energies to `integratedLoudness()` for
     * an album gain that sees every track's blocks together.
     */
    std::span<const double> blockEnergies() const noexcept;

    /**
     * @brief Highest true-peak sample seen so far, as linear amplitude.
     * @ingroup kanariv_dsp_loudness
     * @return Maximum over the 4x oversampled signal; 0.0 before any audio.
     */
    double truePeakLinear() const noexcept;

    /**
     * @brief Current measurement.
     * @ingroup kanariv_dsp_loudness
     * @return Gated integrated loudness and true peak in dBTP.
     * @details Runs the two gating passes over every block, so call it once
     * after the last `addFrames()` rather than after every chunk.
     */
    LoudnessResult result() const noexcept;

    /**
     * @brief Discard all measurement state, keeping the configuration.
     * @ingroup kanariv_dsp_loudness
     */
    void reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit LoudnessMeter(std::unique_ptr<Impl> impl) noexcept;
};

/**
 * @brief Integrated loudness of a sequence of block energies.
 * @ingroup kanariv_dsp_loudness
 * @param blockEnergies 400 ms block energies as reported by
 * `LoudnessMeter::blockEnergies()`.
 * @return Gated loudness in LUFS, or -infinity when no block survives the
 * absolute gate.
 * @details The same two-pass gating `LoudnessMeter::result()` uses, exposed so
 * album loudness can be computed from per-track block lists without replaying
 * the audio.
 */
KANARIV_API double integratedLoudness(std::span<const double> blockEnergies) noexcept;

/**
 * @brief Convert a linear amplitude to dBTP.
 * @ingroup kanariv_dsp_loudness
 * @param linear Linear amplitude (0 = silence, 1 = full scale).
 * @return `20 * log10(linear)`, or -infinity for a non-positive input.
 */
KANARIV_API double linearToDbtp(double linear) noexcept;

/**
 * @brief ReplayGain 2.0 gain for a measurement.
 * @ingroup kanariv_dsp_loudness
 * @param result Measurement to normalize.
 * @return Gain in dB that reaches `kReplayGain2ReferenceLufs`, capped so the
 * true peak stays at or below 0 dBTP. Returns 0 when nothing was gated
 * (silence has no meaningful gain).
 */
KANARIV_API double replayGainDb(const LoudnessResult& result) noexcept;

} // namespace kanariv::dsp::loudness
