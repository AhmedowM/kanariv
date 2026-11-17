#pragma once

#include <cstdint>

/**
 * @file history.hpp
 * @brief Engine-internal clock and history-threshold helpers (NOT installed).
 * @details Free functions used by the transport, persistence and monitor
 * paths; there is no wrapper object over `IHistoryStore` (callers hold an
 * open port already).
 */

namespace kanariv::engine::detail {

/**
 * @brief Returns steady-clock time in milliseconds.
 * @details Monotonic intervals only (progress cadence, position math).
 * Must NEVER back stored timestamps: the display and protocol read
 * them as wall-clock milliseconds since the Unix epoch.
 */
uint64_t nowMs() noexcept;

/**
 * @brief Returns wall-clock time in milliseconds since the Unix epoch.
 * @details Use for every persisted timestamp (`started_at`,
 * `completed_at`, `last_played`). Jumps with the system clock, which is
 * correct for absolute dates and harmless for ordering.
 */
uint64_t wallMs() noexcept;

/**
 * @brief Tests whether playback should count as "played" for history.
 * @param duration TrackRow duration in seconds (0 if unknown).
 * @param played Current accumulated playback time in seconds.
 * @param marked Whether already marked (true => always false).
 * @param pctThr Percentage threshold 0..100 (60 => 0.6); 0 defaults to 60.
 * @param secsThr Absolute seconds threshold; 0 defaults to 90 s.
 * @return true if `played/duration >= pct` or `played >= secs` and not already marked.
 */
bool shouldMarkPlayed(double duration, double played, bool marked, int pctThr,
                      int secsThr) noexcept;

} // namespace kanariv::engine::detail
