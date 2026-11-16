#pragma once

/**
 * @file queue.hpp
 * @brief Queue results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstddef>
#include <cstdint>
#include <kanariv/types/record.hpp>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Queue Results
// ---------------------------------------------------------------------------

/**
 * @struct QueueTracks
 * @brief List of tracks in a queue (returned by QueueList, QueueAdd, QueueRemove, etc.).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueTracks", "tracks": [{"id": 1, "title": "Song", ...}]}
 *
 * @param tracks Vector of tracks in the queue.
 */
struct QueueTracks final {
    /**
     * @brief Vector of tracks in the queue.
     */
    std::vector<kanariv::types::TrackRow> tracks{};
};

/**
 * @struct QueueAddItem
 * @brief One resolved track's outcome inside a QueueAddReport.
 * @ingroup kanariv_ipc
 *
 * @param query The query as sent.
 * @param track Resolved track (empty id when the query did not resolve).
 * @param status "added", "duplicate" (already queued), or "failed".
 * @param detail Failure message; empty unless the status is "failed".
 */
struct QueueAddItem final {
    /**
     * @brief The query as sent.
     */
    std::string query{};
    /**
     * @brief Resolved track (empty id when the query did not resolve).
     */
    kanariv::types::TrackRow track{};
    /**
     * @brief "added", "duplicate", or "failed".
     */
    std::string status{};
    /**
     * @brief Failure message; empty unless the status is "failed".
     */
    std::string detail{};
};

/**
 * @struct QueueAddReport
 * @brief Per-query outcome of a QueueAddBatch.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueAddReport", "items": [{"query": "12", "status": "added"}]}
 *
 * @param items One entry per resolved track, in request order; a query that
 * resolves to nothing yields one entry with an empty track.
 */
struct QueueAddReport final {
    /**
     * @brief One entry per resolved track, in request order; a query that
     * resolves to nothing yields one entry with an empty track.
     */
    std::vector<QueueAddItem> items{};
};

/**
 * @struct QueueRemoveItem
 * @brief One selector's outcome inside a QueueRemoveReport.
 * @ingroup kanariv_ipc
 *
 * @param selector The selector as sent.
 * @param track Resolved track (empty id when the selector did not match).
 * @param status "removed", "missing", or "failed".
 * @param detail Failure message; empty unless the status is "failed".
 */
struct QueueRemoveItem final {
    /**
     * @brief The selector as sent.
     */
    std::string selector{};
    /**
     * @brief Resolved track (empty id when the selector did not match).
     */
    kanariv::types::TrackRow track{};
    /**
     * @brief "removed", "missing", or "failed".
     */
    std::string status{};
    /**
     * @brief Failure message; empty unless the status is "failed".
     */
    std::string detail{};
};

/**
 * @struct QueueRemoveReport
 * @brief Per-selector outcome of a QueueRemoveBatch.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueRemoveReport", "items": [{"selector": "#pos:2", "status":
 * "removed"}]}
 *
 * @param items One entry per selector, in request order.
 */
struct QueueRemoveReport final {
    /**
     * @brief One entry per selector, in request order.
     */
    std::vector<QueueRemoveItem> items{};
};

/**
 * @struct QueueEntry
 * @brief One queue in a Queues listing.
 * @ingroup kanariv_ipc
 *
 * @param id Queue id.
 * @param name Queue display name.
 * @param tracks Number of tracks in the queue.
 * @param active Whether this is the active queue.
 */
struct QueueEntry final {
    /**
     * @brief Queue id.
     */
    int64_t id{0};
    /**
     * @brief Queue display name.
     */
    std::string name{};
    /**
     * @brief Number of tracks in the queue.
     */
    std::size_t tracks{0};
    /**
     * @brief Whether this is the active queue.
     */
    bool active{false};
    /**
     * @brief Whether this is an ephemeral direct-play queue.
     */
    bool temp{false};
};

/**
 * @struct Queues
 * @brief List of queues (returned by QueueQueues).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Queues", "queues": [{"id": 1, "name": "q", "tracks": 3}]}
 *
 * @param entries Queue entries.
 */
struct Queues final {
    /**
     * @brief Queue entries.
     */
    std::vector<QueueEntry> entries{};
};

/**
 * @struct QueueCreated
 * @brief New queue id (returned by QueueCreate).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "QueueCreated", "id": 3, "name": "mix"}
 *
 * @param id New queue id.
 * @param name New queue name.
 */
struct QueueCreated final {
    /**
     * @brief New queue id.
     */
    int64_t id{0};
    /**
     * @brief New queue name.
     */
    std::string name{};
};

} // namespace kanariv::ipc
