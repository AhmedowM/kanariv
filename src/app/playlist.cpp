/**
 * @file playlist.cpp
 * @brief Playlist command handlers.
 * @ingroup caudio_app
 * @details Phase 1: returns outcome data; the frontend renders. Listing,
 * track contents, creation, adding (ids and paths with library
 * resolution), loading, saving, deletion, renaming, file export/import.
 */

#include <app/app_impl.hpp>
#include <app/detail.hpp>
#include <app/format_internal.hpp>
#include <app/paths_internal.hpp>
#include <caudio/app/app.hpp>
#include <caudio/app/paths.hpp>
#include <caudio/client/client.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/utils/error.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace caudio::app {

AppResult App::playlistList() {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::playlistTracks(std::int64_t pid) {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistTracks{pid}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::playlistCreate(const std::string& name,
                              const std::string& smart_query) {
    caudio::ipc::PlaylistCreate pc{name};
    pc.smart_query = smart_query;
    caudio::ipc::Command cmd{std::move(pc)};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* pcRes = std::get_if<caudio::ipc::PlaylistCreated>(&*res)) {
        // Hoisted: copy before Outcome moves *res (see App::resume).
        std::int64_t id = pcRes->id;
        std::string pName = pcRes->name;
        return Outcome{.result = std::move(*res),
                       .line = std::format("Created playlist {} '{}'", id, pName),
                       .toStderr = false,
                       .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

BatchResult App::playlistAdd(std::int64_t pid, const std::vector<std::string>& ids,
                             const std::vector<std::string>& paths, bool recursive) {
    if (ids.empty() && paths.empty())
        return BatchReport::fail("playlist add: need --id ID or PATH");
    std::vector<int64_t> resolved;
    for (auto& s : ids) {
        if (!caudio::app::isNumeric(s))
            return BatchReport::fail("playlist add: --id needs numeric ids");
        try {
            resolved.push_back(std::stoll(s));
        } catch (...) {
            return BatchReport::fail(std::format("playlist add: id out of range: {}", s));
        }
    }
    // PATHs: expand, ensure library rows, resolve exact ids.
    std::vector<std::string> files;
    std::vector<std::string> unmatched;
    for (auto& tok : paths)
        caudio::app::expandAddToken(tok, recursive, files, unmatched);
    BatchReport rep;
    bool hardFail = false;
    for (auto& u : unmatched) {
        if (caudio::app::isNumeric(u)) {
            detail::emitLine(rep.err,
                             std::format("playlist add: '{}' is not a file (use --id for ids)", u));
            hardFail = true;
        } else if (caudio::app::hasGlobChars(u)) {
            detail::emitLine(rep.err, std::format("No files matched: {}", u));
        } else {
            std::error_code ec;
            if (std::filesystem::is_directory(u, ec) && !ec)
                detail::emitLine(rep.err, std::format("No files matched: {}", u));
            else {
                detail::emitLine(rep.err, std::format("playlist add: no such file: {}", u));
                hardFail = true;
            }
        }
    }
    for (auto& f : files) {
        caudio::ipc::Command acmd{caudio::ipc::LibraryAdd{.path = f, .recursive = false}};
        auto ares = impl_->sendRaw(acmd);
        if (!ares) {
            detail::renderErrorInto(rep.err, ares.error());
            hardFail = true;
            continue;
        }
        std::string fn = std::filesystem::path(f).filename().generic_string();
        caudio::ipc::Command scmd{caudio::ipc::LibrarySearch{.query = fn, .limit = 50}};
        auto sres = impl_->sendRaw(scmd);
        if (!sres) {
            detail::renderErrorInto(rep.err, sres.error());
            hardFail = true;
            continue;
        }
        auto* sr = std::get_if<caudio::ipc::SearchResults>(&*sres);
        if (!sr) {
            detail::renderInto(rep.out, *sres);
            hardFail = true;
            continue;
        }
        bool found = false;
        for (auto& t : sr->tracks) {
            if (caudio::app::pathKey(t.path) == caudio::app::pathKey(f)) {
                resolved.push_back(t.id);
                found = true;
                break;
            }
        }
        if (!found) {
            detail::emitLine(rep.err, std::format("playlist add: not in library: {}", f));
            hardFail = true;
        }
    }
    int added = 0;
    caudio::ipc::QueueTracks collected{};
    for (auto tid : resolved) {
        caudio::ipc::Command cmd{caudio::ipc::PlaylistAdd{.pid = pid, .track_id = tid}};
        auto res = impl_->sendRaw(cmd);
        if (!res) {
            const auto& e = res.error();
            if (e.code == caudio::utils::StatusCode::AlreadyExists) {
                detail::emitLine(rep.err, std::format("already on playlist: track {}", tid));
                continue;
            }
            detail::renderErrorInto(rep.err, e);
            hardFail = true;
            continue;
        }
        caudio::ipc::Command gcmd{caudio::ipc::TagGet{.id = tid}};
        std::string label;
        if (auto gres = impl_->sendRaw(gcmd)) {
            if (auto* st = std::get_if<caudio::ipc::SingleTrack>(&*gres)) {
                collected.tracks.push_back(st->track);
                label = caudio::app::addedLabel(st->track);
            }
        }
        if (label.empty())
            label = "track " + std::to_string(tid);
        detail::emitLine(rep.out, std::format("Added {} to playlist {}", label, pid));
        ++added;
    }
    rep.json = caudio::ipc::Result{std::move(collected)};
    detail::countLineText(added, rep.out);
    rep.exitCode = (added == 0 && hardFail) ? 1 : 0;
    return rep;
}

AppResult App::playlistLoad(std::int64_t pid, bool play, bool replace) {
    caudio::ipc::Command cmd{
        caudio::ipc::PlaylistLoad{.pid = pid, .play = play, .replace = replace}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* pl = std::get_if<caudio::ipc::PlaylistLoaded>(&*res)) {
        std::string line =
            replace ? std::format("Replaced queue {} with playlist {}", pl->queue_id, pid)
                    : std::format("Loaded playlist {} into queue {}", pid, pl->queue_id);
        if (play)
            line += std::format("\nPlaying {}", caudio::app::trackWho(pl->status));
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::playlistSave(const std::string& name, std::optional<std::int64_t> qid) {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistSave{.name = name, .queue_id = qid}};
    std::string line = std::format("Saved playlist '{}'", name);
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

AppResult App::playlistDelete(std::int64_t pid) {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistDelete{pid}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Deleted playlist {}", pid));
}

AppResult App::playlistRename(std::int64_t pid, const std::string& name) {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistRename{.pid = pid, .newName = name}};
    return impl_->confirm(impl_->sendRaw(cmd),
                          std::format("Renamed playlist {} to '{}'", pid, name));
}

BatchResult App::playlistExport(std::int64_t pid, const std::string& path,
                                const std::string& format) {
    caudio::ipc::Command cmd{
        caudio::ipc::PlaylistExport{.pid = pid, .path = path, .format = format}};
    caudio::client::Client client{impl_->config_.dbPath, impl_->config_.socketPath};
    auto timeout = std::chrono::milliseconds{5000};
    auto cliRes = client.send(cmd, timeout);
    if (!cliRes)
        return BatchReport::fail(std::format("export: {}", cliRes.error().message));
    if (std::holds_alternative<caudio::utils::Error>(*cliRes)) {
        return BatchReport::fail(
            std::format("export: {}", std::get<caudio::utils::Error>(*cliRes).message));
    }
    BatchReport rep;
    if (std::holds_alternative<caudio::ipc::PlaylistData>(*cliRes)) {
        auto& pd = std::get<caudio::ipc::PlaylistData>(*cliRes);
        std::ofstream ofs(path);
        if (!ofs)
            return BatchReport::fail(std::format("export: failed to open output file '{}'", path));
        if (pd.format == "m3u" || pd.format == "pls") {
            caudio::app::writePlaylistText(ofs, pd.tracks, pd.format);
        } else if (pd.format == "json") {
            caudio::app::writePlaylistJson(ofs, pd.tracks);
        }
        ofs.close();
        rep.out = std::format("exported {} tracks to {}", pd.tracks.size(), path);
        rep.exitCode = 0;
        return rep;
    }
    return BatchReport::fail("export: unexpected response from daemon");
}

AppResult App::playlistImport(const std::string& path, std::optional<std::string> name) {
    caudio::ipc::Command cmd{caudio::ipc::PlaylistImport{.path = path, .name = std::move(name)}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

} // namespace caudio::app
