#pragma once
#include <string>
#include <string_view>

/**
 * @file fts.hpp
 * @brief FTS5 sanitization and LIKE escaping.
 * @ingroup kanariv_db
 * @details Internal helpers: escapeLike() escapes LIKE wildcards,
 * sanitizeFtsTerm() strips FTS5 operators and escapes quotes by doubling
 * them. Hex conversion lives in kanariv::utils (toHex/fromHex). See
 * search.hpp for the quoting logic and the queue UNIQUE invariant
 * discussion.
 */

namespace kanariv::db::internal {

/**
 * @brief Escapes LIKE wildcards % _ and \ with backslash.
 * @ingroup kanariv_db
 * @param term Raw term.
 * @return Escaped term suitable for LIKE ... ESCAPE '\'.
 */
std::string escapeLike(std::string_view term);

/**
 * @brief Sanitizes a user term for FTS5 MATCH.
 * @ingroup kanariv_db
 * @param term Raw user input.
 * @return Sanitized term or empty (match-nothing).
 * @details Strips FTS5 syntax (* : - ( ) ^ ~ '), escapes " by doubling,
 * removes logical operators AND/OR/NOT/NEAR (case-insensitive, word-boundary),
 * collapses spaces and trims. Empty after stripping returns empty.
 */
std::string sanitizeFtsTerm(std::string_view term);

} // namespace kanariv::db::internal
