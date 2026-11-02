#pragma once

/**
 * @file bookmark.hpp
 * @brief Bookmark commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstdint>
#include <optional>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Bookmark Commands
// ---------------------------------------------------------------------------

/**
 * @struct BookmarkList
 * @brief List bookmarks for a track (default: current track).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "BookmarkList", "track_id": 123}
 *
 * @param track_id Track id. If omitted, uses the current/persisted track.
 * @return Bookmarks object.
 */
struct BookmarkList final {
    /**
     * @brief Track id. If omitted, uses the current/persisted track.
     */
    std::optional<int64_t> track_id{};
};

/**
 * @struct BookmarkAdd
 * @brief Add a bookmark to a track (default: current track).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "BookmarkAdd", "position_ms": 1234, "note": "intro", "track_id": 123}
 *
 * @param position_ms Position in milliseconds.
 * @param note Optional note.
 * @param track_id Track id. If omitted, uses the current/persisted track.
 * @return Empty (success with no data).
 */
struct BookmarkAdd final {
    /**
     * @brief Position in milliseconds.
     */
    int64_t position_ms{};
    /**
     * @brief Optional note.
     */
    std::string note{};
    /**
     * @brief Track id. If omitted, uses the current/persisted track.
     */
    std::optional<int64_t> track_id{};
};

/**
 * @struct BookmarkRemove
 * @brief Remove a bookmark by id.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "BookmarkRemove", "id": 7}
 *
 * @param id Bookmark id.
 * @return Empty (success with no data).
 */
struct BookmarkRemove final {
    /**
     * @brief Bookmark id.
     */
    int64_t id{};
};

/**
 * @struct BookmarkGoto
 * @brief Play the bookmarked track and seek to its position.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "BookmarkGoto", "id": 7}
 *
 * @param id Bookmark id.
 * @return Status after the seek.
 */
struct BookmarkGoto final {
    /**
     * @brief Bookmark id.
     */
    int64_t id{};
};

} // namespace kanariv::ipc
