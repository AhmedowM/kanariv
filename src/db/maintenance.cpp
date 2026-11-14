#include <kanariv/db/maintenance.hpp>

#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/log.hpp>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utils/path_utf8.hpp>

namespace kanariv::db {

namespace {

/// Result of checking a stored UTF-8 path against the filesystem.
enum class PathState { present, missing, unknown };

/**
 * @brief Whether a filesystem error means "the path does not exist".
 * @details libstdc++ on Windows reports ERROR_FILE_NOT_FOUND (2) and
 * ERROR_PATH_NOT_FOUND (3) through the error_code overload instead of
 * returning file_type::not_found, so those codes are absence, not failure.
 */
bool isAbsenceError(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
           ec.value() == 2 || ec.value() == 3;
}

/**
 * @brief Classifies a stored UTF-8 path without the ANSI code page.
 * @details `pathFromUtf8` preserves characters the ANSI code page cannot
 * represent; only a clean negative `is_regular_file` answer counts as missing.
 * A filesystem error, an empty path or a conversion failure is `unknown`, so a
 * maintenance pass never reports (or prunes) a row it could not check.
 */
PathState classifyPath(std::string_view utf8Path) {
    if (utf8Path.empty())
        return PathState::missing;
    std::error_code ec;
    try {
        const auto path = kanariv::utils::detail::pathFromUtf8(utf8Path);
        if (std::filesystem::is_regular_file(path, ec))
            return PathState::present;
        if (!ec || isAbsenceError(ec))
            return PathState::missing;
        return PathState::unknown;
    } catch (const std::exception&) {
        return PathState::unknown;
    }
}

} // namespace

std::expected<LibraryMaintenanceReport, kanariv::utils::Error>
libraryReport(Database& db, int64_t libraryId) {
    if (libraryId <= 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "invalid library id"));

    auto libraries = db.listLibraries();
    if (!libraries)
        return std::unexpected(libraries.error());
    bool found = false;
    for (const auto& library : *libraries) {
        if (library.id == libraryId) {
            found = true;
            break;
        }
    }
    if (!found)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "library not found"));

    kanariv::types::TrackQuery query{};
    query.library_id = libraryId;
    auto tracks = db.listTracks(&query);
    if (!tracks)
        return std::unexpected(tracks.error());

    LibraryMaintenanceReport report{};
    std::map<std::string, std::vector<kanariv::types::Track>> paths;
    for (const auto& track : *tracks) {
        ++report.tracks_checked;
        switch (classifyPath(track.path)) {
        case PathState::missing:
            report.missing_tracks.push_back(track);
            break;
        case PathState::unknown:
            if (auto* logger = db.logger())
                logger->warn("maintenance: cannot check track {} path, keeping row", track.id);
            break;
        case PathState::present:
            break;
        }
        if (!track.path.empty())
            paths[track.path].push_back(track);
    }
    for (auto& [path, group] : paths) {
        (void)path;
        if (group.size() > 1)
            report.duplicate_groups.push_back(std::move(group));
    }
    return report;
}

std::expected<std::size_t, kanariv::utils::Error>
libraryPruneMissing(Database& db, const LibraryMaintenanceReport& report) {
    std::size_t pruned = 0;
    for (const auto& track : report.missing_tracks) {
        // Re-check immediately before deleting: the report may be stale, and a
        // path that cannot be represented must never cost a row.
        const PathState state = classifyPath(track.path);
        if (state != PathState::missing) {
            if (auto* logger = db.logger()) {
                logger->warn("maintenance: skipping prune of track {} (path state {})", track.id,
                             state == PathState::present ? "present" : "unknown");
            }
            continue;
        }
        auto removed = db.deleteTrack(track.id);
        if (!removed)
            return std::unexpected(removed.error());
        ++pruned;
    }
    return pruned;
}

std::expected<LibraryMaintenanceReport, kanariv::utils::Error>
libraryApplyMaintenance(Database& db, int64_t libraryId,
                         const LibraryMaintenanceOptions& options) {
    auto report = libraryReport(db, libraryId);
    if (!report)
        return std::unexpected(report.error());
    if (options.prune_missing && !options.dry_run) {
        auto pruned = libraryPruneMissing(db, *report);
        if (!pruned)
            return std::unexpected(pruned.error());
    }
    return report;
}

std::expected<kanariv::types::LibraryMaintenanceReport, kanariv::utils::Error>
Database::libraryReport(int64_t libraryId) {
    return kanariv::db::libraryReport(*this, libraryId);
}

std::expected<kanariv::types::LibraryMaintenanceReport, kanariv::utils::Error>
Database::libraryApplyMaintenance(int64_t libraryId,
                                  const kanariv::types::LibraryMaintenanceOptions& options) {
    return kanariv::db::libraryApplyMaintenance(*this, libraryId, options);
}

} // namespace kanariv::db
