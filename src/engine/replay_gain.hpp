#pragma once

/**
 * @file replay_gain.hpp
 * @brief Playback-side ReplayGain policy (which stored gain applies).
 * @ingroup kanariv_engine
 * @details Internal header: lives in `src/`, not installed. Header-only so the
 * selection rule is unit-testable without an engine or an audio device.
 */

#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>

namespace kanariv::engine::detail {

/**
 * @brief Gain in dB the current ReplayGain mode applies to one track.
 * @ingroup kanariv_engine
 * @param mode Configured ReplayGain mode.
 * @param t Track about to play, with its persisted gain columns.
 * @return 0 when the mode is Off or the track carries no gain.
 * @details Gains are written only by the explicit analysis pass
 * (`kanariv::library::analyzeLibrary`); 0 in the album column means unanalyzed.
 * Album mode therefore falls back to the track gain when the album gain is
 * missing, so enabling Album on a partially analyzed library still normalizes
 * instead of silently playing at full scale.
 */
inline double replayGainDbFor(kanariv::types::ReplayGainMode mode,
                              const kanariv::types::TrackRow& t) noexcept {
    switch (mode) {
    case kanariv::types::ReplayGainMode::Off:
        return 0.0;
    case kanariv::types::ReplayGainMode::Track:
        return t.replayGainTrack;
    case kanariv::types::ReplayGainMode::Album:
        return t.replayGainAlbum != 0.0 ? t.replayGainAlbum : t.replayGainTrack;
    }
    return 0.0;
}

} // namespace kanariv::engine::detail
