/**
 * @file dispatch_library.cpp
 * @brief Library, scan, search, rating and maintenance commands.
 * @ingroup kanariv_service
 * @details Tag editing lives in `dispatch_tag.cpp`, playlists in
 * `dispatch_playlist.cpp`, and history/bookmarks/lyrics in
 * `dispatch_lyrics_bookmark.cpp`.
 */

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/parse.hpp>
#include <kanariv/utils/status_format.hpp>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "service_impl.hpp"

namespace kanariv::service {

using namespace kanariv::ipc;

namespace detail {
double durationFromDecoder(const std::filesystem::path& path) noexcept;
} // namespace detail

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryScan& cmd) {
    std::filesystem::path root;
    if (cmd.path)
        // Command paths travel as UTF-8 (see src/utils/path_utf8.hpp).
        root = kanariv::utils::detail::pathFromUtf8(*cmd.path);
    else {
        auto pp = config_.dbPath.parent_path();
        if (pp.empty())
            pp = std::filesystem::current_path();
        root = pp / "music";
    }
    const auto mode =
        (cmd.full_hash ? kanariv::media::ScanMode::Full : kanariv::media::ScanMode::Sampled);
    // Prefer the scanner when the composition root wired one and a library
    // matches root -- gives dedup + batched transaction. `scanner` is optional
    // in Storage, so a backend without it falls through to the insert path.
    if (storage_.scanner) {
        if (auto libs = storage_.library->listLibraries(); libs) {
            for (auto& l : *libs) {
                if (kanariv::utils::detail::pathFromUtf8(l.path) == root) {
                    std::size_t before = 0;
                    if (auto st = storage_.library->getStats())
                        before = static_cast<std::size_t>(st->num_tracks);
                    const auto scanMode = cmd.full_hash ? kanariv::contracts::ScanMode::Full
                                                        : kanariv::contracts::ScanMode::Sampled;
                    auto sr = storage_.scanner->scanLibrary(l.id, {}, scanMode);
                    if (!sr) {
                        if (logger_)
                            logger_->error("scan failed root={} err={}", l.path,
                                           sr.error().message);
                        return std::unexpected{sr.error()};
                    }
                    std::size_t after = before;
                    if (auto st = storage_.library->getStats())
                        after = static_cast<std::size_t>(st->num_tracks);
                    return Result{kanariv::ipc::ScanReport{after >= before ? after - before : 0}};
                }
            }
        }
    }
    // Fallback: simple insert (the scanner path above is per-500 transactional).
    // Files already recorded with matching size+mtime are skipped, so rescans
    // (including cross-mode ones) do not duplicate untouched files. This is the
    // path every first scan takes (no library row matches yet), so it reports
    // its own summary like the scanner does.
    const std::string rootStr = kanariv::utils::detail::pathToUtf8(root);
    if (logger_)
        logger_->info("scan started root={} mode={} (direct)", rootStr,
                      cmd.full_hash ? "full" : "sampled");
    std::size_t n = 0;
    std::size_t seen = 0;
    std::string lastFile;
    try {
        for (auto t : kanariv::media::scanTracks(root, mode)) {
            ++seen;
            lastFile = t.path;
            if (auto ex = storage_.tracks->findByPath(t.path);
                ex && ex->size == t.size && ex->mtime == t.mtime)
                continue;
            auto r = storage_.tracks->insertTrack(t);
            if (r) {
                ++n;
            } else if (logger_) {
                // Report the file, not just a count: a single unreadable or
                // duplicate row should be traceable from the daemon log.
                logger_->warn("scan insert failed file={} err={}: {}", t.path,
                              kanariv::utils::toString(r.error().code), r.error().message);
            }
        }
    } catch (const std::exception& e) {
        // Without this the exception escaped into the IPC thread, which
        // closed the pipe and left the client with only
        // `ReadFile hdr failed: 109` (ERROR_BROKEN_PIPE).
        if (logger_)
            logger_->error("scan aborted after file={} err={}", lastFile, e.what());
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io,
            "scan aborted at '" + (lastFile.empty() ? rootStr : lastFile) + "': " + e.what())};
    }
    if (logger_)
        logger_->info("scan finished root={} scanned={} added={}", rootStr, seen, n);
    return Result{kanariv::ipc::ScanReport{n}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryAnalyze& cmd) {
    // Analysis decodes audio, so the port is only populated by the composition
    // root (kanariv::library::Analyzer); a bare backend reports the miswiring.
    if (!storage_.analyzer) {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "library analyze: no analyzer configured (start the daemon from the app)")};
    }
    auto report = storage_.analyzer->analyzeLibrary(
        cmd.library_id.value_or(0), kanariv::contracts::LibraryAnalyzeOptions{cmd.reanalyze});
    if (!report)
        return std::unexpected{report.error()};
    return Result{kanariv::ipc::AnalyzeReport{report->tracks, report->analyzed, report->failed,
                                              report->skipped}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibrarySearch& cmd) {
    auto tracks = storage_.library->searchTracks(cmd.query, cmd.limit);
    if (!tracks)
        return std::unexpected{tracks.error()};
    // fallback to like handled inside search; if empty still return
    std::span<const kanariv::types::TrackRow> span{*tracks};
    std::vector<kanariv::types::TrackRow> out(span.begin(), span.end());
    return Result{kanariv::ipc::SearchResults{cmd.query, std::move(out)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryStats& cmd) {
    auto st = storage_.library->libraryStats(cmd.top_tracks, cmd.top_albums, cmd.top_genres,
                                             cmd.top_artists);
    if (!st)
        return std::unexpected{st.error()};
    return Result{std::move(*st)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryList& cmd) {
    kanariv::types::TrackQuery q{};
    q.limit = cmd.limit;
    q.offset = cmd.offset;
    q.sort = cmd.sort;
    q.order = cmd.order;
    if (cmd.query.has_value())
        q.search = *cmd.query;
    if (cmd.artist.has_value())
        q.artist = *cmd.artist;
    if (cmd.album.has_value())
        q.album = *cmd.album;
    if (cmd.genre.has_value())
        q.genre = *cmd.genre;
    if (cmd.rating.has_value())
        q.rating = *cmd.rating;
    auto tracks = storage_.library->listTracks(q);
    if (!tracks)
        return std::unexpected{tracks.error()};
    return Result{Tracks{std::move(*tracks)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Info&) {
    // Get current track from engine status
    int64_t track_id = engine_->currentTrackId();
    if (track_id == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "no track currently playing")};
    }
    auto tr = storage_.tracks->getTrack(track_id);
    if (!tr)
        return std::unexpected{tr.error()};
    kanariv::ipc::TrackInfo ti{};
    ti.track = std::move(*tr);
    ti.play_count = ti.track.play_count;
    ti.last_played = ti.track.last_played;
    return Result{std::move(ti)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryAdd& cmd) {
    if (cmd.path.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "library add: missing path")};
    std::filesystem::path p = kanariv::utils::detail::pathFromUtf8(cmd.path);
    std::error_code ec;
    bool exists = std::filesystem::exists(p, ec);
    if (ec || !exists)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "path not found: " + cmd.path)};
    auto addSingleFile =
        [&](const std::filesystem::path& fp) -> std::expected<void, kanariv::utils::Error> {
        if (!kanariv::media::hasAudioExt(fp))
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Unsupported,
                "unsupported file type: " + kanariv::utils::detail::pathToUtf8(fp))};
        auto fpRes = kanariv::media::internal::computeFingerprint(fp);
        if (!fpRes)
            return std::unexpected{fpRes.error()};
        kanariv::types::TrackRow t;
        t.path = kanariv::utils::detail::pathToUtf8(fp);
        t.fingerprint = *fpRes;
        t.duration = detail::durationFromDecoder(fp);
        std::error_code ec2;
        auto sz = std::filesystem::file_size(fp, ec2);
        if (!ec2)
            t.size = static_cast<int64_t>(sz);
        auto ftime = std::filesystem::last_write_time(fp, ec2);
        if (!ec2)
            t.mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
        // Extract metadata (title, artist, album, etc.)
        if (auto meta = kanariv::media::extractMetadata(t.path); meta) {
            t.title = std::move(meta->title);
            t.artist = std::move(meta->artist);
            t.album = std::move(meta->album);
            t.album_artist = std::move(meta->album_artist);
            t.genre = std::move(meta->genre);
            t.year = meta->year;
            t.track_num = meta->track_num;
            t.disc_num = meta->disc_num;
            if (meta->duration > 0)
                t.duration = meta->duration;
            t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
            t.channels = static_cast<uint32_t>(meta->channels);
            t.bitrate = meta->bitrate;
        }
        auto ins = storage_.tracks->insertTrack(t);
        if (!ins) {
            if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
                auto existing = storage_.tracks->findByFingerprint(t.fingerprint);
                if (existing)
                    return {};
                auto byPath = storage_.tracks->findByPath(t.path);
                if (byPath)
                    return {};
            }
            return std::unexpected{ins.error()};
        }
        return {};
    };
    if (std::filesystem::is_regular_file(p, ec)) {
        auto r = addSingleFile(p);
        if (!r)
            return std::unexpected{r.error()};
        return Result{Empty{}};
    } else if (std::filesystem::is_directory(p, ec)) {
        std::size_t added = 0;
        std::error_code iterEc;
        if (cmd.recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, iterEc);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (iterEc)
                    break;
                std::error_code e3;
                if (it->is_regular_file(e3) && !e3 && kanariv::media::hasAudioExt(it->path())) {
                    auto r = addSingleFile(it->path());
                    if (r)
                        ++added;
                }
            }
        } else {
            for (auto it = std::filesystem::directory_iterator(p, iterEc);
                 it != std::filesystem::directory_iterator(); ++it) {
                if (iterEc)
                    break;
                std::error_code e3;
                if (it->is_regular_file(e3) && !e3 && kanariv::media::hasAudioExt(it->path())) {
                    auto r = addSingleFile(it->path());
                    if (r)
                        ++added;
                }
            }
        }
        if (added == 0) {
            // check if any audio files existed but failed?
            // Return Empty still if dir was empty -- not an error.
        }
        return Result{Empty{}};
    } else {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "not a file or directory: " + cmd.path)};
    }
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryRemove& cmd) {
    if (cmd.query.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "library remove: missing id")};
    std::string q = cmd.query;
    // trim
    q.erase(0, q.find_first_not_of(" \t\r\n"));
    q.erase(q.find_last_not_of(" \t\r\n") + 1);
    // A digits-only query is a library id; anything else falls through to a
    // path lookup, as before.
    if (const auto id = kanariv::utils::parseId(q); id && *id != 0) {
        auto r = storage_.tracks->deleteTrack(*id);
        if (r)
            return Result{Empty{}};
        if (r.error().code != kanariv::utils::StatusCode::NotFound)
            return std::unexpected{r.error()};
        // fallthrough to path lookup
    }
    // try by path (separator-insensitive: stored rows mix native and
    // generic forms depending on which command added them)
    auto byPath = storage_.tracks->findByPath(cmd.query);
    if (!byPath) {
        std::string alt = cmd.query;
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
        if (changed)
            byPath = storage_.tracks->findByPath(alt);
    }
    if (byPath) {
        auto r = storage_.tracks->deleteTrack(byPath->id);
        if (!r)
            return std::unexpected{r.error()};
        return Result{Empty{}};
    }
    // also try trimmed path
    if (q != cmd.query) {
        auto byPath2 = storage_.tracks->findByPath(q);
        if (byPath2) {
            auto r = storage_.tracks->deleteTrack(byPath2->id);
            if (!r)
                return std::unexpected{r.error()};
            return Result{Empty{}};
        }
    }
    return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                     "track not found: " + cmd.query)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Preview&) {
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::TrackRate& cmd) {
    if (cmd.track_id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "track rate: invalid id")};
    if (cmd.rating < 0 || cmd.rating > 5)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "track rate: rating must be 0..5")};
    if (!storage_.tracks->getTrack(cmd.track_id))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                      "track not found: " + std::to_string(cmd.track_id))};
    auto r = storage_.tracks->setRating(cmd.track_id, cmd.rating);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryMaintenance& cmd) {
    if (cmd.action == "report") {
        auto report = storage_.library->libraryReport(cmd.library_id);
        if (!report)
            return std::unexpected{report.error()};
        kanariv::ipc::LibraryMaintenanceReport rep{};
        rep.tracks_checked = report->tracks_checked;
        for (const auto& t : report->missing_tracks)
            rep.missing_tracks.push_back(t);
        rep.duplicate_groups = report->duplicate_groups.size();
        rep.duplicate_tracks = report->duplicateTracks();
        return Result{std::move(rep)};
    }
    if (cmd.action == "prune") {
        kanariv::types::LibraryMaintenanceOptions opts{};
        opts.dry_run = cmd.dry_run;
        opts.prune_missing = true;
        auto report = storage_.library->libraryApplyMaintenance(cmd.library_id, opts);
        if (!report)
            return std::unexpected{report.error()};
        if (cmd.dry_run) {
            kanariv::ipc::LibraryMaintenanceReport rep{};
            rep.tracks_checked = report->tracks_checked;
            for (const auto& t : report->missing_tracks)
                rep.missing_tracks.push_back(t);
            rep.duplicate_groups = report->duplicate_groups.size();
            rep.duplicate_tracks = report->duplicateTracks();
            return Result{std::move(rep)};
        }
        return Result{Empty{}};
    }
    return std::unexpected{kanariv::utils::makeError(
        kanariv::utils::StatusCode::InvalidArg,
        "library maintenance: unknown action '" + cmd.action + "' (expected 'report' or 'prune')")};
}

} // namespace kanariv::service
