/**
 * @file dispatch_playlist.cpp
 * @brief Playlist commands, including M3U/PLS/JSON import and export.
 * @ingroup kanariv_service
 */

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/status_format.hpp>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "service_impl.hpp"
#include "service_status.hpp"

namespace kanariv::service {

using kanariv::engine::detail::EngineAccess;
using namespace kanariv::ipc;

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistList&) {
    auto pls = storage_.playlists->listPlaylists();
    if (!pls)
        return std::unexpected{pls.error()};
    std::span<const kanariv::types::Playlist> span{*pls};
    std::vector<kanariv::types::Playlist> out(span.begin(), span.end());
    return Result{Playlists{std::move(out)}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistTracks& cmd) {
    auto tracks = storage_.playlists->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    std::span<const kanariv::types::TrackRow> span{*tracks};
    std::vector<kanariv::types::TrackRow> out(span.begin(), span.end());
    return Result{PlaylistData{std::move(out), {}}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistLoad& cmd) {
    auto tracks = storage_.playlists->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    auto pl = storage_.playlists->getPlaylist(cmd.pid);
    if (!pl)
        return std::unexpected{pl.error()};
    int64_t qid;
    if (cmd.replace) {
        // Replace the active queue (legacy behavior targeted queue 1).
        qid = engine_->activeQueueId();
        auto clr = storage_.queues->queueClear(qid);
        if (!clr)
            return std::unexpected{clr.error()};
    } else {
        auto created = storage_.queues->createQueue("playlist: " + pl->name);
        if (!created)
            return std::unexpected{created.error()};
        qid = *created;
    }
    std::vector<int64_t> ids;
    ids.reserve(tracks->size());
    for (auto& t : std::span<const kanariv::types::TrackRow>(*tracks))
        ids.push_back(t.id);
    if (!ids.empty()) {
        auto er = detail::enqueueInChunks(*storage_.queues, qid, ids);
        if (!er)
            return std::unexpected{er.error()};
    }
    EngineAccess::noteEnqueued(*engine_, qid, ids.size());
    if (cmd.play) {
        auto sw = engine_->switchQueue(qid);
        if (!sw)
            return std::unexpected{sw.error()};
        auto pr = engine_->play(qid);
        if (!pr)
            return std::unexpected{pr.error()};
    }
    noteStateChanged();
    auto st = detail::buildStatus(*engine_, storage_);
    if (!st)
        return std::unexpected{st.error()};
    return Result{kanariv::ipc::PlaylistLoaded{qid, std::move(*st)}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistSave& cmd) {
    if (cmd.name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "empty playlist name")};
    auto pidRes = storage_.playlists->createPlaylist(cmd.name);
    if (!pidRes)
        return std::unexpected{pidRes.error()};
    int64_t pid = *pidRes;
    int64_t qid = cmd.queue_id.value_or(1);
    auto items = storage_.queues->listQueueItems(qid);
    if (!items)
        return std::unexpected{items.error()};
    // Queues may hold legacy duplicates; playlists forbid them.
    std::vector<int64_t> seen;
    seen.reserve(items->size());
    for (auto& it : std::span<const kanariv::types::QueueItem>(*items)) {
        if (std::find(seen.begin(), seen.end(), it.track_id) != seen.end())
            continue;
        seen.push_back(it.track_id);
        auto r = storage_.playlists->playlistAddTrack(pid, it.track_id);
        if (!r)
            return std::unexpected{r.error()};
    }
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistCreate& cmd) {
    if (cmd.name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "playlist create: missing name")};
    int type = cmd.smart_query.empty() ? 0 : 1;
    auto id = storage_.playlists->createPlaylist(cmd.name, type, cmd.smart_query);
    if (!id)
        return std::unexpected{id.error()};
    return Result{kanariv::ipc::PlaylistCreated{*id, cmd.name}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistAdd& cmd) {
    auto pl = storage_.playlists->getPlaylist(cmd.pid);
    if (!pl)
        return std::unexpected{pl.error()};
    auto tr = storage_.tracks->getTrack(cmd.track_id);
    if (!tr)
        return std::unexpected{tr.error()};
    if (auto items = storage_.playlists->playlistGetTracks(cmd.pid)) {
        for (auto& t : std::span<const kanariv::types::TrackRow>(*items)) {
            if (t.id == cmd.track_id) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::AlreadyExists, "already on playlist")};
            }
        }
    }
    auto r = storage_.playlists->playlistAddTrack(cmd.pid, cmd.track_id);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistDelete& cmd) {
    auto r = storage_.playlists->deletePlaylist(cmd.pid);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistRename& cmd) {
    if (cmd.newName.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "empty playlist name")};
    auto r = storage_.playlists->renamePlaylist(cmd.pid, cmd.newName);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistExport& cmd) {
    auto tracks = storage_.playlists->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    std::vector<kanariv::types::TrackRow> out;
    out.reserve(tracks->size());
    for (auto& t : std::span<const kanariv::types::TrackRow>(*tracks))
        out.push_back(std::move(t));
    return Result{PlaylistData{std::move(out), cmd.format}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::PlaylistImport& cmd) {
    std::filesystem::path path = kanariv::utils::detail::pathFromUtf8(cmd.path);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "file not found: " + cmd.path)};
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    std::vector<std::string> lines;
    if (ext == ".json") {
        // Parse JSON format
        std::ifstream ifs(path);
        if (!ifs)
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "failed to open file")};
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        try {
            const kanariv::utils::Json j = kanariv::utils::Json::parse(content);
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const kanariv::utils::Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto trackExp = tracks.at(ti);
                    if (!trackExp)
                        continue;
                    const kanariv::utils::Json track = std::move(*trackExp);
                    if (track.contains("path") && track["path"].isString()) {
                        lines.push_back(track["path"].get<std::string>());
                    }
                }
            }
        } catch (const std::exception& e) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Corrupt, std::string("JSON parse error: ") + e.what())};
        }
    } else {
        // M3U/PLS format
        std::ifstream ifs(path);
        if (!ifs)
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "failed to open file")};
        std::string line;
        while (std::getline(ifs, line)) {
            // trim
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (line.empty() || line[0] == '#')
                continue;
            // Handle PLS format: File1=path, File2=path, etc.
            if (ext == ".pls") {
                std::size_t eq = line.find('=');
                if (eq != std::string::npos && eq + 1 < line.size()) {
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    // trim key and val
                    key.erase(0, key.find_first_not_of(" \t\r\n"));
                    key.erase(key.find_last_not_of(" \t\r\n") + 1);
                    val.erase(0, val.find_first_not_of(" \t\r\n"));
                    val.erase(val.find_last_not_of(" \t\r\n") + 1);
                    if (key.rfind("File", 0) == 0) { // starts with "File"
                        lines.push_back(val);
                    }
                }
            } else {
                // M3U format: just the path
                lines.push_back(line);
            }
        }
    }
    std::vector<int64_t> trackIds;
    trackIds.reserve(lines.size());
    // Separator-insensitive lookup: stored rows mix native and generic forms.
    auto findTrack = [&](const std::string& line, kanariv::types::TrackRow& out) -> bool {
        if (auto tr = storage_.tracks->findByPath(line)) {
            out = std::move(*tr);
            return true;
        }
        std::string alt = line;
        bool changed = false;
        for (char& c : alt) {
#ifdef _WIN32
            if (c == '/') {
                c = '\\';
                changed = true;
            }
#else
            if (c == '\\') {
                c = '/';
                changed = true;
            }
#endif
        }
        if (changed) {
            if (auto tr = storage_.tracks->findByPath(alt)) {
                out = std::move(*tr);
                return true;
            }
        }
        return false;
    };
    size_t matched = 0, skipped = 0, duplicates = 0;
    for (const auto& line : lines) {
        kanariv::types::TrackRow tr;
        if (!findTrack(line, tr)) {
            ++skipped;
            continue;
        }
        if (std::find(trackIds.begin(), trackIds.end(), tr.id) != trackIds.end()) {
            ++duplicates;
            continue;
        }
        trackIds.push_back(tr.id);
        ++matched;
    }
    if (trackIds.empty()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "no matching tracks found in library")};
    }
    std::string name = cmd.name.value_or(kanariv::utils::detail::pathToUtf8(path.stem()));
    auto pidRes = storage_.playlists->createPlaylistFromTracks(name, trackIds);
    if (!pidRes)
        return std::unexpected{pidRes.error()};
    kanariv::ipc::PlaylistImportReport rep{};
    rep.pid = *pidRes;
    rep.name = name;
    rep.matched = matched;
    rep.skipped = skipped;
    rep.duplicates = duplicates;
    auto tracksRes = storage_.playlists->playlistGetTracks(*pidRes);
    if (tracksRes) {
        for (auto& t : std::span<const kanariv::types::TrackRow>(*tracksRes))
            rep.tracks.push_back(std::move(t));
    }
    return Result{std::move(rep)};
}
} // namespace kanariv::service
