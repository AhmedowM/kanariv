/**
 * @file commands_history.cpp
 * @brief Playback history and bookmark commands.
 */

#include <cstdint>
#include <iostream>
#include <kanariv/utils/print.hpp>
#include <optional>
#include <string>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"
#include "shell.hpp"

namespace kanariv::app::cli::detail {

void declareHistory(CLI::App& cli, Options& o) {
    auto& h = o.history;
    h.history = cli.add_subcommand("history", "Playback history operations");
    h.list = h.history->add_subcommand("list", "List playback history");
    addJsonFlag(h.list, h.listJson);
    h.list->add_option("--limit", h.limit, "Limit entries");
    h.clear = h.history->add_subcommand("clear", "Clear playback history");
    addJsonFlag(h.clear, h.clearJson);
    h.bookmark = cli.add_subcommand("bookmark", "Bookmark operations");
    h.add = h.bookmark->add_subcommand("add", "Add bookmark at a position");
    h.add->add_option("time", h.addTime, "mm:ss or seconds (default: current position)");
    h.add->add_option("--note", h.addNote, "Bookmark note");
    h.add->add_option("--track", h.addTrack, "Track id (default: current track)");
    addJsonFlag(h.add, h.addJson);
    h.bookmarks = h.bookmark->add_subcommand("list", "List bookmarks");
    h.bookmarks->add_option("--track", h.bookmarksTrack, "Track id (default: current track)");
    addJsonFlag(h.bookmarks, h.bookmarksJson);
    h.remove = h.bookmark->add_subcommand("remove", "Remove a bookmark");
    h.remove->add_option("id", h.removeId, "Bookmark id")->required();
    addJsonFlag(h.remove, h.removeJson);
    h.gotoBookmark = h.bookmark->add_subcommand("goto", "Play and seek to a bookmark");
    h.gotoBookmark->add_option("id", h.gotoId, "Bookmark id")->required();
    addJsonFlag(h.gotoBookmark, h.gotoJson);
}

std::optional<int> runHistory(App& app, const Options& o) {
    const auto& h = o.history;
    if (h.history->parsed()) {
        if (h.list->parsed())
            return render(app.historyList(h.limit), h.listJson);
        if (h.clear->parsed())
            return render(app.historyClear(), h.clearJson);
        std::cout << h.history->help() << "\n";
        return 0;
    }
    if (h.bookmark->parsed()) {
        if (h.add->parsed()) {
            std::optional<double> pos;
            if (!h.addTime.empty()) {
                auto parsed = parseTime(h.addTime);
                if (!parsed) {
                    kanariv::println(
                        std::cerr, "bookmark add: '{}': {} (expected mm:ss, hh:mm:ss, or seconds)",
                        h.addTime, parsed.error().message);
                    return exitUsage;
                }
                pos = *parsed;
            }
            std::optional<std::int64_t> track;
            if (h.addTrack > 0)
                track = h.addTrack;
            return render(app.bookmarkAdd(pos, h.addNote, track), h.addJson);
        }
        if (h.bookmarks->parsed()) {
            std::optional<std::int64_t> track;
            if (h.bookmarksTrack > 0)
                track = h.bookmarksTrack;
            return render(app.bookmarkList(track), h.bookmarksJson);
        }
        if (h.remove->parsed())
            return render(app.bookmarkRemove(h.removeId), h.removeJson);
        if (h.gotoBookmark->parsed())
            return render(app.bookmarkGoto(h.gotoId), h.gotoJson);
        std::cout << h.bookmark->help() << "\n";
        return 0;
    }
    return std::nullopt;
}

} // namespace kanariv::app::cli::detail
