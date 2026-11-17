#pragma once

#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <string_view>

/**
 * @file stmt_helpers.hpp
 * @brief Shared statement helpers for the db layer.
 * @ingroup kanariv_db
 * @details Provides the canonical column list `selectTracksCols`,
 * the row-mapper `fillTrackFromStmt()` and the RAII `SqliteErrGuard`.
 * All helpers are header-only and live in `kanariv::db::internal`.
 */

// Forward declarations for SQLite handles (sqlite3.h stays in .cpp files).
struct sqlite3;
struct sqlite3_stmt;

namespace kanariv::db::internal {

/**
 * @brief Canonical SELECT column list for `tracks`.
 * @ingroup kanariv_db
 * @details Order matches `fillTrackFromStmt()` indices 0..30. Keep in sync.
 */
inline constexpr std::string_view selectTracksCols =
    "SELECT id, fingerprint, path, size, mtime, duration, sample_rate, channels, "
    "bitrate, title, artist, album, album_artist, genre, year, track_num, disc_num, "
    "cover_art_path, rating, play_count, last_played, date_added, last_scanned, dirty, library_id, "
    "source_kind, source_key, source_available, source_display_name, "
    "replay_gain_track, replay_gain_album "
    "FROM tracks";

/**
 * @brief Reads a TEXT column as string_view (empty on NULL).
 * @ingroup kanariv_db
 * @param stmt Statement handle.
 * @param col Zero-based column index.
 * @return View of the column text; empty if NULL.
 * @par Thread safety
 * Caller must hold appropriate DB lock; sqlite3 handles are not thread-safe.
 */
std::string_view columnText(sqlite3_stmt* stmt, int col) noexcept;

/**
 * @brief RAII guard that frees a `sqlite3_exec` error string.
 * @ingroup kanariv_db
 * @details Wraps a `char*` reference returned via `sqlite3_exec(..., &err)`.
 * On destruction, calls `sqlite3_free` if non-null and nulls the reference.
 * Non-copyable.
 */
struct SqliteErrGuard {
    char*& ref; ///< Reference to the error pointer.
    /**
     * @brief Constructs guard for the given error pointer.
     * @ingroup kanariv_db
     * @param r Reference to `char*` that will receive the SQLite error.
     */
    explicit SqliteErrGuard(char*& r) : ref(r) {}
    SqliteErrGuard(const SqliteErrGuard&) = delete;
    SqliteErrGuard& operator=(const SqliteErrGuard&) = delete;
    /** @brief Frees the error string if set. @ingroup kanariv_db */
    ~SqliteErrGuard();
};

/**
 * @brief RAII guard that finalizes a raw `sqlite3_stmt*` on scope exit.
 * @ingroup kanariv_db
 * @details Non-copyable. For code working with raw handles (prepare/step
 * loops) where SqliteStatement ownership is not wanted.
 */
struct StmtGuard {
    sqlite3_stmt* s = nullptr; ///< Owned raw statement, finalized on destruction.
    explicit StmtGuard(sqlite3_stmt* stmt) noexcept : s(stmt) {}
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
    ~StmtGuard();
    sqlite3_stmt* get() const noexcept {
        return s;
    }
    sqlite3_stmt* operator->() const noexcept {
        return s;
    }
};

/**
 * @brief Maps a failed SQLite step to the layer's StatusCode.
 * @ingroup kanariv_db
 * @param db Connection the statement ran on (supplies message and extended code).
 * @param rc Return code from `sqlite3_step`.
 * @return `AlreadyExists` for uniqueness violations, `Busy` for lock
 * contention, `Internal` otherwise; the message carries `sqlite3_errmsg`.
 * @par Thread safety
 * Caller must hold the DB lock protecting the connection.
 */
kanariv::utils::Error sqliteError(sqlite3* db, int rc);

/**
 * @brief Maps the current row of a `selectTracksCols` statement into a TrackRow.
 * @ingroup kanariv_db
 * @param stmt Prepared statement positioned on a row (via `step()`).
 * @param t Output track to populate.
 * @details Column order must match `selectTracksCols`:
 * 0:id 1:fingerprint(BLOB32) 2:path 3:size 4:mtime 5:duration
 * 6:sample_rate 7:channels 8:bitrate 9:title 10:artist 11:album
 * 12:album_artist 13:genre 14:year 15:track_num 16:disc_num 17:cover_art_path
 * 18:rating 19:play_count 20:last_played 21:date_added 22:last_scanned
 * 23:dirty 24:library_id 25:source_kind 26:source_key 27:source_available
 * 28:source_display_name 29:replay_gain_track 30:replay_gain_album.
 * Fingerprint is zeroed if the blob is not 32 bytes.
 * @par Thread safety
 * Caller must hold the DB lock protecting the statement's connection.
 * @see selectTracksCols
 */
void fillTrackFromStmt(sqlite3_stmt* stmt, kanariv::types::TrackRow& t);

} // namespace kanariv::db::internal
