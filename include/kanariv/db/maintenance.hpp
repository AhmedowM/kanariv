#pragma once

/**
 * @file maintenance.hpp
 * @brief Library reconciliation and safe stale-row cleanup.
 * @ingroup kanariv_db
 */

#include <kanariv/db/database.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace kanariv::db {

/**
 * @brief Summary of a library reconciliation pass.
 * @ingroup kanariv_db
 */
struct LibraryMaintenanceReport {
    std::size_t tracks_checked{}; ///< Live rows examined.
    std::vector<kanariv::types::Track> missing_tracks{}; ///< Rows whose paths no longer exist.
    std::vector<std::vector<kanariv::types::Track>>
        duplicate_groups{}; ///< Groups sharing a path.

    /**
     * @brief Returns the number of missing-file rows.
     */
    std::size_t missingFiles() const noexcept { return missing_tracks.size(); }

    /**
     * @brief Returns the number of redundant rows in duplicate groups.
     */
    std::size_t duplicateTracks() const noexcept {
        std::size_t count = 0;
        for (const auto& group : duplicate_groups)
            if (group.size() > 1)
                count += group.size() - 1;
        return count;
    }
};

/**
 * @brief Reports database rows whose files are missing or paths are duplicated.
 * @ingroup kanariv_db
 * @param db Open database to inspect.
 * @param libraryId Library id to inspect.
 * @return Reconciliation report or an error.
 * @details This function is read-only and never deletes files or database rows.
 */
KANARIV_API std::expected<LibraryMaintenanceReport, kanariv::utils::Error>
libraryReport(Database& db, int64_t libraryId);

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
KANARIV_API std::expected<std::size_t, kanariv::utils::Error>
libraryPruneMissing(Database& db, const LibraryMaintenanceReport& report);

/**
 * @brief Options for applying library maintenance actions.
 * @ingroup kanariv_db
 */
struct LibraryMaintenanceOptions {
    bool dry_run{true}; ///< Report planned actions without changing the database.
    bool prune_missing{false}; ///< Delete rows reported as missing when not dry-running.
};

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
KANARIV_API std::expected<LibraryMaintenanceReport, kanariv::utils::Error>
libraryApplyMaintenance(Database& db, int64_t libraryId,
                         const LibraryMaintenanceOptions& options = {});

} // namespace kanariv::db
