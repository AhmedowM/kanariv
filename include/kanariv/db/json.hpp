#pragma once

/**
 * @file json.hpp
 * @brief JSON import/export for tracks.
 * @ingroup kanariv_db
 * @details Implements `exportJson` and `importJson` for JSON serialization of
 * Track rows. The Track codec itself lives in `kanariv::utils`
 * (`utils/record_json.hpp`). Uses ordered JSON (keys in insertion order) to
 * preserve field order across export/import.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/utils/record_json.hpp>
#include <expected>
#include <filesystem>

namespace kanariv::db {

/**
 * @brief Exports all tracks to a JSON file.
 * @ingroup kanariv_db
 * @param db Database to export from.
 * @param outPath Destination file path.
 * @return Success, or `Error` with `StatusCode::Internal`/`Io` on DB or file failure.
 * @details Calls `db.listTracks(nullptr)` and writes `{"tracks": [...]}` with
 * 2-space indentation. Holds a shared lock via `listTracks`.
 * @par Thread safety
 * Thread-safe (shared lock).
 * @see importJson
 * @see trackToJson
 */
KANARIV_API std::expected<void, kanariv::utils::Error> exportJson(contracts::IDatabase& db,
                                                     const std::filesystem::path& outPath);

/**
 * @brief Imports tracks from a JSON file (transactional upsert).
 * @ingroup kanariv_db
 * @param db Database to import into.
 * @param inPath Source file path.
 * @return Success, or `Error` with `StatusCode::Io` if file cannot be opened,
 * `StatusCode::Corrupt` if JSON is invalid or missing `tracks` array, or
 * `StatusCode::Internal` on DB errors. On `Corrupt`, the transaction is rolled back.
 * @details Reads the entire file, parses JSON, validates `tracks` is an array,
 * then inserts each track inside a single `DbTransaction` (`BEGIN IMMEDIATE`).
 * On `SQLITE_CONSTRAINT` (duplicate fingerprint), looks up the existing row
 * and updates it; path collisions are silently ignored. Holds `db.mutex()`
 * exclusively for the transaction duration.
 * @par Thread safety
 * Thread-safe: acquires `db.mutex()` as `unique_lock`.
 * @par Lock ordering
 * `db.mutex()` (dbMutex_) exclusively for the whole import.
 * @see exportJson
 * @see trackFromJson
 */
KANARIV_API std::expected<void, kanariv::utils::Error> importJson(contracts::IDatabase& db,
                                                     const std::filesystem::path& inPath);

} // namespace kanariv::db
