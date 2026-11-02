#pragma once

/**
 * @file database_access.hpp
 * @brief Single friendship surface for `Database`'s private members.
 * @ingroup kanariv_db
 * @details `Database` keeps its raw locking surface (`handle`, `handleLocked`,
 * `mutex`, `getCachedForUse`) and its `*Locked` helpers private. This struct
 * is the sole `friend` of `Database` and exposes static forwarders so that
 * sibling translation units (scan/json/search, engine, service) can reach
 * those members without befriending individual functions or classes.
 *
 * Every forwarder only delegates; the locking contract stays with the
 * caller -- the `*Locked` helpers still require the caller to hold
 * `dbMutex_`, and `getCachedForUse` still requires `cacheMutex_`.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <array>
#include <kanariv/db/database.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace kanariv::db {

/**
 * @brief Static forwarders to the private members of `Database`.
 * @ingroup kanariv_db
 * @details Sole friend of `Database`. Each function forwards to the matching
 * private member one-for-one; see `Database` for the locking contract.
 */
struct DatabaseAccess {
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
     * @return Reference to the shared_mutex guarding the handle and tables.
     */
    static std::shared_mutex& mutex(Database& db) noexcept;
    /**
     * @brief Forwards to Database::getCachedForUse().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param sql SQL text used as cache key.
     * @return Prepared statement pointer, or Error (Internal) if prepare fails.
     */
    static std::expected<SqliteStatement*, kanariv::utils::Error>
    getCachedForUse(Database& db, std::string_view sql);

    // --- Track / library locked helpers ---
    /**
     * @brief Forwards to Database::findByPathLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param path Exact path to search.
     * @return Track or Error NotFound/Internal.
     */
    static std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByPathLocked(Database& db, std::string_view path);
    /**
     * @brief Forwards to Database::findByFingerprintLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param fp 32-byte fingerprint.
     * @return Track or Error NotFound/Internal.
     */
    static std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByFingerprintLocked(Database& db, const std::array<uint8_t, 32>& fp);
    /**
     * @brief Forwards to Database::getTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param id Track id.
     * @return Track or Error.
     */
    static std::expected<kanariv::types::Track, kanariv::utils::Error> getTrackLocked(Database& db,
                                                                                    int64_t id);
    /**
     * @brief Forwards to Database::insertTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param t Track to insert.
     * @return New row id or Error AlreadyExists/Internal.
     */
    static std::expected<int64_t, kanariv::utils::Error>
    insertTrackLocked(Database& db, const kanariv::types::Track& t);
    /**
     * @brief Forwards to Database::updateTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param t Track with fields to persist.
     * @return Success or Error InvalidArg/NotFound/Internal.
     */
    static std::expected<void, kanariv::utils::Error>
    updateTrackLocked(Database& db, const kanariv::types::Track& t);
    /**
     * @brief Forwards to Database::deleteTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param id Track id.
     * @return Success or Error NotFound/Internal.
     */
    static std::expected<void, kanariv::utils::Error> deleteTrackLocked(Database& db, int64_t id);
    /**
     * @brief Forwards to Database::libraryListLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @return Vector of libraries or Error.
     */
    static std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error>
    libraryListLocked(Database& db);
    /**
     * @brief Forwards to Database::libraryUpdateLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param l kanariv::types::Library with updated fields.
     * @return Success or Error InvalidArg/NotFound/Internal.
     */
    static std::expected<void, kanariv::utils::Error> libraryUpdateLocked(Database& db,
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
    static std::expected<int64_t, kanariv::utils::Error>
    createPlaylistLocked(Database& db, std::string_view name, int type = 0,
                         std::string_view smart_query = {});
    /**
     * @brief Forwards to Database::playlistAddTrackLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param pid Playlist id.
     * @param tid Track id.
     * @param pos Position (-1 = append).
     * @return Success or Error.
     */
    static std::expected<void, kanariv::utils::Error>
    playlistAddTrackLocked(Database& db, int64_t pid, int64_t tid, int64_t pos = -1);
    /**
     * @brief Forwards to Database::queueEnqueueLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param qid kanariv::types::Queue id (0 -> 1).
     * @param tid Track id.
     * @param pos Position (-1 = append).
     * @return Success or Error.
     */
    static std::expected<void, kanariv::utils::Error>
    queueEnqueueLocked(Database& db, int64_t qid, int64_t tid, int64_t pos = -1);
    /**
     * @brief Forwards to Database::queueDequeueLocked().
     * @ingroup kanariv_db
     * @param db Database instance.
     * @param qid kanariv::types::Queue id.
     * @return QueueItem or Error NotFound.
     */
    static std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
    queueDequeueLocked(Database& db, int64_t qid);
};

} // namespace kanariv::db
