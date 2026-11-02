#pragma once

/**
 * @file smart.hpp
 * @brief Smart-playlist query parser.
 * @ingroup kanariv_db
 * @details Parses an allowlisted filter language into a TrackQuery so the
 * db layer can evaluate a playlist's smart_query. Predicates are comma
 * separated: field OP value. Allowed fields: rating, year, play_count,
 * duration, artist, album, genre, search. Ops: =, >=, <=, >, <.
 */

#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <expected>
#include <string_view>

namespace kanariv::db {

/**
 * @brief Parses a smart-playlist query string into a TrackQuery.
 * @ingroup kanariv_db
 * @param q Raw smart_query text (comma-separated field OP value predicates).
 * @return Parsed TrackQuery, or Error InvalidArg on an unrecognized field,
 *         operator, or value.
 * @details Allowed fields: rating, year, play_count, duration (numeric;
 *         ops =, >=, <=, >, <), artist, album, genre (exact string match;
 *         op = only), search (substring; op = only). Multiple predicates
 *         combine with AND. Empty input yields an empty TrackQuery (match all).
 * @par Thread safety
 * Pure function; no locking.
 */
KANARIV_API std::expected<kanariv::types::TrackQuery, kanariv::utils::Error>
parseSmartQuery(std::string_view q);

} // namespace kanariv::db
