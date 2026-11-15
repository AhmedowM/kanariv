/**
 * @file commands_playlist.cpp
 * @brief Playlist inspection, editing and file transfer commands.
 */

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declarePlaylist(CLI::App& cli, Options& o) {
    auto& pl = o.playlist;
    pl.playlist = cli.add_subcommand("playlist", "Playlist operations");
    pl.list = pl.playlist->add_subcommand("list", "List playlists");
    addJsonFlag(pl.list, pl.listJson);
    pl.tracks = pl.playlist->add_subcommand("tracks", "Tracks in playlist");
    pl.tracks->add_option("pid", pl.tracksPid, "Playlist id")->required();
    addJsonFlag(pl.tracks, pl.tracksJson);
    pl.create = pl.playlist->add_subcommand("create", "Create an empty playlist");
    pl.create->add_option("name", pl.createName, "Playlist name")->required();
    pl.create->add_option("--smart", pl.createSmart, "Smart-playlist filter expression");
    addJsonFlag(pl.create, pl.createJson);
    pl.add = pl.playlist->add_subcommand("add", "Add tracks to playlist");
    pl.add->add_option("pid", pl.addPid, "Playlist id")->required();
    pl.add->add_option("--id", pl.addIds, "Library track id (repeatable)");
    pl.add->add_option("path", pl.addPaths, "File, folder, or glob (repeatable)");
    pl.add->add_flag("--recursive", pl.addRecursive, "Recurse into folders");
    addJsonFlag(pl.add, pl.addJson);
    pl.load = pl.playlist->add_subcommand("load", "Load playlist into queue");
    pl.load->add_option("pid", pl.loadPid, "Playlist id")->required();
    pl.load->add_flag("--play", pl.loadPlay, "Play after load");
    pl.load->add_flag("--replace", pl.loadReplace, "Replace active queue");
    addJsonFlag(pl.load, pl.loadJson);
    pl.save = pl.playlist->add_subcommand("save", "Save queue as playlist");
    pl.save->add_option("name", pl.saveName, "Playlist name")->required();
    pl.save->add_option("--queue", pl.saveQid, "Queue id");
    addJsonFlag(pl.save, pl.saveJson);
    pl.remove = pl.playlist->add_subcommand("delete", "Delete playlist");
    pl.remove->add_option("pid", pl.removePid, "Playlist id")->required();
    addJsonFlag(pl.remove, pl.removeJson);
    pl.rename = pl.playlist->add_subcommand("rename", "Rename playlist");
    pl.rename->add_option("pid", pl.renamePid, "Playlist id")->required();
    pl.rename->add_option("name", pl.renameName, "New name")->required();
    addJsonFlag(pl.rename, pl.renameJson);
    pl.exportFile = pl.playlist->add_subcommand("export", "Export playlist to file");
    pl.exportFile->add_option("pid", pl.exportPid, "Playlist id")->required();
    pl.exportFile->add_option("path", pl.exportPath, "Output file path")->required();
    pl.exportFile->add_option("--format", pl.exportFormat, "Format: m3u|pls|json")
        ->check(CLI::IsMember({"m3u", "pls", "json"}));
    pl.importFile = pl.playlist->add_subcommand("import", "Import playlist from file");
    pl.importFile->add_option("path", pl.importPath, "Input file path")->required();
    pl.importFile->add_option("--name", pl.importName, "Playlist name (default: filename)");
    addJsonFlag(pl.importFile, pl.importJson);
}

std::optional<int> runPlaylist(App& app, const Options& o) {
    const auto& pl = o.playlist;
    if (!pl.playlist->parsed())
        return std::nullopt;
    if (pl.list->parsed())
        return render(app.playlistList(), pl.listJson);
    if (pl.tracks->parsed())
        return render(app.playlistTracks(pl.tracksPid), pl.tracksJson);
    if (pl.create->parsed())
        return render(app.playlistCreate(pl.createName, pl.createSmart), pl.createJson);
    if (pl.add->parsed())
        return renderBatch(app.playlistAdd(pl.addPid, pl.addIds, pl.addPaths, pl.addRecursive),
                           pl.addJson);
    if (pl.load->parsed())
        return render(app.playlistLoad(pl.loadPid, pl.loadPlay, pl.loadReplace), pl.loadJson);
    if (pl.save->parsed()) {
        std::optional<std::int64_t> qid;
        if (pl.save->get_option("--queue")->count() > 0)
            qid = pl.saveQid;
        return render(app.playlistSave(pl.saveName, qid), pl.saveJson);
    }
    if (pl.remove->parsed())
        return render(app.playlistDelete(pl.removePid), pl.removeJson);
    if (pl.rename->parsed())
        return render(app.playlistRename(pl.renamePid, pl.renameName), pl.renameJson);
    if (pl.exportFile->parsed())
        return renderBatch(app.playlistExport(pl.exportPid, pl.exportPath, pl.exportFormat), false);
    if (pl.importFile->parsed()) {
        std::optional<std::string> name;
        if (!pl.importName.empty())
            name = pl.importName;
        return render(app.playlistImport(pl.importPath, name), pl.importJson);
    }
    std::cout << pl.playlist->help() << "\n";
    return 0;
}

} // namespace kanariv::app::cli::detail
