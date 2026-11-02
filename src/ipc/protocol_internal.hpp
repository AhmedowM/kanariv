#pragma once

/**
 * @file protocol_internal.hpp
 * @brief Internal IPC JSON helpers -- not part of the public API.
 * @ingroup kanariv_ipc
 * @details Declares the JSON serialization helpers for enums, playlists,
 * and errors used by `src/ipc/protocol.cpp`. Lives in `src/` (not installed)
 * so downstream consumers never see them.
 */

#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace kanariv::ipc::detail {

using Json = kanariv::utils::Json;

/**
 * @brief Convert StatusCode enum to string representation.
 * @ingroup kanariv_ipc
 * @param r Status code to convert.
 * @return String representation (e.g., "Ok", "InvalidArg", "NotFound").
 */
std::string resultCodeToString(kanariv::utils::StatusCode r);

/**
 * @brief Convert string to StatusCode enum.
 * @ingroup kanariv_ipc
 * @param sv String to parse.
 * @return StatusCode enum value, or error if unknown.
 */
std::expected<kanariv::utils::StatusCode, kanariv::utils::Error>
resultCodeFromString(std::string_view sv);

/**
 * @brief Convert Playlist struct to JSON object.
 * @ingroup kanariv_ipc
 * @param p Playlist to convert.
 * @return JSON object with all playlist fields.
 */
Json playlistToJson(const kanariv::types::Playlist& p);

/**
 * @brief Convert JSON object to Playlist struct.
 * @ingroup kanariv_ipc
 * @param j JSON object with playlist fields.
 * @return Playlist struct, or error if parsing fails.
 */
std::expected<kanariv::types::Playlist, kanariv::utils::Error> playlistFromJson(const Json& j);

/**
 * @brief Convert Error struct to JSON object.
 * @ingroup kanariv_ipc
 * @param e Error to convert.
 * @return JSON object with type, code, code_value, and message fields.
 */
Json errorToJson(const kanariv::utils::Error& e);

/**
 * @brief Convert JSON object to Error struct.
 * @ingroup kanariv_ipc
 * @param j JSON object with code/code_value and message fields.
 * @return Error struct, or error if parsing fails.
 */
std::expected<kanariv::utils::Error, kanariv::utils::Error> errorFromJson(const Json& j);

} // namespace kanariv::ipc::detail
