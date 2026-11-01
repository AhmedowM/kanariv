#pragma once

/**
 * @file history.hpp
 * @brief Playback history results for the caudio IPC protocol.
 * @ingroup caudio_ipc
 * @details This is a transport value, not a live database cursor. The vector
 * owns its entries and is fully materialized before it is serialized into an
 * IPC reply.
 */

#include <caudio/types/record.hpp>
#include <vector>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// History Results
// ---------------------------------------------------------------------------

/**
 * @struct History
 * @brief List of playback history entries (returned by HistoryList).
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "History", "entries": [...]}
 *
 * @param entries Vector of history entries.
 */
struct History final {
    /**
     * @brief Vector of history entries.
     */
    std::vector<types::HistoryEntry> entries{};
};

} // namespace caudio::ipc
