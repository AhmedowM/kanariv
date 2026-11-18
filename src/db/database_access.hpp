#pragma once

/**
 * @file database_access.hpp
 * @brief Single friendship surface for `Database`'s private members.
 * @ingroup kanariv_db
 * @details `Database` keeps its raw locking surface (`handle`, `handleLocked`,
 * `mutex`, `getCachedForUse`) and its `*Locked` helpers private. This struct
 * is the sole `friend` of `Database` and exposes static forwarders so that
 * sibling translation units (json/search and the library scan component) can
 * reach those members without befriending individual functions or classes.
 *
 * Every forwarder only delegates; the locking contract stays with the
 * caller -- the `*Locked` helpers still require the caller to hold
 * `dbMutex_`, and `getCachedForUse` is only safe under that same lock.
 *
 * KANARIV_API: the scan orchestration lives in kanariv::library (W2), so the
 * forwarders cross a DLL boundary even though this header stays src-private.
 * Internal header: lives in `src/`, not installed.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <mutex>
#include <string_view>
#include <vector>

namespace kanariv::db {

/**
 * @brief Static forwarders to the private members of `Database`.
 * @ingroup kanariv_db
 * @details Sole friend of `Database`. Each function forwards to the matching
 * private member one-for-one; see `Database` for the locking contract.
 */
struct KANARIV_API DatabaseAccess {
    // --- Raw locking surface ---
    /**
     * @brief Forwards to Database::handle().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @return Borrowed handle, may be null if not open.
     */
    static sqlite3* handle(Database& db) noexcept;
    /**
     * @brief Forwards to Database::handleLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @return Borrowed handle.
     */
    static sqlite3* handleLocked(Database& db) noexcept;
    /**
     * @brief Forwards to Database::mutex().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @return Reference to the mutex guarding the handle, statement cache and tables.
     */
    static std::mutex& mutex(Database& db) noexcept;
    /**
     * @brief Forwards to Database::getCachedForUse().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param sql SQL text used as cache key.
     * @return Prepared statement pointer, or Error (Internal) if prepare fails.
     */
    static kanariv::utils::Expected<SqliteStatement*> getCachedForUse(Database& db,
                                                                      std::string_view sql);

    // --- TrackRow / library locked helpers ---
    /**
     * @brief Forwards to Database::findByPathLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param path Exact path to search.
     * @return TrackRow or Error NotFound/Internal.
     */
    static kanariv::utils::Expected<kanariv::types::TrackRow>
    findByPathLocked(Database& db, std::string_view path);
    /**
     * @brief Forwards to Database::findByFingerprintLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param fp 32-byte fingerprint.
     * @return TrackRow or Error NotFound/Internal.
     */
    static kanariv::utils::Expected<kanariv::types::TrackRow>
    findByFingerprintLocked(Database& db, const std::array<uint8_t, 32>& fp);
    /**
     * @brief Forwards to Database::getTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param id TrackRow id.
     * @return TrackRow or Error.
     */
    static kanariv::utils::Expected<kanariv::types::TrackRow> getTrackLocked(Database& db,
                                                                             int64_t id);
    /**
     * @brief Forwards to Database::insertTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param t TrackRow to insert.
     * @return New row id or Error AlreadyExists/Internal.
     */
    static kanariv::utils::Expected<int64_t> insertTrackLocked(Database& db,
                                                               const kanariv::types::TrackRow& t);
    /**
     * @brief Forwards to Database::updateTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param t TrackRow with fields to persist.
     * @return Success or Error InvalidArg/NotFound/Internal.
     */
    static kanariv::utils::Expected<void> updateTrackLocked(Database& db,
                                                            const kanariv::types::TrackRow& t);
    /**
     * @brief Forwards to Database::setReplayGainLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param id TrackRow id.
     * @param trackGainDb ReplayGain 2.0 track gain in dB (0 = unanalyzed).
     * @param albumGainDb ReplayGain 2.0 album gain in dB (0 = unanalyzed or no album).
     * @return Success or Error NotFound/Internal.
     * @details Writes only the two gain columns so the analyze pass does not
     * rewrite (and cannot accidentally clobber) the rest of the row.
     */
    static kanariv::utils::Expected<void>
    setReplayGainLocked(Database& db, int64_t id, double trackGainDb, double albumGainDb);
    /**
     * @brief Forwards to Database::deleteTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param id TrackRow id.
     * @return Success or Error NotFound/Internal.
     */
    static kanariv::utils::Expected<void> deleteTrackLocked(Database& db, int64_t id);
    /**
     * @brief Forwards to Database::libraryListLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @return Vector of libraries or Error.
     */
    static kanariv::utils::Expected<std::vector<kanariv::types::Library>>
    libraryListLocked(Database& db);
    /**
     * @brief Forwards to Database::libraryUpdateLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param l kanariv::types::Library with updated fields.
     * @return Success or Error InvalidArg/NotFound/Internal.
     */
    static kanariv::utils::Expected<void> libraryUpdateLocked(Database& db,
                                                              const kanariv::types::Library& l);

    // --- Playlist / queue locked helpers ---
    /**
     * @brief Forwards to Database::createPlaylistLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param name Playlist name.
     * @param type Playlist type (0 = manual).
     * @param smart_query Smart filter (empty for manual).
     * @return New id or Error InvalidArg/Internal.
     */
    static kanariv::utils::Expected<int64_t>
    createPlaylistLocked(Database& db, std::string_view name, int type = 0,
                         std::string_view smart_query = {});
    /**
     * @brief Forwards to Database::playlistAddTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param pid Playlist id.
     * @param tid TrackRow id.
     * @param pos Position (-1 = append).
     * @return Success or Error.
     */
    static kanariv::utils::Expected<void> playlistAddTrackLocked(Database& db, int64_t pid,
                                                                 int64_t tid, int64_t pos = -1);
    /**
     * @brief Forwards to Database::queueEnqueueLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param qid kanariv::types::Queue id (0 -> defaultQueueId).
     * @param tid TrackRow id.
     * @param index Zero-based insertion index, or appendIndex (-1) to append.
     * @return Success or Error.
     */
    static kanariv::utils::Expected<void>
    queueEnqueueLocked(Database& db, int64_t qid, int64_t tid,
                       int64_t index = kanariv::db::appendIndex);
    /**
     * @brief Forwards to Database::queueDequeueLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param qid kanariv::types::Queue id.
     * @return QueueItem or Error NotFound.
     */
    static kanariv::utils::Expected<kanariv::types::QueueItem> queueDequeueLocked(Database& db,
                                                                                  int64_t qid);
};

} // namespace kanariv::db
