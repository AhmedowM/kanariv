#pragma once

/**
 * @file convert_loop.hpp
 * @brief Fixed-window pump for a streaming resampler.
 * @ingroup kanariv_media
 * @details Internal header: lives in `src/`, not installed. Deliberately free
 * of FFmpeg types so the truncation logic is unit-testable without an audio
 * file or a real `SwrContext`.
 */

#include <cstddef>
#include <span>

namespace kanariv::media::internal {

/**
 * @brief Pumps a converter into a fixed-capacity window until it reports empty.
 * @ingroup kanariv_media
 * @tparam Convert `int(float* out, std::size_t frames)`: fills at most `frames`
 * interleaved frames into `out`, returns the frames produced (>= 0) or a
 * negative error, and returns 0 when nothing is left.
 * @tparam OnBlock `void(std::span<const float>)`: receives each non-empty window
 * in stream order, already slice to `channels` samples per frame.
 * @param window Reusable output window (`windowFrames * channels` samples).
 * @param channels Interleaved channel count; must be nonzero.
 * @param convert Converter (typically a `swr_convert` wrapper).
 * @param onBlock Consumer of the converted samples.
 * @return 0 when the converter is drained, otherwise its first negative error.
 * @details A single `swr_convert` call is not enough: swresample produces at
 * most what the output window holds, holds the rest of an oversized input
 * frame internally, and later returns it only when called again (with a null
 * input, or with the next frame). Calling it once per input frame therefore
 * silently truncates any frame larger than `windowFrames` and drops the
 * resampler's filter-delay tail at end of stream. This loop repeats until the
 * converter reports 0, which is exactly the "spill" pattern used by
 * `kanariv::media::Decoder` (`convertFrameBuffered`/`drainResampler`).
 */
template <typename Convert, typename OnBlock>
int pumpConvert(std::span<float> window, std::size_t channels, Convert&& convert,
                OnBlock&& onBlock) {
    if (channels == 0 || window.empty())
        return 0;
    const std::size_t windowFrames = window.size() / channels;
    for (;;) {
        const int produced = convert(window.data(), windowFrames);
        if (produced < 0)
            return produced;
        if (produced == 0)
            return 0;
        // A converter that claims more than the window cannot have written it;
        // clamp so a broken implementation degrades to one window instead of
        // reading past the buffer.
        const auto frames = static_cast<std::size_t>(produced);
        const std::size_t usable = frames < windowFrames ? frames : windowFrames;
        onBlock(window.first(usable * channels));
    }
}

} // namespace kanariv::media::internal
