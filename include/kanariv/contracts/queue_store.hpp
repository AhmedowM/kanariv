#pragma once

/**
 * @file queue_store.hpp
 * @brief Queue persistence port.
 * @ingroup kanariv_contracts
 * @details Operations on queue containers and their ordered items. The engine
 * drives playback through this port; the service mutates it. No other
 * persistence concern belongs here.
 *
 * @par Queue index vocabulary
 * Every parameter named `index` is a zero-based index into the ordered queue
 * listing, in `[0, count]` for insertion and `[0, count)` for removal.
 * `QueueItem::position` is the backend-assigned gap-based ordering value; it
 * is an implementation detail and must never be passed back as an index.
 *
 * @par Thread safety
 * Implementations must serialize their own storage access. Callers may issue
 * operations from the engine's monitor/decode paths and from IPC handlers.
 *
 * @par Error behavior
 * Implementations use `StatusCode::NotFound` for absent queues or items,
 * `InvalidArg` for out-of-range indices and empty names, `State` for an
 * invalid lifecycle state, and `Io`, `Corrupt`, `Internal` or `Busy` for
 * backend failures.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief Queue container and item persistence.
 * @ingroup kanariv_contracts
 */
class IQueueStore : public IStore {
  public:
    /**
     * @brief Loads a queue container by identifier.
     * @param qid Queue identifier; implementations normalize zero to the
     * default queue.
     * @return Queue value, or `NotFound`/a backend error.
     */
    virtual std::expected<kanariv::types::Queue, kanariv::utils::Error> getQueue(int64_t qid) = 0;

    /**
     * @brief Reports whether a queue exists.
     * @param qid Queue id; implementations normalize zero to the default queue.
     * @return `true`/`false`, or a backend error.
     */
    virtual std::expected<bool, kanariv::utils::Error> queueExists(int64_t qid) = 0;

    /**
     * @brief Lists all queue containers.
     * @return Queue values, or a backend error.
     */
    virtual std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error>
    listQueues() = 0;

    /**
     * @brief Returns the number of items in a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Item count (zero for an empty queue), or a backend error.
     */
    virtual std::expected<std::size_t, kanariv::utils::Error> queueCount(int64_t qid) = 0;

    /**
     * @brief Finds a track's zero-based index in a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @param trackId Track id to locate (first occurrence by position).
     * @return Zero-based index of the first matching item, `std::nullopt` when
     * the track is absent, or a backend error.
     */
    virtual std::expected<std::optional<std::size_t>, kanariv::utils::Error>
    queueIndexOf(int64_t qid, int64_t trackId) = 0;

    /**
     * @brief Loads the track at a zero-based queue index.
     * @param qid Queue identifier; zero selects the default queue.
     * @param index Zero-based index into the ordered listing.
     * @return Track on success, or `NotFound` when the index is beyond the
     * queue or the row is missing.
     */
    virtual std::expected<kanariv::types::Track, kanariv::utils::Error>
    queueTrackAt(int64_t qid, int64_t index) = 0;

    /**
     * @brief Lists queue items in ascending position order.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Queue items, or a backend error. Each item's `position` is the
     * stored gap-based ordering value, not its zero-based index.
     */
    virtual std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
    listQueueItems(int64_t qid) = 0;

    /**
     * @brief Creates a queue container.
     * @param name Display name; implementations reject an empty name.
     * @param library_id Owning library identifier.
     * @return The new queue identifier, or `InvalidArg`/a backend error.
     */
    virtual std::expected<int64_t, kanariv::utils::Error> createQueue(std::string_view name,
                                                                      int64_t library_id = 1) = 0;

    /**
     * @brief Deletes a queue container and its items.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> deleteQueue(int64_t qid) = 0;

    /**
     * @brief Sets whether a queue is temporary.
     * @param qid Queue identifier.
     * @param temp `true` to mark the queue temporary, otherwise `false`.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> setQueueTemp(int64_t qid, bool temp) = 0;

    /**
     * @brief Sets the repeat mode stored for a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @param mode Repeat mode to store.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    setQueueRepeat(int64_t qid, kanariv::types::RepeatMode mode) = 0;

    /**
     * @brief Enqueues track identifiers atomically.
     * @param qid Queue identifier; zero selects the default queue.
     * @param tids Ordered track identifiers to append.
     * @return Success, or an error; no partial batch is committed.
     */
    virtual std::expected<void, kanariv::utils::Error>
    queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) = 0;

    /**
     * @brief Enqueues one track at a zero-based insertion index.
     * @param qid Queue identifier; zero selects the default queue.
     * @param tid Track identifier.
     * @param index Zero-based insertion index in `[0, count]`, or `-1` to
     * append.
     * @return Success, or `InvalidArg` for an out-of-range index, `NotFound`,
     * or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> queueEnqueue(int64_t qid, int64_t tid,
                                                                    int64_t index = -1) = 0;

    /**
     * @brief Removes the item at a zero-based queue index.
     * @param qid Queue identifier; zero selects the default queue.
     * @param index Zero-based index to remove (must be >= 0).
     * @return Success, or `InvalidArg` for a negative index, `NotFound` when
     * the index is beyond the queue, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> queueRemove(int64_t qid, int64_t index) = 0;

    /**
     * @brief Replaces all items in a queue from an ordered span of identifiers.
     * @param qid Queue identifier; zero selects the default queue.
     * @param ids New ordered track identifiers.
     * @return Success, or an error; implementations must commit atomically.
     */
    virtual std::expected<void, kanariv::utils::Error>
    queueReplaceAll(int64_t qid, std::span<const int64_t> ids) = 0;

    /**
     * @brief Removes all items from a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> queueClear(int64_t qid) = 0;
};

} // namespace kanariv::contracts
