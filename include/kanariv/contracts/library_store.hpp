#pragma once

/**
 * @file library_store.hpp
 * @brief Library catalogue port: roots, queries, statistics and maintenance.
 * @ingroup kanariv_contracts
 * @details Everything that treats the catalogue as a whole: configured library
 * roots, filtered listing and search, aggregate statistics, and the
 * reconciliation report/apply pair used by maintenance commands.
 *
 * @par Error behavior
 * Implementations use `StatusCode::NotFound` for absent libraries,
 * `InvalidArg` for invalid caller input, and `Io`, `Corrupt` or `Internal` for
 * backend failures.
 */

#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief Configured libraries, catalogue queries and statistics.
 * @ingroup kanariv_contracts
 */
class ILibraryStore : public IStore {
  public:
    /**
     * @brief Lists configured library roots.
     * @return Library values, or a backend error.
     */
    virtual kanariv::utils::Expected<std::vector<kanariv::types::Library>> listLibraries() = 0;

    /**
     * @brief Adds a library root.
     * @param path Root directory path; implementations reject an empty or
     * duplicate path.
     * @param name Display name (may be empty).
     * @return The new identifier, or `InvalidArg`/`AlreadyExists`/a backend
     * error.
     */
    virtual kanariv::utils::Expected<int64_t> libraryAdd(std::string_view path,
                                                         std::string_view name = {}) = 0;

    /**
     * @brief Updates an existing library definition.
     * @param l Library value containing the identifier and new fields.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual kanariv::utils::Expected<void> libraryUpdate(const kanariv::types::Library& l) = 0;

    /**
     * @brief Deletes a library root by identifier.
     * @param id Library identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual kanariv::utils::Expected<void> libraryDelete(int64_t id) = 0;

    /**
     * @brief Returns aggregate database statistics.
     * @return Statistics snapshot, or a backend error.
     */
    virtual kanariv::utils::Expected<kanariv::types::PlaybackStats> getStats() = 0;

    /**
     * @brief Returns library totals and optional top-N listening reports.
     * @param topTracks Number of top tracks by total plays; omitted or
     * non-positive = totals only.
     * @param topAlbums Number of top albums by total plays; omitted or
     * non-positive = not requested.
     * @param topGenres Number of top genres by total plays; omitted or
     * non-positive = not requested.
     * @param topArtists Number of top artists by total plays; omitted or
     * non-positive = not requested.
     * @details Multi-artist tracks are split so each individual artist is
     * counted separately in `top_artists`.
     */
    virtual kanariv::utils::Expected<kanariv::types::LibraryStats>
    libraryStats(std::optional<int> topTracks = std::nullopt,
                 std::optional<int> topAlbums = std::nullopt,
                 std::optional<int> topGenres = std::nullopt,
                 std::optional<int> topArtists = std::nullopt) = 0;

    /**
     * @brief Lists tracks matching an optional filter.
     * @param q Optional filter; fields are combined with AND and `search`
     * matches common text metadata. An empty filter lists all tracks ordered
     * by id.
     */
    virtual kanariv::utils::Expected<std::vector<kanariv::types::TrackRow>>
    listTracks(std::optional<kanariv::types::TrackQuery> q = std::nullopt) = 0;

    /**
     * @brief Searches track metadata using the implementation's full-text
     * index.
     * @param query Raw user query.
     * @param limit Maximum rows; non-positive values use the implementation
     * default.
     * @return Matching tracks, or a backend error.
     * @details Implementations fall back to a substring match when the
     * full-text index yields nothing.
     */
    virtual kanariv::utils::Expected<std::vector<kanariv::types::TrackRow>>
    searchTracks(std::string_view query, int limit = 50) = 0;

    /**
     * @brief Reports rows whose files are missing or whose paths are
     * duplicated.
     * @param library_id Library identifier to inspect.
     * @return Reconciliation report, or `InvalidArg`/`NotFound`/a backend
     * error.
     * @details Read-only: never deletes files or rows.
     */
    virtual kanariv::utils::Expected<kanariv::types::LibraryMaintenanceReport>
    libraryReport(int64_t library_id) = 0;

    /**
     * @brief Applies selected library maintenance actions after producing a
     * report.
     * @param library_id Library identifier to inspect and optionally update.
     * @param options Actions and dry-run policy.
     * @return Report describing the state before application, or an error.
     * @details Deleting rows is skipped when `options.dry_run` is true.
     */
    virtual kanariv::utils::Expected<kanariv::types::LibraryMaintenanceReport>
    libraryApplyMaintenance(int64_t library_id,
                            const kanariv::types::LibraryMaintenanceOptions& options) = 0;

    /**
     * @brief Exports every track row to a JSON file.
     * @param outPath Destination file path.
     * @return Success, or `Internal`/`Io`.
     */
    virtual kanariv::utils::Expected<void> exportJson(const std::filesystem::path& outPath) = 0;

    /**
     * @brief Imports track rows from a JSON file inside one transaction.
     * @param inPath Source file path.
     * @return Success, or `Io` (unreadable), `Corrupt` (malformed or failing
     * row, reported with its index) or `Internal`; the transaction rolls back
     * on any failure.
     */
    virtual kanariv::utils::Expected<void> importJson(const std::filesystem::path& inPath) = 0;
};

} // namespace kanariv::contracts
