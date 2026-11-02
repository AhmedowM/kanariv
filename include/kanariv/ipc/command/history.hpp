#pragma once

/**
 * @file history.hpp
 * @brief Playback history commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <optional>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// History Commands
// ---------------------------------------------------------------------------

/**
 * @struct HistoryList
 * @brief List playback history entries.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "HistoryList", "limit": 50}
 *
 * @param limit Maximum number of entries to return. If omitted, returns 50.
 * @return History object with vector of history entries.
 */
struct HistoryList final {
    /**
     * @brief Maximum number of entries to return. If omitted, returns 50.
     */
    std::optional<int> limit{};
};

/**
 * @struct HistoryClear
 * @brief Clear all playback history.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "HistoryClear"}
 *
 * @return Empty (success with no data).
 */
struct HistoryClear final {};

} // namespace kanariv::ipc
