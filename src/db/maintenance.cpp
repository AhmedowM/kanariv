#include <caudio/db/maintenance.hpp>

#include <caudio/types/db.hpp>
#include <caudio/types/record.hpp>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace caudio::db {

std::expected<LibraryMaintenanceReport, caudio::utils::Error>
libraryReport(Database& db, int64_t libraryId) {
    if (libraryId <= 0)
        return std::unexpected(
            caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg, "invalid library id"));

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
            caudio::utils::makeError(caudio::utils::StatusCode::NotFound, "library not found"));

    caudio::types::TrackQuery query{};
    query.library_id = libraryId;
    auto tracks = db.listTracks(&query);
    if (!tracks)
        return std::unexpected(tracks.error());

    LibraryMaintenanceReport report{};
    std::map<std::string, std::vector<caudio::types::Track>> paths;
    for (const auto& track : *tracks) {
        ++report.tracks_checked;
        std::error_code ec;
        if (track.path.empty() || !std::filesystem::is_regular_file(track.path, ec)) {
            report.missing_tracks.push_back(track);
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

std::expected<std::size_t, caudio::utils::Error>
libraryPruneMissing(Database& db, const LibraryMaintenanceReport& report) {
    std::size_t pruned = 0;
    for (const auto& track : report.missing_tracks) {
        auto removed = db.deleteTrack(track.id);
        if (!removed)
            return std::unexpected(removed.error());
        ++pruned;
    }
    return pruned;
}

std::expected<LibraryMaintenanceReport, caudio::utils::Error>
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

} // namespace caudio::db
