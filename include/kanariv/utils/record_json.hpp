#pragma once

/**
 * @file record_json.hpp
 * @brief JSON serialization for shared record types.
 * @ingroup kanariv_utils
 * @details Single definition of the `Track` <-> `Json` mapping used by the db
 * export/import path and the IPC wire codec. It lives in `utils` (below both
 * `db` and `ipc`) so neither component has to link the other. Keys keep
 * insertion order; deserialization tolerates missing keys and normalizes
 * `library_id == 0` to 1. A missing or invalid `fingerprint` hex falls back to
 * a deterministic path-derived digest.
 */

#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <expected>

namespace kanariv::utils {

/**
 * @brief Serializes a Track to JSON.
 * @ingroup kanariv_utils
 * @param t Track to serialize.
 * @return JSON object with all Track fields; fingerprint is hex-encoded.
 * Keys keep insertion order (ordered backend behind `kanariv::utils::Json`).
 * @par Thread safety
 * Pure function, thread-safe.
 */
KANARIV_API Json trackToJson(const kanariv::types::Track& t);

/**
 * @brief Deserializes a Track from ordered JSON.
 * @ingroup kanariv_utils
 * @param j JSON object (as produced by `trackToJson`).
 * @return Track on success, or `Error` with `StatusCode::Corrupt` if parsing fails.
 * @details Missing keys use defaults; `library_id == 0` is normalized to 1.
 * If `fingerprint` is missing or not valid hex, a fallback fingerprint
 * derived from `path` is used.
 * @par Thread safety
 * Pure function, thread-safe.
 * @see trackToJson
 */
KANARIV_API std::expected<kanariv::types::Track, Error> trackFromJson(const Json& j);

} // namespace kanariv::utils
