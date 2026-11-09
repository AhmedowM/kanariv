#pragma once

#include <kanariv/types/db.hpp>
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
 * @details Order matches `fillTrackFromStmt()` indices 0..29. Keep in sync.
 */
inline constexpr std::string_view selectTracksCols =
    "SELECT id, fingerprint, path, deleted_at, size, mtime, duration, sample_rate, channels, "
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
 * @brief Maps the current row of a `selectTracksCols` statement into a Track.
 * @ingroup kanariv_db
 * @param stmt Prepared statement positioned on a row (via `step()`).
 * @param t Output track to populate.
 * @details Column order must match `selectTracksCols`:
 * 0:id 1:fingerprint(BLOB32) 2:path 3:deleted_at 4:size 5:mtime 6:duration
 * 7:sample_rate 8:channels 9:bitrate 10:title 11:artist 12:album
 * 13:album_artist 14:genre 15:year 16:track_num 17:disc_num 18:cover_art_path
 * 19:rating 20:play_count 21:last_played 22:date_added 23:last_scanned
 * 24:dirty 25:library_id 26:source_kind 27:source_key 28:source_available
 * 29:source_display_name 30:replay_gain_track 31:replay_gain_album.
 * Fingerprint is zeroed if the blob is not 32 bytes.
 * @par Thread safety
 * Caller must hold the DB lock protecting the statement's connection.
 * @see selectTracksCols
 */
void fillTrackFromStmt(sqlite3_stmt* stmt, kanariv::types::Track& t);

} // namespace kanariv::db::internal
