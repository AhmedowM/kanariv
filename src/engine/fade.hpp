#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

/**
 * @file fade.hpp
 * @brief Fade-in/fade-out envelope and crossfade mixing for decoded chunks.
 * @ingroup kanariv_engine
 * @details Internal header (lives in `src/`, not installed). Extracted from
 * `Engine::Impl::decodeLoop` so the envelope can be unit-tested directly at
 * mono/stereo/5.1 channel counts without an audio device or decode thread.
 *
 * The chunk buffer is interleaved `frames * channels` floats. The envelope is
 * evaluated once per *frame* (a frame is one sample period across all
 * channels) and the resulting gain is applied to every channel of that frame;
 * `FadeParams::startFrame` is therefore written in frames, matching
 * `fadeInMs * sampleRate / 1000` (a frame count, not a sample count).
 *
 * Thread safety: stateless pure function over caller-owned buffers; the caller
 * (decodeLoop) owns the buffer and the fade counters.
 */

namespace kanariv::engine::detail {

/**
 * @brief Fade envelope parameters for one decoded chunk.
 * @ingroup kanariv_engine
 * @details `startFrame` positions the chunk inside the track so the ramps
 * continue seamlessly across chunk boundaries.
 */
struct FadeParams {
    std::uint64_t startFrame{0};    ///< Frame index of the chunk start within the track.
    std::uint64_t fadeInFrames{0};  ///< Fade-in length in frames (0 disables fade-in).
    std::uint64_t fadeOutFrames{0}; ///< Fade-out length in frames (0 disables fade-out).
    double fadeOutStartSec{0.0};    ///< Position (seconds) where the fade-out begins.
    double sampleRate{48000.0};     ///< Frames per second (<= 0 falls back to 48000).
};

/**
 * @brief Applies the fade envelope and optional crossfade mix in place.
 * @ingroup kanariv_engine
 * @param buf Interleaved chunk buffer, at least `frames * channels` floats.
 * @param frames Frame count in the chunk.
 * @param channels Channel count (0 is treated as invalid and yields 0).
 * @param params Envelope parameters (fade-in/out lengths and start position).
 * @param incoming Optional second buffer (same layout) mixed in for a
 * crossfade; empty applies the envelope only.
 * @param crossfadeProgress Equal-power mix position in [0,1]; clamped above 1.
 * @return Number of interleaved samples written (`frames * channels`), or 0
 * when the arguments cannot describe a valid chunk.
 * @details Fade-in ramps 0 -> 1 over `fadeInFrames`; fade-out ramps 1 -> 0
 * over `fadeOutFrames` from `fadeOutStartSec` and takes precedence when both
 * apply (matching the original decodeLoop arithmetic in double precision).
 * The crossfade mix keeps the outgoing gain applied to both tracks:
 * `out * sqrt(1-p) * gain + in * sqrt(p) * gain`.
 * @par Thread safety
 * Stateless; buffers are caller-owned and must not alias the envelope state.
 */
inline std::size_t applyFadeEnvelope(std::span<float> buf, std::size_t frames,
                                     std::uint32_t channels, const FadeParams& params,
                                     std::span<const float> incoming = {},
                                     double crossfadeProgress = 0.0) noexcept {
    if (channels == 0 || frames == 0 || buf.size() < frames * channels)
        return 0;
    const std::size_t samples = frames * channels;
    const double sr = params.sampleRate > 0.0 ? params.sampleRate : 48000.0;
    const bool crossfade = incoming.size() >= samples && !incoming.empty();
    double progress = crossfadeProgress;
    if (progress > 1.0)
        progress = 1.0;
    if (progress < 0.0)
        progress = 0.0;
    // Equal-power crossfade weights (constant for the chunk).
    const double outWeight = std::sqrt(1.0 - progress);
    const double inWeight = std::sqrt(progress);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint64_t frameIdx = params.startFrame + f;
        double gain = 1.0;
        // Fade-in: ramp 0 -> 1 over fadeInFrames.
        if (params.fadeInFrames > 0 && frameIdx < params.fadeInFrames)
            gain = static_cast<double>(frameIdx) / static_cast<double>(params.fadeInFrames);
        // Fade-out: ramp 1 -> 0 over fadeOutFrames starting at fadeOutStartSec.
        if (params.fadeOutFrames > 0) {
            const double posSec = static_cast<double>(frameIdx) / sr;
            if (posSec >= params.fadeOutStartSec) {
                const double fadeProg = (posSec - params.fadeOutStartSec) /
                                        (static_cast<double>(params.fadeOutFrames) / sr);
                gain = (fadeProg >= 1.0) ? 0.0 : (1.0 - fadeProg);
            }
        }
        const std::size_t base = f * channels;
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::size_t idx = base + c;
            if (crossfade) {
                const double outSample = static_cast<double>(buf[idx]);
                const double inSample = static_cast<double>(incoming[idx]);
                buf[idx] =
                    static_cast<float>(outSample * outWeight * gain + inSample * inWeight * gain);
            } else {
                buf[idx] = static_cast<float>(static_cast<double>(buf[idx]) * gain);
            }
        }
    }
    return samples;
}

} // namespace kanariv::engine::detail
