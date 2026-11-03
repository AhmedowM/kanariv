#pragma once

/**
 * @file search.hpp
 * @brief FTS5 search with LIKE fallback.
 * @ingroup kanariv_db
 * @details Implements three-stage search:
 * 1) Exact `MATCH sanitized`.
 * 2) Prefix `MATCH sanitized*`.
 * 3) `LIKE %escaped% ESCAPE '\' COLLATE NOCASE` on title/artist/album/album_artist/genre.
 * All stages are under a single `shared_lock` to avoid unlock gaps.
 */

#include <expected>
#include <kanariv/fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::db {

/**
 * @brief FTS5 search with LIKE fallback (three stages).
 * @ingroup kanariv_db
 * @param db Database to search (shared lock held for entire operation).
 * @param query Raw user query (sanitized internally).
 * @param limit Maximum rows (<=0 defaults to 50).
 * @return Matching tracks, or `Error` with `StatusCode::Internal` if no DB handle,
 * or `Corrupt`/`Internal` on SQLite errors.
 * @details Stages (all under a single `shared_lock` to avoid unlock gaps):
 * 1) Exact `MATCH sanitized`.
 * 2) Prefix `MATCH sanitized*`.
 * 3) `LIKE %escaped% ESCAPE '\' COLLATE NOCASE` on title/artist/album/album_artist/genre.
 * @par Thread safety
 * Thread-safe: acquires `db.mutex()` as `shared_lock`.
 * @see sanitizeFtsTerm
 * @see tryFtsQuery
 * @see searchLike
 */
KANARIV_API std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
searchFts(contracts::IDatabase& db, std::string_view query, int limit = 50);

/**
 * @brief LIKE-only search (no FTS).
 * @ingroup kanariv_db
 * @param db Database to search.
 * @param query Raw query substring (escaped for LIKE).
 * @param limit Maximum rows (<=0 defaults to 50).
 * @return Matching tracks, or `Error` with `StatusCode::Internal` if no handle.
 * @details Single `LIKE %escaped%` query across 5 columns with `COLLATE NOCASE`.
 * @par Thread safety
 * Thread-safe: acquires `shared_lock`.
 */
KANARIV_API std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
searchLike(contracts::IDatabase& db, std::string_view query, int limit = 50);

/**
 * @brief Sanitizes a raw FTS5 query term.
 * @ingroup kanariv_db
 * @param term Raw user input.
 * @return Sanitized FTS5 term (empty means match-nothing).
 * @details Strips FTS5 operators and escapes quotes by doubling them.
 */
KANARIV_API std::string sanitizeFtsTerm(std::string_view term);

/**
 * @brief Unified search entry point (FTS with fallback).
 * @ingroup kanariv_db
 * @param db Database to search.
 * @param query Raw query.
 * @param limit Maximum rows.
 * @return Matching tracks.
 * @par Thread safety
 * Thread-safe (delegates to `searchFts`).
 * @see searchFts
 */
KANARIV_API std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
search(contracts::IDatabase& db, std::string_view query, int limit = 50);

} // namespace kanariv::db
