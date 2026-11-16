#pragma once

/**
 * @file playback_policy.hpp
 * @brief Ring, preroll and decode-chunk policy shared by Player and Engine.
 * @details Both paths own an SpscRing feeding the audio device plus a decode
 * thread that fills it. Their buffering numbers had drifted apart (Player
 * prerolled a quarter ring, Engine half; chunk sizing was copied), so the
 * policy lives here once and both decode loops derive their numbers from it.
 * The ring capacities stay deliberately different -- see engineRingFrames().
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <kanariv/media/decoder.hpp>

namespace kanariv::playback::policy {

/// Player's ring capacity in frames (P-3: flat in every channel layout).
inline constexpr std::size_t playerRingFrames = 8192;

/**
 * @brief Engine's ring capacity in frames.
 * @details Kept at 8192*channels rather than flat 8192: a flat cap measured
 * +9 MB daemon RSS on an mp3 with an attached mjpeg cover (release-lto,
 * clang, 67.7 -> 76.6 MB WS). The mechanism is unknown (suspect indirect
 * FFmpeg buffering/timing); do not shrink without a measured explanation.
 */
[[nodiscard]] constexpr std::size_t engineRingFrames(std::uint32_t channels) noexcept {
    return playerRingFrames * channels;
}

/// Frames buffered before the device starts: half the ring, in frames.
[[nodiscard]] constexpr std::size_t prerollFrames(std::size_t ringCapacityFrames) noexcept {
    return ringCapacityFrames / 2;
}

/// Upper bound on one decoder call, in frames.
inline constexpr std::size_t maxChunkFrames = 1024;

/// Upper bound on one decoder call, in samples (keeps wide layouts cache-sized).
inline constexpr std::size_t maxChunkSamples = 2048;

/**
 * @brief Largest decode chunk for a channel layout, in frames.
 * @details min(1024, 2048/channels), never zero.
 */
[[nodiscard]] constexpr std::size_t chunkCapFrames(std::uint32_t channels) noexcept {
    if (channels == 0 || channels > maxChunkSamples)
        return 1;
    const std::size_t bySamples = maxChunkSamples / channels;
    return bySamples < maxChunkFrames ? bySamples : maxChunkFrames;
}

/**
 * @brief Decode chunk for the next call, in frames.
 * @param channels Channel count; must be non-zero.
 * @param freeFrames Frames the ring can currently accept (availableWrite).
 * @return 0 when the ring has no free frame (the caller idles); otherwise
 * min(freeFrames/channels, chunkCapFrames(channels)), at least one frame.
 */
[[nodiscard]] constexpr std::size_t chunkFrames(std::uint32_t channels,
                                                std::size_t freeFrames) noexcept {
    if (channels == 0 || freeFrames == 0)
        return 0;
    const std::size_t byRing = freeFrames / channels;
    const std::size_t cap = chunkCapFrames(channels);
    if (byRing >= cap)
        return cap;
    return byRing == 0 ? 1 : byRing;
}

/// How one decode call ends, for the caller's transition policy.
enum class DecodeOutcome : std::uint8_t {
    Continue,    ///< Frames were produced or more may follow.
    EndOfStream, ///< Eof with no new frames: nothing more can arrive.
    Failed       ///< I/O or codec failure: never a clean end of stream.
};

/**
 * @brief Classify a decode call.
 * @details The rule is shared; the transition it triggers is not (Player stops,
 * Engine crossfades/advances). A failure is never a clean end (P2), and an Eof
 * that still produced frames is not an end yet: those frames must be written
 * first.
 */
[[nodiscard]] constexpr DecodeOutcome classifyDecode(media::DecodeStatus status,
                                                     std::size_t frames) noexcept {
    if (status == media::DecodeStatus::Error)
        return DecodeOutcome::Failed;
    if (status == media::DecodeStatus::Eof && frames == 0)
        return DecodeOutcome::EndOfStream;
    return DecodeOutcome::Continue;
}

/// Decode-thread backoff while the ring is full or a spent decoder drains.
inline constexpr std::chrono::milliseconds ringIdlePoll{5};

} // namespace kanariv::playback::policy
