/**
 * @file library.cpp
 * @brief Library command handlers (scan/search/stats/list/add/remove).
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders.
 */

#include <app/app_impl.hpp>
#include <app/detail.hpp>
#include <app/format_internal.hpp>
#include <cctype>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/client/output_formatter.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/parse.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace kanariv::app {

AppResult App::libraryScan(std::optional<std::string> path, bool fullHash) {
    kanariv::ipc::Command cmd{kanariv::ipc::LibraryScan{path, fullHash}};
    // LibraryScan carries the LongRunning transport policy, so the client
    // budget already covers a tree walk that outlives the interactive default.
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::libraryAnalyze(bool reanalyze, std::optional<std::int64_t> libraryId) {
    kanariv::ipc::LibraryAnalyze cmd{};
    cmd.reanalyze = reanalyze;
    cmd.library_id = libraryId;
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{std::move(cmd)}));
}

AppResult App::librarySearch(const std::string& query, int limit) {
    kanariv::ipc::Command cmd{kanariv::ipc::LibrarySearch{query, limit}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

BatchResult App::libraryStats(std::optional<int> topTracks, std::optional<int> topAlbums,
                              std::optional<int> topGenres, std::optional<int> topArtists,
                              const std::vector<std::string>& queues,
                              const std::vector<std::int64_t>& playlists) {
    BatchReport rep;
    kanariv::ipc::LibraryStats cmd{};
    cmd.top_tracks = topTracks;
    cmd.top_albums = topAlbums;
    cmd.top_genres = topGenres;
    cmd.top_artists = topArtists;
    auto statsRes = impl_->sendRaw(kanariv::ipc::Command{std::move(cmd)});
    if (!statsRes)
        return std::unexpected{statsRes.error()};
    rep.json = std::move(*statsRes);
    detail::appendBlock(rep.out, kanariv::client::formatResult(*rep.json));
    if (!queues.empty() || !playlists.empty())
        rep.out += '\n';
    if (!queues.empty()) {
        kanariv::ipc::Command queueCmd{kanariv::ipc::QueueQueues{}};
        auto res = impl_->sendRaw(queueCmd);
        if (!res)
            return std::unexpected{res.error()};
        auto* qs = std::get_if<kanariv::ipc::Queues>(&*res);
        if (!qs) {
            detail::appendBlock(rep.out, kanariv::client::formatResult(*res));
            return rep;
        }
        bool all = false;
        for (auto& q : queues) {
            if (q == "all") {
                all = true;
                break;
            }
        }
        if (all) {
            detail::appendBlock(rep.out, kanariv::client::formatResult(*res));
        } else {
            for (auto& q : queues) {
                const auto id = kanariv::utils::parseId(q);
                if (!id)
                    return BatchReport::fail(
                        std::format("library stats: bad queue selector '{}'", q));
                bool found = false;
                for (auto& e : qs->entries) {
                    if (e.id == *id) {
                        if (!rep.out.empty())
                            rep.out += '\n';
                        rep.out += std::format("Queue {} '{}': {} tracks", e.id, e.name, e.tracks);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return BatchReport::fail(std::format("library stats: no such queue: {}", *id));
            }
        }
    }
    for (auto pid : playlists) {
        kanariv::ipc::Command listCmd{kanariv::ipc::PlaylistTracks{pid}};
        auto res = impl_->sendRaw(listCmd);
        if (!res)
            return std::unexpected{res.error()};
        auto* pd = std::get_if<kanariv::ipc::PlaylistData>(&*res);
        if (!pd) {
            detail::appendBlock(rep.out, kanariv::client::formatResult(*res));
            continue;
        }
        std::string name;
        {
            kanariv::ipc::Command lcmd{kanariv::ipc::PlaylistList{}};
            auto lres = impl_->sendRaw(lcmd);
            if (lres) {
                if (auto* pl = std::get_if<kanariv::ipc::Playlists>(&*lres)) {
                    for (auto& p : pl->playlists) {
                        if (p.id == pid) {
                            name = p.name;
                            break;
                        }
                    }
                }
            }
        }
        if (name.empty())
            return BatchReport::fail(std::format("library stats: no such playlist: {}", pid));
        if (!rep.out.empty())
            rep.out += '\n';
        if (pd->tracks.size() == 1)
            rep.out += std::format("Playlist '{}': 1 track", name);
        else
            rep.out += std::format("Playlist '{}': {} tracks", name, pd->tracks.size());
    }
    return rep;
}

AppResult App::libraryList(const std::string& query, int limit, int offset, const std::string& sort,
                           const std::string& order, const std::string& artist,
                           const std::string& album, const std::string& genre,
                           std::optional<std::int32_t> rating) {
    kanariv::ipc::LibraryList cmd{};
    cmd.query = query.empty() ? std::nullopt : std::optional<std::string>{query};
    cmd.limit = limit;
    cmd.offset = offset;
    cmd.sort = sort;
    cmd.order = order;
    cmd.artist = artist.empty() ? std::nullopt : std::optional<std::string>{artist};
    cmd.album = album.empty() ? std::nullopt : std::optional<std::string>{album};
    cmd.genre = genre.empty() ? std::nullopt : std::optional<std::string>{genre};
    cmd.rating = rating;
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{std::move(cmd)}));
}

AppResult App::libraryFavorites(std::int32_t minRating, int limit) {
    kanariv::ipc::LibraryList cmd{};
    cmd.limit = limit;
    cmd.rating = minRating;
    kanariv::ipc::Command c{std::move(cmd)};
    return impl_->confirm(impl_->sendRaw(c));
}

AppResult App::libraryMaintenance(const std::string& action, bool dryRun, std::int64_t libraryId) {
    kanariv::ipc::LibraryMaintenance cmd{};
    cmd.action = action;
    cmd.dry_run = dryRun;
    cmd.library_id = libraryId;
    kanariv::ipc::Command c{std::move(cmd)};
    return impl_->confirm(impl_->sendRaw(c));
}

AppResult App::libraryAdd(const std::string& path, bool recursive) {
    kanariv::ipc::Command cmd{kanariv::ipc::LibraryAdd{path, recursive}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Added {} to library", path));
}

AppResult App::libraryRemove(const std::string& query) {
    // Resolve a label first so the confirmation names it.
    std::string label;
    if (const auto id = kanariv::utils::parseId(query); id && *id != 0) {
        kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.id = *id}};
        if (auto res = impl_->sendRaw(cmd)) {
            if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*res))
                label = kanariv::app::addedLabel(st->track);
        }
    }
    if (label.empty()) {
        std::string fn = std::filesystem::path(query).filename().generic_string();
        if (!fn.empty())
            label = fn;
    }
    kanariv::ipc::Command cmd{kanariv::ipc::LibraryRemove{query}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    std::string line = !label.empty() ? std::format("Removed from library {}", label)
                                      : std::format("Removed {} from library", query);
    return Outcome{std::move(*res), std::move(line), false, false};
}

AppResult App::trackRate(std::string idOrPath, std::int32_t rating) {
    int64_t id = 0;
    if (const auto parsed = kanariv::utils::parseId(idOrPath)) {
        id = *parsed;
    } else {
        // Resolve path to track id via TagGet{path}.
        kanariv::ipc::Command tgCmd{kanariv::ipc::TagGet{.path = idOrPath}};
        auto tgRes = impl_->sendRaw(tgCmd);
        if (!tgRes)
            return std::unexpected{tgRes.error()};
        if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
            id = st->track.id;
        else
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                          "track not found for path '" + idOrPath + "'")};
    }
    kanariv::ipc::Command cmd{kanariv::ipc::TrackRate{id, rating}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Rated track {} as {}", id, rating));
}

} // namespace kanariv::app
