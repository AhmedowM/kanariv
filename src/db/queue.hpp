#pragma once

#include <kanariv/types/db.hpp>
#include <kanariv/utils.hpp>
#include <cstdint>
#include <db/detail.hpp>
#include <db/statement.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file queue.hpp
 * @brief kanariv::types::Queue table helpers (single-writer invariant).
 * @ingroup kanariv_db
 * @details All helpers are `*Locked` -- the caller must hold the
 * Database mutex (dbMutex_) exclusively for mutating ops and at least
 * shared for reads. The `queue` table enforces UNIQUE(queue_id, position)
 * (see schema.hpp); position shifts use
 * `UPDATE queue SET position=position+1 WHERE queue_id=? AND position>=?`
 * which requires serialized access -- guaranteed by the single-writer lock.
 * Every function normalizes qid == 0 to 1 (default queue).
 */

// Forward declarations for SQLite handles (sqlite3.h stays in .cpp files).
struct sqlite3;
struct sqlite3_stmt;

namespace kanariv::db {

/**
 * @brief Enqueues a track (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @param tid Track id (must be non-zero).
 * @param pos Position (-1 = append at MAX+1, else shift + insert).
 * @return Success or Error with StatusCode::InvalidArg if tid==0,
 * StatusCode::Internal if no handle or SQLite error.
 * @details If pos < 0, computes MAX(position)+1; otherwise shifts
 * positions >= pos via position+1 before inserting. The UNIQUE invariant
 * requires exclusive lock (caller must hold dbMutex_).
 * @par Thread safety
 * Caller must hold Database::mutex().
 */
std::expected<void, kanariv::utils::Error> queueEnqueueLocked(sqlite3* db, int64_t qid, int64_t tid,
                                                             int64_t pos = -1);

/**
 * @brief Dequeues the head item (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return QueueItem or Error NotFound/Internal.
 * @details Selects ORDER BY position LIMIT 1, deletes it, then shifts
 * remaining positions down by 1 (position-1 where position > removed).
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeueLocked(sqlite3* db,
                                                                                 int64_t qid);

/**
 * @brief Removes item at position (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @param pos Position to remove.
 * @return Success or Error NotFound/Internal.
 * @details Deletes WHERE queue_id=? AND position=?, then shifts down
 * positions > pos.
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<void, kanariv::utils::Error> queueRemoveLocked(sqlite3* db, int64_t qid, int64_t pos);

/**
 * @brief Clears all items in a queue (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return Success or Error Internal.
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<void, kanariv::utils::Error> queueClearLocked(sqlite3* db, int64_t qid);

/**
 * @brief Lists all items in a queue ordered by position (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return Vector of QueueItems or Error Internal.
 * @par Thread safety
 * Caller must hold Database::mutex() (shared or exclusive).
 */
std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
queueListLocked(sqlite3* db, int64_t qid);

/**
 * @brief Counts items in a queue (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return Count (0 if no db or on prepare failure).
 * @par Thread safety
 * Caller must hold Database::mutex() (shared or exclusive).
 */
size_t queueCountLocked(sqlite3* db, int64_t qid);

/**
 * @brief Finds the 0-based index of a track in a queue (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @param trackId Track id to locate (first occurrence by position).
 * @return 0-based index into the ORDER BY position listing, or std::nullopt
 * if the track is not in the queue (or on prepare failure / null handle).
 * @details O(log n) via the queue position index: resolves the track's
 * minimum position, then counts predecessors. Matches the vector-index
 * semantics of queueListLocked even if positions ever have gaps.
 * @par Thread safety
 * Caller must hold Database::mutex() (shared or exclusive).
 */
std::optional<size_t> queueIndexOfLocked(sqlite3* db, int64_t qid, int64_t trackId);

/**
 * @brief Gets a queue container row (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return kanariv::types::Queue or Error NotFound/Internal.
 * @par Thread safety
 * Caller must hold Database::mutex().
 */
std::expected<kanariv::types::Queue, kanariv::utils::Error> getQueueLocked(sqlite3* db, int64_t qid);

/**
 * @brief Lists all queue containers (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @return Vector of Queues or Error.
 * @par Thread safety
 * Caller must hold Database::mutex().
 */
std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> listQueuesLocked(sqlite3* db);

/**
 * @brief Creates a queue container (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param name kanariv::types::Queue name (must be non-empty).
 * @param library_id Owning library.
 * @return New row id or Error InvalidArg/Internal.
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<int64_t, kanariv::utils::Error> createQueueLocked(sqlite3* db, std::string_view name,
                                                               int64_t library_id = 1);

/**
 * @brief Deletes a queue container and its items (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @return Success or Error NotFound/Internal.
 * @details First deletes from queue (items), then from queues (container).
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<void, kanariv::utils::Error> deleteQueueLocked(sqlite3* db, int64_t qid);

/**
 * @brief Sets the repeat mode for a queue (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @param repeat_mode New mode.
 * @return Success or Error NotFound/Internal.
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<void, kanariv::utils::Error> setQueueRepeatLocked(sqlite3* db, int64_t qid,
                                                               int repeat_mode);

/**
 * @brief Sets the temporary flag for a queue (caller holds DB mutex).
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @param qid kanariv::types::Queue id (0 -> 1).
 * @param temp New flag (temporary queues are purged on daemon shutdown).
 * @return Success or Error NotFound/Internal.
 * @par Thread safety
 * Caller must hold Database::mutex() exclusively.
 */
std::expected<void, kanariv::utils::Error> setQueueTempLocked(sqlite3* db, int64_t qid, bool temp);

} // namespace kanariv::db
