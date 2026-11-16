#pragma once

/**
 * @file migration.hpp
 * @brief Versioned schema migrations driven by `PRAGMA user_version`.
 * @ingroup kanariv_db
 * @details The schema is an ordered ladder: each step runs in its own
 * `BEGIN IMMEDIATE`/`COMMIT` transaction together with the `user_version`
 * bump, so a failing step rolls back and leaves the previous version intact.
 * `user_version == 0` marks a database created before versioning (or a fresh
 * file); it runs every step from the base schema upwards.
 *
 * Statements inside one step are executed in order under one transaction and
 * an error aborts the step: no schema error is tolerated silently.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <cstdint>
#include <expected>
#include <kanariv/utils.hpp>
#include <span>
#include <string_view>

// Forward declaration for the SQLite handle (sqlite3.h stays in .cpp files).
struct sqlite3;

namespace kanariv::db::internal {

/**
 * @brief Applies a schema step that cannot be expressed as plain SQL.
 * @ingroup kanariv_db
 * @param db SQLite handle.
 * @return Success, or the error that aborts (and rolls back) the step.
 */
using MigrationFn = std::expected<void, kanariv::utils::Error> (*)(sqlite3* db);

/**
 * @brief One ordered schema migration.
 * @ingroup kanariv_db
 * @details Steps are applied in ascending `version`; the stored
 * `PRAGMA user_version` is set to `version` only after the step succeeded.
 */
struct MigrationStep {
    int version;                ///< Schema version this step produces.
    std::string_view name;      ///< Diagnostic label used in error messages.
    std::string_view sql;       ///< Statements to run; empty when `apply` is set.
    MigrationFn apply{nullptr}; ///< Optional C++ step (e.g. idempotent ALTERs).
};

/**
 * @brief Current schema version a fully migrated database reports.
 * @ingroup kanariv_db
 */
inline constexpr int kSchemaVersion = 8;

/**
 * @brief Applies connection-level pragmas (WAL, busy timeout, foreign keys).
 * @ingroup kanariv_db
 * @param db SQLite handle (must not be null).
 * @return Success, or an error when a pragma fails.
 * @details Must run before migrations: journal mode and page size cannot
 * change inside a transaction.
 */
std::expected<void, kanariv::utils::Error> configureConnection(sqlite3* db);

/**
 * @brief Returns the production migration ladder (ascending versions).
 * @ingroup kanariv_db
 */
std::span<const MigrationStep> schemaMigrations() noexcept;

/**
 * @brief Reads `PRAGMA user_version`.
 * @ingroup kanariv_db
 * @param db SQLite handle (must not be null).
 * @return Stored schema version, or an error when the pragma cannot be read.
 */
std::expected<int, kanariv::utils::Error> readUserVersion(sqlite3* db);

/**
 * @brief Applies every step above the database's stored version.
 * @ingroup kanariv_db
 * @param db SQLite handle (must not be null).
 * @param steps Ordered steps; steps with `version` <= the stored version are
 * skipped, so re-running against a current database is a no-op.
 * @return Success, or the error of the first failing step (already rolled back).
 * @details Each step runs inside its own transaction. A database whose
 * `user_version` is newer than the last step is rejected instead of being
 * downgraded.
 */
std::expected<void, kanariv::utils::Error> applyMigrations(sqlite3* db,
                                                           std::span<const MigrationStep> steps);

} // namespace kanariv::db::internal
