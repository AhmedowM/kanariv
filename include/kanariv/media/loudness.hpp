#pragma once

/**
 * @file loudness.hpp
 * @brief ReplayGain 2.0 analysis of a media file.
 * @ingroup kanariv_media
 * @defgroup kanariv_media_loudness kanariv.media.loudness
 *
 * Decodes a whole file and measures it with `kanariv::dsp::loudness`: EBU R128
 * integrated loudness and true peak, turned into a ReplayGain 2.0 gain against
 * the -18 LUFS reference. The file is decoded once, through the same FFmpeg
 * stack as playback, into interleaved float frames.
 *
 * Album gain is not a per-file quantity: take `blockEnergies` of every track of
 * the album, concatenate them and pass the result to
 * `kanariv::dsp::loudness::integratedLoudness`. `kanariv::library` does that in
 * its analyze pass.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <string_view>
#include <vector>

namespace kanariv::media {

/**
 * @brief Loudness measurement of one decoded file.
 * @ingroup kanariv_media_loudness
 * @details `integratedLufs` and `truePeakDbtp` are -infinity for digital
 * silence; the gain is then 0, because there is nothing to normalize.
 */
struct LoudnessAnalysis {
    double integratedLufs{-std::numeric_limits<double>::infinity()}; ///< EBU R128 LUFS.
    double truePeakDbtp{-std::numeric_limits<double>::infinity()};   ///< True peak in dBTP.
    double gainDb{}; ///< ReplayGain 2.0 track gain (dB), peak-limited at 0 dBTP.
    /// Per-400 ms block energies; the input for an album-level measurement.
    std::vector<double> blockEnergies;
};

/**
 * @brief Measures the loudness and true peak of a media file.
 * @ingroup kanariv_media_loudness
 * @param path Audio file path (UTF-8; owned internally).
 * @return Measurement, or `Error` with `Io` (open/read/decode failed),
 * `Unsupported` (no audio stream, no decoder, or a sample rate/channel count
 * outside what the meter supports), or `Internal` (no audio decoded).
 * @par Thread safety
 * Re-entrant; the FFmpeg session is local to the call.
 */
KANARIV_API kanariv::utils::Expected<LoudnessAnalysis> analyzeLoudness(std::string_view path);

} // namespace kanariv::media
