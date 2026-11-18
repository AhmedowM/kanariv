#pragma once

/**
 * @file maintenance.hpp
 * @brief Library reconciliation and safe stale-row cleanup.
 * @ingroup kanariv_db
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <string>
#include <vector>

namespace kanariv::db {

/**
 * @brief Summary of a library reconciliation pass.
 * @ingroup kanariv_db
 * @details Alias of the neutral value type exposed by
 * `kanariv::contracts::ILibraryStore`.
 */
using LibraryMaintenanceReport = kanariv::types::LibraryMaintenanceReport;

/**
 * @brief Reports database rows whose files are missing or paths are duplicated.
 * @ingroup kanariv_db
 * @param db Open database to inspect.
 * @param libraryId Library id to inspect.
 * @return Reconciliation report or an error.
 * @details This function is read-only and never deletes files or database rows.
 */
KANARIV_API kanariv::utils::Expected<LibraryMaintenanceReport> libraryReport(Database& db,
                                                                             int64_t libraryId);

/**
 * @brief Deletes database rows for files that are no longer present.
 * @ingroup kanariv_db
 * @param db Open database to update.
 * @param report Report produced for the same database state.
 * @return Number of deleted rows or an error.
 * @details Files on disk are never deleted. The operation is transactional
 * at the database implementation level only through its individual delete
 * calls; callers needing a preview should call `libraryReport` first.
 */
KANARIV_API kanariv::utils::Expected<std::size_t>
libraryPruneMissing(Database& db, const LibraryMaintenanceReport& report);

/**
 * @brief Options for applying library maintenance actions.
 * @ingroup kanariv_db
 * @details Alias of the neutral value type used by `ILibraryStore`.
 */
using LibraryMaintenanceOptions = kanariv::types::LibraryMaintenanceOptions;

/**
 * @brief Applies selected maintenance actions after producing a report.
 * @ingroup kanariv_db
 * @param db Open database to inspect and optionally update.
 * @param libraryId Library id to inspect.
 * @param options Actions and dry-run policy.
 * @return Report describing the state before application, or an error.
 * @details Duplicate groups are report-only. They are not merged because
 * choosing a canonical row requires explicit queue, history, bookmark and
 * user-metadata policies.
 */
KANARIV_API kanariv::utils::Expected<LibraryMaintenanceReport>
libraryApplyMaintenance(Database& db, int64_t libraryId,
                        const LibraryMaintenanceOptions& options = {});

} // namespace kanariv::db
