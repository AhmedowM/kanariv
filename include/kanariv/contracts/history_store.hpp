#pragma once

/**
 * @file history_store.hpp
 * @brief Playback history port.
 * @ingroup kanariv_contracts
 * @details History and play counts are strictly opt-in: a consumer that does
 * not want them keeps the port wired but never queries it, and the engine
 * treats a missing store as "history off" rather than an error.
 *
 * @par Error behavior
 * Implementations use `StatusCode::NotFound` for absent rows and `Io` or
 * `Internal` for backend failures.
 */

#include <cstdint>
#include <expected>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief Playback history and play counts.
 * @ingroup kanariv_contracts
 */
class IHistoryStore : public IStore {
  public:
    /**
     * @brief Records a completed or partially completed track play.
     * @param track_id TrackRow identifier.
     * @param started_at Start timestamp in the implementation's persisted time
     * unit.
     * @param completed_at Completion timestamp in the same unit.
     * @param position_ms Position reached in milliseconds.
     * @param completion_pct Completion percentage.
     * @param queue_id Queue from which the track was played.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    markPlayed(int64_t track_id, int64_t started_at, int64_t completed_at, int64_t position_ms,
               double completion_pct, int64_t queue_id) = 0;

    /**
     * @brief Lists playback history entries.
     * @param limit Maximum number of entries; the implementation defines the
     * behavior of non-positive values.
     * @return History entries, or a backend error.
     */
    virtual std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(int limit) = 0;

    /**
     * @brief Clears playback history.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> clearHistory() = 0;
};

} // namespace kanariv::contracts
