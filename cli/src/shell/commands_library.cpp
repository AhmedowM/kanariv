/**
 * @file commands_library.cpp
 * @brief Library scan, search, listing and maintenance commands.
 */

#include <cstdint>
#include <iostream>
#include <kanariv/utils/print.hpp>
#include <optional>
#include <string>
#include <vector>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declareLibrary(CLI::App& cli, Options& o) {
    auto& lib = o.library;
    lib.library = cli.add_subcommand("library", "Library operations");
    lib.scan = lib.library->add_subcommand("scan", "Scan library");
    lib.scan->add_option("--path", lib.scanPath, "Scan path (default: library music dir)");
    lib.scan->add_flag("--full-hash", lib.scanFullHash, "Full-file BLAKE3 (slower)");
    addJsonFlag(lib.scan, lib.scanJson);
    lib.search = lib.library->add_subcommand("search", "Search library");
    lib.search->add_option("query", lib.searchQuery, "FTS query")->required();
    lib.search->add_option("--limit", lib.searchLimit, "Limit");
    addJsonFlag(lib.search, lib.searchJson);
    lib.stats = lib.library->add_subcommand("stats", "Library stats");
    addJsonFlag(lib.stats, lib.statsJson);
    lib.stats->add_option("--top-tracks", lib.statsTopTracks,
                          "Top-N most played tracks (0 = totals only)");
    lib.stats->add_option("--top-albums", lib.statsTopAlbums,
                          "Top-N albums by total plays (0 = not requested)");
    lib.stats->add_option("--top-genres", lib.statsTopGenres,
                          "Top-N genres by total plays (0 = not requested)");
    lib.stats->add_option("--top-artists", lib.statsTopArtists,
                          "Top-N artists by total plays (multi-artist tracks split)");
    lib.stats->add_option("--queue", lib.statsQueues, "Queue overview: all or id (repeatable)");
    lib.stats->add_option("--playlist", lib.statsPlaylists, "Playlist overview by id (repeatable)");
    lib.add = lib.library->add_subcommand("add", "Add file or directory to library");
    lib.add->add_option("path", lib.addPath, "File or directory path")->required();
    lib.add->add_flag("--recursive", lib.addRecursive, "Recurse into subdirectories");
    addJsonFlag(lib.add, lib.addJson);
    lib.remove = lib.library->add_subcommand("remove", "Remove track from library");
    lib.remove->add_option("id", lib.removeQuery, "Track id or path")->required();
    addJsonFlag(lib.remove, lib.removeJson);
    lib.list = lib.library->add_subcommand("list", "List all tracks in library");
    lib.list->add_option("--query", lib.listQuery, "Search query (title, artist, album, genre)");
    lib.list->add_option(
        "--sort", lib.listSort,
        "Sort field: id, title, artist, album, year, rating, play_count, duration, date_added");
    lib.list->add_option("--order", lib.listOrder, "Sort direction: asc or desc");
    lib.list->add_option("--columns", lib.listColumns,
                         "Column allowlist: id,title,artist,album,genre,year,rating,duration,path");
    lib.list->add_option("--artist", lib.listArtist, "Filter by artist");
    lib.list->add_option("--album", lib.listAlbum, "Filter by album");
    lib.list->add_option("--genre", lib.listGenre, "Filter by genre");
    lib.list->add_option("--rating", lib.listRating, "Filter by user rating (0-5)")
        ->check(CLI::Range(0, 5));
    lib.list->add_option("--limit", lib.listLimit, "Limit results");
    lib.list->add_option("--offset", lib.listOffset, "Offset for pagination");
    addJsonFlag(lib.list, lib.listJson);
    lib.favorites = lib.library->add_subcommand("favorites", "List favorited (rated) tracks");
    lib.favorites->add_option("--min-rating", lib.favoritesMinRating, "Minimum rating (0-5)")
        ->check(CLI::Range(0, 5));
    lib.favorites->add_option("--limit", lib.favoritesLimit, "Limit results");
    addJsonFlag(lib.favorites, lib.favoritesJson);
    lib.maintenance =
        lib.library->add_subcommand("maintenance", "Report or prune missing/duplicate tracks");
    lib.maintenance->add_option("action", lib.maintenanceAction, "report or prune")
        ->check(CLI::IsMember({"report", "prune"}));
    lib.maintenance->add_flag("--dry-run", lib.maintenanceDryRun, "Report only, no changes");
    lib.maintenance->add_option("--library", lib.maintenanceLibraryId, "Library id (default: 1)");
    addJsonFlag(lib.maintenance, lib.maintenanceJson);
}

std::optional<int> runLibrary(App& app, const Options& o) {
    const auto& lib = o.library;
    if (!lib.library->parsed())
        return std::nullopt;
    if (lib.scan->parsed()) {
        std::optional<std::string> p;
        if (!lib.scanPath.empty())
            p = lib.scanPath;
        return render(app.libraryScan(p, lib.scanFullHash), lib.scanJson);
    }
    if (lib.search->parsed()) {
        auto sr = app.librarySearch(lib.searchQuery, lib.searchLimit);
        if (!sr)
            return renderError(sr.error());
        if (!sr->result)
            return 0;
        if (lib.searchJson) {
            renderJson(*sr->result);
            return exitOk;
        }
        kanariv::client::OutputFormatter fmt{false, useColor()};
        fmt.setHighlightNeedle(lib.searchQuery);
        fmt.print(*sr->result, std::cout);
        return 0;
    }
    if (lib.stats->parsed()) {
        if (lib.statsJson && (!lib.statsQueues.empty() || !lib.statsPlaylists.empty())) {
            kanariv::println(std::cerr, "library stats: --json takes no --queue or --playlist");
            return exitUsage;
        }
        return renderBatch(
            app.libraryStats(
                lib.statsTopTracks >= 0 ? std::optional<int>{lib.statsTopTracks} : std::nullopt,
                lib.statsTopAlbums > 0 ? std::optional<int>{lib.statsTopAlbums} : std::nullopt,
                lib.statsTopGenres > 0 ? std::optional<int>{lib.statsTopGenres} : std::nullopt,
                lib.statsTopArtists > 0 ? std::optional<int>{lib.statsTopArtists} : std::nullopt,
                lib.statsQueues, lib.statsPlaylists),
            lib.statsJson);
    }
    if (lib.list->parsed()) {
        std::optional<std::int32_t> ratingOpt;
        if (lib.listRating > 0)
            ratingOpt = lib.listRating;
        return render(app.libraryList(lib.listQuery, lib.listLimit, lib.listOffset, lib.listSort,
                                      lib.listOrder, lib.listArtist, lib.listAlbum, lib.listGenre,
                                      ratingOpt),
                      lib.listJson, lib.listColumns);
    }
    if (lib.favorites->parsed())
        return render(app.libraryFavorites(lib.favoritesMinRating, lib.favoritesLimit),
                      lib.favoritesJson);
    if (lib.maintenance->parsed()) {
        return render(app.libraryMaintenance(lib.maintenanceAction, lib.maintenanceDryRun,
                                             lib.maintenanceLibraryId),
                      lib.maintenanceJson);
    }
    if (lib.add->parsed())
        return render(app.libraryAdd(lib.addPath, lib.addRecursive), lib.addJson);
    if (lib.remove->parsed())
        return render(app.libraryRemove(lib.removeQuery), lib.removeJson);
    std::cout << lib.library->help() << "\n";
    return 0;
}

} // namespace kanariv::app::cli::detail
