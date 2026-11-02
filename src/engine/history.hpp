#pragma once

#include <kanariv/contracts/database.hpp>
#include <kanariv/utils.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

/**
 * @file history.hpp
 * @brief Engine-internal history helper (NOT installed).
 * @details `History` is constructed by Engine only; the entry type is the
 * shared `kanariv::types::HistoryEntry` (11 fields incl. track snapshots).
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
 * @param duration Track duration in seconds (0 if unknown).
 * @param played Current accumulated playback time in seconds.
 * @param marked Whether already marked (true => always false).
 * @param pctThr Percentage threshold 0..100 (60 => 0.6); 0 defaults to 60.
 * @param secsThr Absolute seconds threshold; 0 defaults to 90 s.
 * @return true if `played/duration >= pct` or `played >= secs` and not already marked.
 */
bool shouldMarkPlayed(double duration, double played, bool marked, int pctThr, int secsThr) noexcept;

} // namespace kanariv::engine::detail

namespace kanariv::engine {

/**
 * @brief Thin history query helper over an IDatabase implementation.
 */
class History final {
  public:
    using ExpectedVoid = std::expected<void, kanariv::utils::Error>; ///< Void or Error.
    using ExpectedEntries = std::expected<std::vector<kanariv::types::HistoryEntry>,
                                          kanariv::utils::Error>; ///< Entries or Error.

    /**
     * @brief Constructs a History helper.
     * @param db Shared database contract (must be open).
     */
    explicit History(std::shared_ptr<kanariv::contracts::IDatabase> db);

    /**
     * @brief Lists recent history entries joined with track metadata.
     * @param limit Max rows to return (0 = no limit, default 50).
     * @return Vector of entries ordered by started_at DESC or Error.
     * @par Thread safety
     * Synchronization is owned by the database implementation.
     */
    ExpectedEntries listHistory(int limit = 50);

    /**
     * @brief Clears all history rows.
     * @return Success or Error.
     * @par Thread safety
     * Synchronization is owned by the database implementation.
     */
    ExpectedVoid clearHistory();

  private:
    std::shared_ptr<kanariv::contracts::IDatabase> db_; ///< Shared persistence contract.
};

} // namespace kanariv::engine
