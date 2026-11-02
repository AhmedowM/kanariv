#pragma once

/**
 * @file bookmark.hpp
 * @brief Bookmark results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details The payload owns a snapshot of bookmark rows. It can therefore be
 * retained after the client connection or database operation that produced
 * it has ended.
 */

#include <kanariv/types/record.hpp>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Bookmark Results
// ---------------------------------------------------------------------------

/**
 * @struct Bookmarks
 * @brief List of bookmarks (returned by BookmarkList).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Bookmarks", "entries": [...]}
 *
 * @param entries Vector of bookmark entries.
 */
struct Bookmarks final {
    /**
     * @brief Vector of bookmark entries.
     */
    std::vector<types::Bookmark> entries{};
};

} // namespace kanariv::ipc
