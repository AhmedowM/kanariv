#pragma once

/**
 * @file transition_policy.hpp
 * @brief Pure decisions for the gapless/crossfade transition state machine.
 * @ingroup kanariv_engine
 * @details Internal header (lives in `src/`, not installed). The engine drives
 * track transitions from two places -- `engineTick()` arms or advances while
 * the track nears its end, the producer (`EngineSource::produce`) completes or
 * defers the transition at end of stream -- and both used to encode the
 * precedence rules inline, interleaved with locking and side effects. The rules
 * live here once, as constexpr functions over plain inputs, so the
 * configuration precedence is reviewable in one place and testable without an
 * engine, an audio device or a decode thread.
 *
 * Precedence (identical to the previous inline logic):
 * - a configured crossfade owns the transition: the monitor arms the overlap
 *   pre-roll and the decode loop never advances past an unarmed crossfade;
 * - a crossfade refused because the incoming track's layout or rate differs
 *   from the live graph is no crossfade at all: the seam advances through the
 *   normal end-of-track path, which reopens the graph at the new layout;
 * - a configured inter-track gap starts only at end of stream (the track must
 *   sound its full tail before the silence) and is ignored while a crossfade
 *   is configured;
 * - otherwise the gapless pre-roll advances the queue before the seam.
 * The callers keep every thread/lock/flag side effect; these functions only
 * name the decision.
 */

#include <cstdint>

namespace kanariv::engine::detail {

/// What the monitor should do for a track that is still playing.
enum class NearEndAction : std::uint8_t {
    None,          ///< Outside every pre-roll window, or a flag forbids arming.
    ArmCrossfade,  ///< Flag the crossfade pending; the producer preloads the next entry.
    AdvanceGapless ///< Advance the queue now; the ring's preroll covers the seam.
};

/// Inputs for the near-end decision (see Engine::Impl::engineTick).
struct NearEndInput {
    double remainingSec{0.0};     ///< duration - position, in seconds.
    bool hasCurrent{false};       ///< Whether a track is published.
    int crossfadeMs{0};           ///< EngineConfig::crossfadeMs.
    int gaplessMs{0};             ///< EngineConfig::gaplessMs.
    int interTrackGapMs{0};       ///< EngineConfig::interTrackGapMs.
    bool crossfadeActive{false};  ///< An overlap is mixing right now.
    bool crossfadePending{false}; ///< A preload request is already queued.
    bool crossfadeRefused{false}; ///< The next entry's layout blocks the overlap.
};

/**
 * @brief Decides what the monitor does as the playing track nears its end.
 * @param in Snapshot of the transport state and the transition config.
 * @return The action the caller applies (flag store or epoch-guarded advance).
 * @details A negative (or NaN) remaining time is past the end: nothing is
 * \e armed there, but the decode loop may still run its end-of-stream ladder.
 * Gapless is only considered when neither a crossfade nor a gap is configured.
 */
[[nodiscard]] constexpr NearEndAction nearEndAction(const NearEndInput& in) noexcept {
    if (!(in.remainingSec >= 0.0))
        return NearEndAction::None;
    if (in.crossfadeMs > 0) {
        if (in.crossfadeRefused)
            return NearEndAction::None;
        const double window = static_cast<double>(in.crossfadeMs) / 1000.0;
        if (in.remainingSec <= window && !in.crossfadeActive && !in.crossfadePending &&
            in.hasCurrent)
            return NearEndAction::ArmCrossfade;
        return NearEndAction::None;
    }
    // Gapless needs both values to be exactly zero: a configured gap starts at
    // EOF, and a negative value (reachable from the config file, which stores
    // plain ints) neither arms nor falls through to gapless in the old rules.
    if (in.crossfadeMs != 0 || in.interTrackGapMs != 0)
        return NearEndAction::None;
    const double window = static_cast<double>(in.gaplessMs) / 1000.0;
    if (in.remainingSec <= window && in.hasCurrent)
        return NearEndAction::AdvanceGapless;
    return NearEndAction::None;
}

/// What the decode thread should do once the outgoing decoder is at EOF.
enum class EofAction : std::uint8_t {
    PublishCrossfade, ///< Complete the in-flight overlap now.
    HoldForCrossfade, ///< Keep the cursor; the monitor re-arms the preload.
    ArmGap,           ///< Tail drained: start the inter-track silence clock.
    WaitDrain,        ///< Silence cannot start until the buffered tail is heard.
    WaitGap,          ///< Silence clock is running and the gap has not elapsed.
    Advance           ///< Move the queue to the next entry (epoch-guarded).
};

/// Inputs for the end-of-stream decision (see Engine::Impl::EngineSource::produce).
/// The gap fields are read only in the inter-track-gap branch.
struct EofInput {
    bool crossfadeActive{false};  ///< An overlap is mixing right now.
    bool crossfadeRefused{false}; ///< The overlap was refused for this seam.
    int crossfadeMs{0};           ///< EngineConfig::crossfadeMs.
    int interTrackGapMs{0};      ///< EngineConfig::interTrackGapMs.
    bool gapArmed{false};        ///< The silence clock already started.
    bool gapTailDrained{false};  ///< The ring holds no unplayed samples.
    bool gapElapsed{false};      ///< now - gapStart >= interTrackGapMs.
};

/**
 * @brief Decides how the decode thread ends the current track.
 * @param in Snapshot of the transition flags and the gap sub-state.
 * @return The action the caller applies.
 * @details A crossfade owns the transition even at EOF: the incoming track is
 * already in use, so waiting for the overlap to run out would stall the ring
 * with nothing left to decode. An armed-but-unfinished crossfade keeps the
 * cursor where it is (advancing would silently skip the next entry), but a
 * refused one (layout/rate mismatch) advances, since no overlap can start for
 * this seam. A configured gap only ever advances after the drained tail plus
 * the silence.
 */
[[nodiscard]] constexpr EofAction eofAction(const EofInput& in) noexcept {
    if (in.crossfadeActive)
        return EofAction::PublishCrossfade;
    // A refused overlap will never become active for this seam: holding the
    // cursor would stall at end of stream instead of reaching the next track.
    if (in.crossfadeRefused)
        return EofAction::Advance;
    if (in.crossfadeMs > 0)
        return EofAction::HoldForCrossfade;
    if (in.interTrackGapMs > 0) {
        if (!in.gapArmed)
            return in.gapTailDrained ? EofAction::ArmGap : EofAction::WaitDrain;
        return in.gapElapsed ? EofAction::Advance : EofAction::WaitGap;
    }
    return EofAction::Advance;
}

} // namespace kanariv::engine::detail
