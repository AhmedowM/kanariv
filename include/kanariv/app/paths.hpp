#pragma once

/**
 * @file paths.hpp
 * @brief File/glob/path utilities shared by all frontends.
 * @ingroup kanariv_app
 * @details Moved out of the CLI shell: glob expansion for queue-add style
 * inputs. Frontends expand user tokens with these, then hand file lists to
 * App methods. The comparable path key and the numeric-token check are
 * app-internal (`src/app/paths_internal.hpp`).
 */

#include <kanariv/fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

/**
 * @brief Checks for glob characters (`*` or `?`).
 * @ingroup kanariv_app
 * @param s Token to inspect.
 * @return `true` when the token contains either wildcard character; false
 * otherwise. No filesystem access is performed.
 */
KANARIV_API bool hasGlobChars(std::string_view s);

/**
 * @brief Expands one add token into files.
 * @ingroup kanariv_app
 * @param token Glob, folder, or single file path.
 * @param recursive Descend into subfolders for folder tokens.
 * @param files Out: absolutized audio files (sorted per token).
 * @param unmatched Out: tokens resolving to nothing (caller diagnoses).
 * @details Glob = non-recursive filename match in the pattern's parent dir;
 * folders yield top-level (or recursive) audio files; anything else lands
 * in `unmatched`.
 * @par Thread safety
 * Re-entrant for independent output vectors. The filesystem is queried while
 * expanding a token, so results can change if another process mutates the
 * directory concurrently.
 */
KANARIV_API void expandAddToken(const std::string& token, bool recursive,
                                std::vector<std::string>& files,
                                std::vector<std::string>& unmatched);

} // namespace kanariv::app
