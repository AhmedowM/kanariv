#pragma once

/**
 * @file queue.hpp
 * @brief Queue management commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstddef>
#include <cstdint>
#include <kanariv/fwd.hpp>
#include <kanariv/types/wire.hpp>
#include <optional>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Queue Commands
// ---------------------------------------------------------------------------

/**
 * @struct QueueList
 * @brief List all tracks in the active queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueList", "order": "playback"}
 *
 * @param order Track order: "playback" (shuffle order, default) or "added"
 * (insertion order).
 * @return QueueTracks object with vector of tracks.
 */
struct QueueList final {
    /**
     * @brief Track order ("playback" or "added").
     */
    std::string order{"playback"};
};

/**
 * @struct QueueQueues
 * @brief List all available queues with their track counts.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueQueues"}
 *
 * @return Queues object with queue entries (id, name, track count, active).
 */
struct QueueQueues final {};

/**
 * @struct QueueCreate
 * @brief Create a new empty queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueCreate", "name": "mix"}
 *
 * @param name Display name for the new queue.
 * @return QueueCreated object with the new queue id and name.
 */
struct QueueCreate final {
    /**
     * @brief Display name for the new queue.
     */
    std::string name{};
};

/**
 * @struct QueueDelete
 * @brief Delete a queue by id (never the active queue).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueDelete", "qid": 2}
 *
 * @param qid Queue ID to delete.
 * @return Empty (success with no data).
 */
struct QueueDelete final {
    /**
     * @brief Queue ID to delete.
     */
    int64_t qid{1};
};

/**
 * @struct PlayFiles
 * @brief Play files now in a (temporary) queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "PlayFiles", "paths": ["/music/a.mp3"], "save": false}
 *
 * @param paths Absolute audio file paths.
 * @param save When false the queue is temporary (purged on daemon shutdown).
 * @return Status object with updated playback state.
 */
struct PlayFiles final {
    /**
     * @brief Absolute audio file paths.
     */
    std::vector<std::string> paths{};
    /**
     * @brief Keep the queue permanently (default: temporary).
     */
    bool save{false};
};

/**
 * @struct QueueSwitch
 * @brief Switch the active queue by ID.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueSwitch", "qid": 2}
 *
 * @param qid Queue ID to switch to (1-based).
 * @return Status object with updated playback state.
 */
struct QueueSwitch final {
    /**
     * @brief Queue ID to switch to (1-based).
     */
    int64_t qid{1};
};

/**
 * @struct QueueAdd
 * @brief Add track(s) to the active queue.
 *
 * Can add a single file by path, search by query, or add by track ID.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueAdd", "query": "song.mp3", "search": false}
 *
 * @param query Search query, file path, or track ID string.
 * @param search If true, treat query as a search term; otherwise as a path or ID.
 * @return QueueTracks object with updated queue contents.
 */
struct QueueAdd final {
    /**
     * @brief Search query, file path, or track ID string.
     */
    std::string query{};
    /**
     * @brief If true, treat query as a search term; otherwise as a path or ID.
     */
    bool search{false};
};

/**
 * @struct QueueRemove
 * @brief Remove a track from the active queue by position or track ID.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueRemove", "idOrIndex": "5"}
 *
 * @param idOrIndex Queue position (0-based) or track ID.
 * @return QueueTracks object with updated queue contents.
 */
struct QueueRemove final {
    /**
     * @brief Queue position (0-based) or track ID.
     */
    std::string idOrIndex{};
};

/**
 * @struct QueueMove
 * @brief Move a track within the queue from one position to another.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueMove", "from": 3, "to": 0}
 *
 * @param from Source position (0-based).
 * @param to Destination position (0-based).
 * @return QueueTracks object with updated queue contents.
 */
struct QueueMove final {
    /**
     * @brief Source position (0-based).
     */
    std::size_t from{0};
    /**
     * @brief Destination position (0-based).
     */
    std::size_t to{0};
};

/**
 * @struct QueueClear
 * @brief Remove all tracks from the active queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueClear"}
 *
 * @return QueueTracks object with empty queue.
 */
struct QueueClear final {};

/**
 * @struct QueueShuffle
 * @brief Enable, disable, or toggle shuffle mode for the active queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueShuffle", "on": true}
 *
 * @param on If set, explicitly enable (true) or disable (false). If omitted, toggles the current
 * state.
 * @return Status object with updated shuffle state.
 */
struct QueueShuffle final {
    /**
     * @brief If set, explicitly enable (true) or disable (false).
     * If omitted, toggles the current state.
     */
    std::optional<bool> on{};
};

/**
 * @struct QueueRepeat
 * @brief Set or query repeat mode for the active queue.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueRepeat", "mode": "One"}
 *
 * @param mode Repeat mode to set. If omitted, returns current mode via status.
 * @return Status object with updated repeat mode.
 */
struct QueueRepeat final {
    /**
     * @brief Repeat mode to set. If omitted, returns current mode via status.
     */
    std::optional<kanariv::types::RepeatMode> mode{};
};

} // namespace kanariv::ipc
