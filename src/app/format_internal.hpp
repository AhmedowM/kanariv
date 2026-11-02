#pragma once

/**
 * @file format_internal.hpp
 * @brief App-internal display formatting helpers (NOT public API).
 * @ingroup kanariv_app
 * @details Track labels and the playlist writers used inside the app layer.
 * The public `kanariv/app/format.hpp` exposes only the clock formatter, so
 * frontends cannot depend on these helpers.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <kanariv/ipc/result/playback.hpp>
#include <kanariv/types/record.hpp>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

/**
 * @brief Human track label for transport confirmations.
 * @ingroup kanariv_app
 * @param st Status carrying artist/title/path.
 * @return "artist - title", either half, the filename, or "unknown track".
 */
std::string trackWho(const kanariv::ipc::Status& st);

/**
 * @brief Human label for added tracks: "artist - title: file.ext".
 * @ingroup kanariv_app
 * @details Artist/title parts omitted when empty, bare filename when both
 * are, "track <id>" when nothing else exists.
 */
std::string addedLabel(const kanariv::types::Track& t);

/**
 * @brief Writes tracks as M3U/PLS/plain text.
 * @ingroup kanariv_app
 * @param format "m3u", "pls", or anything else for plain paths.
 */
void writePlaylistText(std::ostream& os, const std::vector<kanariv::types::Track>& tracks,
                       std::string_view format);

/**
 * @brief Writes tracks as `{"format":"kanariv-playlist",...}` JSON.
 * @ingroup kanariv_app
 */
void writePlaylistJson(std::ostream& os, const std::vector<kanariv::types::Track>& tracks);

} // namespace kanariv::app
