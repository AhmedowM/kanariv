#include <algorithm>
#include <array>
#include <kanariv/db/database.hpp>
#include <kanariv/db/maintenance.hpp>
#include <kanariv/db/scan.hpp>
#include <kanariv/db/search.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <media/fingerprint.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fileref.h>
#include <attachedpictureframe.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <tstring.h>

#include "service_impl.hpp"
#include "service_status.hpp"
#include "tag_writer.hpp"

namespace kanariv::service {

using kanariv::engine::detail::ServiceAccess;
using namespace kanariv::ipc;
using kanariv::ipc::HistoryClear;
using kanariv::ipc::HistoryList;
using kanariv::ipc::Info;
using kanariv::ipc::LibraryAdd;
using kanariv::ipc::LibraryList;
using kanariv::ipc::LibraryMaintenance;
using kanariv::ipc::LibraryRemove;
using kanariv::ipc::LibraryScan;
using kanariv::ipc::LibrarySearch;
using kanariv::ipc::LibraryStats;
using kanariv::ipc::PlaylistAdd;
using kanariv::ipc::PlaylistCreate;
using kanariv::ipc::PlaylistDelete;
using kanariv::ipc::PlaylistExport;
using kanariv::ipc::PlaylistImport;
using kanariv::ipc::PlaylistList;
using kanariv::ipc::PlaylistLoad;
using kanariv::ipc::PlaylistRename;
using kanariv::ipc::PlaylistSave;
using kanariv::ipc::PlaylistTracks;
using kanariv::ipc::Preview;
using kanariv::ipc::TagEdit;
using kanariv::ipc::TagGet;
using kanariv::ipc::TrackRate;

namespace {

// Extract embedded cover art (APIC / FLAC picture / etc.) from an audio file
// using TagLib. Returns {raw_bytes, mime_type} or nullopt when no embedded
// picture is found or the file cannot be read.
std::optional<std::pair<std::vector<std::byte>, std::string>>
extractEmbeddedCoverArt(const std::filesystem::path& path, kanariv::utils::Logger* logger) {
    TagLib::FileRef file(path.generic_string().c_str());
    if (!file.file()) {
        if (logger)
            logger->debug("cover art: cannot open file {}", path.generic_string());
        return std::nullopt;
    }
    if (logger)
        logger->debug("cover art: file opened: {}", path.generic_string());

    // Helper: copy a TagLib ByteVector into a vector<byte>.
    auto copyBytes = [](const TagLib::ByteVector& bv) {
        std::vector<std::byte> out;
        out.reserve(bv.size());
        for (auto b : bv)
            out.push_back(static_cast<std::byte>(b));
        return out;
    };

    // ID3v2 (MP3, etc.) — Attached Picture Frame (APIC).
    // Construct ID3v2::Tag directly from the file so we find ID3v2 tags even
    // when file.tag() returns ID3v1 (which has no cover art support).
    TagLib::ID3v2::Tag id3v2(file.file(), 0);
    if (logger)
        logger->debug("cover art: ID3v2 tag read, {} APIC frames", id3v2.frameList("APIC").size());
    for (auto* f : id3v2.frameList("APIC")) {
        auto* apic = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(f);
        if (!apic || apic->type() != TagLib::ID3v2::AttachedPictureFrame::FrontCover)
            continue;
        auto mime = apic->mimeType().to8Bit(true);
        std::string mimeStr(mime.data(), mime.size());
        if (mimeStr.empty())
            mimeStr = "image/jpeg";
        if (logger)
            logger->debug("cover art: found ID3v2 APIC front cover, {} bytes", apic->picture().size());
        return std::make_pair(copyBytes(apic->picture()), mimeStr);
    }
    if (logger)
        logger->debug("cover art: no ID3v2 APIC front cover frame found");

    // FLAC — Picture blocks
    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file.file())) {
        auto pics = flac->pictureList();
        if (logger)
            logger->debug("cover art: FLAC file, {} pictures", pics.size());
        for (const auto* pic : pics) {
            if (pic->type() == TagLib::FLAC::Picture::FrontCover) {
                std::string mimeStr(pic->mimeType().to8Bit(true).data());
                if (mimeStr.empty())
                    mimeStr = "image/png";
                if (logger)
                    logger->debug("cover art: found FLAC front cover picture, {} bytes",
                                  pic->data().size());
                return std::make_pair(copyBytes(pic->data()), mimeStr);
            }
        }
    }

    if (logger)
        logger->debug("cover art: no cover art found in file {}", path.generic_string());
    return std::nullopt;
}

} // namespace


namespace detail {
double durationFromDecoder(const std::filesystem::path& path) noexcept;
} // namespace detail

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryScan& cmd) {
    std::filesystem::path root;
    if (cmd.path)
        root = std::filesystem::path(*cmd.path);
    else {
        auto pp = config_.dbPath.parent_path();
        if (pp.empty())
            pp = std::filesystem::current_path();
        root = pp / "music";
    }
    auto mode = (cmd.full_hash ? kanariv::media::ScanMode::Full : kanariv::media::ScanMode::Sampled);
    // Prefer scanLibrary if a library matches root -- gives dedup + batched
    // transaction
    if (auto libs = db_->listLibraries(); libs) {
        for (auto& l : *libs) {
            if (std::filesystem::path(l.path) == root) {
                std::size_t before = 0;
                if (auto st = db_->getStats())
                    before = static_cast<std::size_t>(st->num_tracks);
                auto sr = kanariv::db::scanLibrary(*db_, l.id, {}, mode);
                if (!sr)
                    return std::unexpected{sr.error()};
                std::size_t after = before;
                if (auto st = db_->getStats())
                    after = static_cast<std::size_t>(st->num_tracks);
                return Result{kanariv::ipc::ScanReport{after >= before ? after - before : 0}};
            }
        }
    }
    // Fallback: simple insert (batched path uses scanLibrary above which is already
    // per-500 transactional). Files already recorded with matching size+mtime
    // are skipped, so rescans (including cross-mode ones) do not duplicate
    // untouched files. This is the path every first scan takes (no library
    // row matches yet), so it reports its own summary like scanLibrary does.
    if (logger_)
        logger_->info("scan started root={} mode={} (direct)", root.generic_string(),
                      cmd.full_hash ? "full" : "sampled");
    std::size_t n = 0;
    std::size_t seen = 0;
    for (auto t : kanariv::media::scanTracks(root, mode)) {
        ++seen;
        if (auto ex = db_->findByPath(t.path); ex && ex->size == t.size && ex->mtime == t.mtime)
            continue;
        auto r = db_->insertTrack(t);
        if (r)
            ++n;
    }
    if (logger_)
        logger_->info("scan finished root={} scanned={} added={}", root.generic_string(), seen, n);
    return Result{kanariv::ipc::ScanReport{n}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibrarySearch& cmd) {
    auto tracks = kanariv::db::search(*db_, cmd.query, cmd.limit);
    if (!tracks)
        return std::unexpected{tracks.error()};
    // fallback to like handled inside search; if empty still return
    std::span<const kanariv::types::Track> span{*tracks};
    std::vector<kanariv::types::Track> out(span.begin(), span.end());
    return Result{kanariv::ipc::SearchResults{.query=cmd.query, .tracks=std::move(out)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryStats& cmd) {
    auto st = db_->libraryStats(cmd.top_tracks, cmd.top_albums, cmd.top_genres, cmd.top_artists);
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
    auto tracks = db_->listTracks(&q);
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
    auto tr = db_->getTrack(track_id);
    if (!tr)
        return std::unexpected{tr.error()};
    kanariv::ipc::TrackInfo ti{};
    ti.track = std::move(*tr);
    ti.play_count = ti.track.play_count;
    ti.last_played = ti.track.last_played;
    return Result{std::move(ti)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::HistoryList& cmd) {
    int limit = cmd.limit.value_or(50);
    if (limit <= 0)
        limit = 50;
    auto hist = engine_->listHistory(limit);
    if (!hist)
        return std::unexpected{hist.error()};
    return Result{kanariv::ipc::History{std::move(*hist)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::HistoryClear&) {
    auto r = engine_->clearHistory();
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::BookmarkList& cmd) {
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    auto list = db_->listBookmarks(tid);
    if (!list)
        return std::unexpected{list.error()};
    return Result{kanariv::ipc::Bookmarks{std::move(*list)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::BookmarkAdd& cmd) {
    if (cmd.position_ms < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "bookmark: negative position")};
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    if (auto tr = db_->getTrack(tid); !tr)
        return std::unexpected{tr.error()};
    auto r = db_->bookmarkAdd(tid, cmd.position_ms, cmd.note);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::BookmarkRemove& cmd) {
    auto r = db_->bookmarkRemove(cmd.id);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::BookmarkGoto& cmd) {
    auto b = db_->bookmarkGet(cmd.id);
    if (!b)
        return std::unexpected{b.error()};
    auto pr = engine_->playTrack(b->track_id);
    if (!pr)
        return std::unexpected{pr.error()};
    if (auto sr = engine_->seek(static_cast<double>(b->position_ms) / 1000.0); !sr)
        return std::unexpected{sr.error()};
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryAdd& cmd) {
    if (cmd.path.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "library add: missing path")};
    std::filesystem::path p(cmd.path);
    std::error_code ec;
    bool exists = std::filesystem::exists(p, ec);
    if (ec || !exists)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                        "path not found: " + cmd.path)};
    auto addSingleFile =
        [&](const std::filesystem::path& fp) -> std::expected<void, kanariv::utils::Error> {
        if (!kanariv::media::hasAudioExt(fp))
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Unsupported, "unsupported file type: " + fp.string())};
        auto fpRes = kanariv::media::internal::computeFingerprint(fp);
        if (!fpRes)
            return std::unexpected{fpRes.error()};
        kanariv::types::Track t;
        t.path = fp.generic_string();
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
        auto ins = db_->insertTrack(t);
        if (!ins) {
            if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
                auto existing = db_->findByFingerprint(t.fingerprint);
                if (existing)
                    return {};
                auto byPath = db_->findByPath(t.path);
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
    // try numeric id
    bool isNum = !q.empty();
    for (size_t i = (q[0] == '-' ? 1 : 0); i < q.size() && isNum; ++i)
        if (!std::isdigit(static_cast<unsigned char>(q[i])))
            isNum = false;
    if (isNum) {
        try {
            int64_t id = std::stoll(q);
            if (id != 0) {
                auto r = db_->deleteTrack(id);
                if (r)
                    return Result{Empty{}};
                if (r.error().code != kanariv::utils::StatusCode::NotFound)
                    return std::unexpected{r.error()};
                // fallthrough to path lookup
            }
        } catch (...) {
        }
    }
    // try by path (separator-insensitive: stored rows mix native and
    // generic forms depending on which command added them)
    auto byPath = db_->findByPath(cmd.query);
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
            byPath = db_->findByPath(alt);
    }
    if (byPath) {
        auto r = db_->deleteTrack(byPath->id);
        if (!r)
            return std::unexpected{r.error()};
        return Result{Empty{}};
    }
    // also try trimmed path
    if (q != cmd.query) {
        auto byPath2 = db_->findByPath(q);
        if (byPath2) {
            auto r = db_->deleteTrack(byPath2->id);
            if (!r)
                return std::unexpected{r.error()};
            return Result{Empty{}};
        }
    }
    return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                    "track not found: " + cmd.query)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::TagEdit& cmd) {
    if (cmd.id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "tag edit: invalid id")};
    static const std::array<std::string_view, 8> allowed{
        "title", "artist", "album", "album_artist", "genre", "year", "track_number", "disc_number"};
    bool ok = false;
    for (auto a : allowed)
        if (a == cmd.field) {
            ok = true;
            break;
        }
    if (!ok)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "invalid field: " + cmd.field)};
    auto trRes = db_->getTrack(cmd.id);
    if (!trRes)
        return std::unexpected{trRes.error()};
    kanariv::types::Track t = std::move(*trRes);
    if (cmd.field == "title")
        t.title = cmd.value;
    else if (cmd.field == "artist")
        t.artist = cmd.value;
    else if (cmd.field == "album")
        t.album = cmd.value;
    else if (cmd.field == "album_artist")
        t.album_artist = cmd.value;
    else if (cmd.field == "genre")
        t.genre = cmd.value;
    else if (cmd.field == "year") {
        try {
            size_t pos = 0;
            int v = std::stoi(cmd.value, &pos);
            if (pos != cmd.value.size())
                throw std::invalid_argument("extra");
            t.year = v;
        } catch (...) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                            "invalid year: " + cmd.value)};
        }
    } else if (cmd.field == "track_number") {
        try {
            size_t pos = 0;
            int v = std::stoi(cmd.value, &pos);
            if (pos != cmd.value.size())
                throw std::invalid_argument("extra");
            t.track_num = v;
        } catch (...) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                            "invalid track_number: " + cmd.value)};
        }
    } else if (cmd.field == "disc_number") {
        try {
            size_t pos = 0;
            int v = std::stoi(cmd.value, &pos);
            if (pos != cmd.value.size())
                throw std::invalid_argument("extra");
            t.disc_num = v;
        } catch (...) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                            "invalid disc_number: " + cmd.value)};
        }
    }
    // File-first-then-rescan: the audio file is the source of truth, so the
    // DB row can never diverge from it silently.
    if (!detail::canWriteFileTags(t.path)) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                     "tag write not supported for this format: " +
                                         std::filesystem::path(t.path).extension().string())};
    }
    // A loaded (playing or paused) track holds the file open through the
    // decoder: suspend to release the handles, write + re-register, reopen.
    auto& eng = *engine_;
    bool loaded = (eng.state() == kanariv::types::PlaybackState::Playing ||
                   eng.state() == kanariv::types::PlaybackState::Paused) &&
                  eng.currentTrackId() == t.id;
    double resumePos = 0.0;
    bool wasPaused = false;
    if (loaded) {
        resumePos = eng.position();
        wasPaused = (eng.state() == kanariv::types::PlaybackState::Paused);
        if (auto s = ServiceAccess::suspendCurrentTrack(eng); !s)
            return std::unexpected{s.error()};
    }
    auto wr = detail::writeFileTags(t.path, t);
    if (!wr) {
        // Reopen either way so playback is never left suspended; the DB row
        // is untouched and still matches the (unmodified) file.
        if (logger_)
            logger_->warn("tag write failed file={} field={} err={}", t.path, cmd.field,
                          wr.error().message);
        if (loaded)
            (void)ServiceAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
        return std::unexpected{wr.error()};
    }
    auto reg = detail::reregisterTrackRow(*db_, t);
    if (!reg) {
        if (loaded)
            (void)ServiceAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
        return std::unexpected{reg.error()};
    }
    if (loaded) {
        if (auto r = ServiceAccess::resumeAfterTagWrite(eng, *reg, resumePos, wasPaused); !r)
            return std::unexpected{r.error()};
    }
    if (logger_)
        logger_->info("tag edit id={} field={} file={} suspended={}", cmd.id, cmd.field, t.path,
                      loaded);
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::TagGet& cmd) {
    int64_t trackId = 0;
    if (cmd.id.has_value()) {
        trackId = *cmd.id;
    } else if (cmd.path.has_value()) {
        // Resolve path to track id.
        auto tr = db_->findByPath(*cmd.path);
        if (!tr)
            return std::unexpected{tr.error()};
        trackId = tr->id;
    } else {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                     "tag get: id or path required")};
    }
    auto tr = db_->getTrack(trackId);
    if (!tr) {
        if (tr.error().message.empty())
            return std::unexpected{kanariv::utils::makeError(
                tr.error().code, "track not found: " + std::to_string(trackId))};
        return std::unexpected{tr.error()};
    }
    kanariv::ipc::SingleTrack st{.track=std::move(*tr)};
    if (cmd.cover) {
        // Cover art: prefer the stored cover_art_path file, then fall back to
        // embedded cover art (APIC / FLAC picture / MP4 cover art / etc.)
        // extracted from the audio file itself.
        if (!st.track.cover_art_path.empty()) {
            std::ifstream ifs(st.track.cover_art_path, std::ios::binary);
            if (ifs) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                if (!content.empty()) {
                    st.cover_mime =
                        kanariv::utils::Base64::mimeTypeFromPath(st.track.cover_art_path);
                    std::vector<std::byte> data;
                    data.reserve(content.size());
                    for (char c : content)
                        data.push_back(static_cast<std::byte>(c));
                    st.cover_data = kanariv::utils::Base64::encode(
                        std::span<const std::byte>(data.data(), data.size()));
                }
            }
        }
        if (st.cover_data.empty() && !st.track.path.empty()) {
            auto embedded = extractEmbeddedCoverArt(st.track.path, logger_.get());
            if (embedded) {
                st.cover_data = kanariv::utils::Base64::encode(
                    std::span<const std::byte>(embedded->first.data(), embedded->first.size()));
                st.cover_mime = embedded->second;
            }
        }
        if (st.cover_data.empty())
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound,
                "tag get: no cover art for track " + std::to_string(trackId))};
    }
    return Result{std::move(st)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistList&) {
    auto pls = db_->listPlaylists();
    if (!pls)
        return std::unexpected{pls.error()};
    std::span<const kanariv::types::Playlist> span{*pls};
    std::vector<kanariv::types::Playlist> out(span.begin(), span.end());
    return Result{Playlists{std::move(out)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistTracks& cmd) {
    auto tracks = db_->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    std::span<const kanariv::types::Track> span{*tracks};
    std::vector<kanariv::types::Track> out(span.begin(), span.end());
    return Result{PlaylistData{.tracks=std::move(out), .format={}}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistLoad& cmd) {
    auto tracks = db_->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    auto pl = db_->getPlaylist(cmd.pid);
    if (!pl)
        return std::unexpected{pl.error()};
    int64_t qid;
    if (cmd.replace) {
        // Replace the active queue (legacy behavior targeted queue 1).
        qid = engine_->activeQueueId();
        auto clr = db_->queueClear(qid);
        if (!clr)
            return std::unexpected{clr.error()};
    } else {
        auto created = db_->createQueue("playlist: " + pl->name);
        if (!created)
            return std::unexpected{created.error()};
        qid = *created;
    }
    std::vector<int64_t> ids;
    ids.reserve(tracks->size());
    for (auto& t : std::span<const kanariv::types::Track>(*tracks))
        ids.push_back(t.id);
    if (!ids.empty()) {
        auto er = db_->queueEnqueueBatch(qid, ids);
        if (!er)
            return std::unexpected{er.error()};
    }
    ServiceAccess::noteEnqueued(*engine_, qid, ids.size());
    if (cmd.play) {
        auto sw = engine_->switchQueue(qid);
        if (!sw)
            return std::unexpected{sw.error()};
        auto pr = engine_->play(qid);
        if (!pr)
            return std::unexpected{pr.error()};
    }
    updateShmStatus();
    auto st = detail::buildStatus(*engine_, *db_);
    if (!st)
        return std::unexpected{st.error()};
    return Result{kanariv::ipc::PlaylistLoaded{.queue_id=qid, .status=std::move(*st)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistSave& cmd) {
    if (cmd.name.empty())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "empty playlist name")};
    auto pidRes = db_->createPlaylist(cmd.name);
    if (!pidRes)
        return std::unexpected{pidRes.error()};
    int64_t pid = *pidRes;
    int64_t qid = cmd.queue_id.value_or(1);
    auto items = db_->listQueueItems(qid);
    if (!items)
        return std::unexpected{items.error()};
    // Queues may hold legacy duplicates; playlists forbid them.
    std::vector<int64_t> seen;
    seen.reserve(items->size());
    for (auto& it : std::span<const kanariv::types::QueueItem>(*items)) {
        if (std::find(seen.begin(), seen.end(), it.track_id) != seen.end())
            continue;
        seen.push_back(it.track_id);
        auto r = db_->playlistAddTrack(pid, it.track_id);
        if (!r)
            return std::unexpected{r.error()};
    }
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistCreate& cmd) {
    if (cmd.name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "playlist create: missing name")};
    int type = cmd.smart_query.empty() ? 0 : 1;
    auto id = db_->createPlaylist(cmd.name, type, cmd.smart_query);
    if (!id)
        return std::unexpected{id.error()};
    return Result{kanariv::ipc::PlaylistCreated{.id=*id, .name=cmd.name}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistAdd& cmd) {
    auto pl = db_->getPlaylist(cmd.pid);
    if (!pl)
        return std::unexpected{pl.error()};
    auto tr = db_->getTrack(cmd.track_id);
    if (!tr)
        return std::unexpected{tr.error()};
    if (auto items = db_->playlistGetTracks(cmd.pid)) {
        for (auto& t : std::span<const kanariv::types::Track>(*items)) {
            if (t.id == cmd.track_id) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::AlreadyExists, "already on playlist")};
            }
        }
    }
    auto r = db_->playlistAddTrack(cmd.pid, cmd.track_id);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistDelete& cmd) {
    auto r = db_->deletePlaylist(cmd.pid);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistRename& cmd) {
    if (cmd.newName.empty())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "empty playlist name")};
    auto r = db_->renamePlaylist(cmd.pid, cmd.newName);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistExport& cmd) {
    auto tracks = db_->playlistGetTracks(cmd.pid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    std::vector<kanariv::types::Track> out;
    out.reserve(tracks->size());
    for (auto& t : std::span<const kanariv::types::Track>(*tracks))
        out.push_back(std::move(t));
    return Result{PlaylistData{.tracks=std::move(out), .format=cmd.format}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlaylistImport& cmd) {
    std::filesystem::path path{cmd.path};
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
    auto findTrack = [&](const std::string& line, kanariv::types::Track& out) -> bool {
        if (auto tr = db_->findByPath(line)) {
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
            if (auto tr = db_->findByPath(alt)) {
                out = std::move(*tr);
                return true;
            }
        }
        return false;
    };
    size_t matched = 0, skipped = 0, duplicates = 0;
    for (const auto& line : lines) {
        kanariv::types::Track tr;
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
    std::string name = cmd.name.value_or(path.stem().string());
    auto pidRes = db_->createPlaylistFromTracks(name, trackIds);
    if (!pidRes)
        return std::unexpected{pidRes.error()};
    kanariv::ipc::PlaylistImportReport rep{};
    rep.pid = *pidRes;
    rep.name = name;
    rep.matched = matched;
    rep.skipped = skipped;
    rep.duplicates = duplicates;
    auto tracksRes = db_->playlistGetTracks(*pidRes);
    if (tracksRes) {
        for (auto& t : std::span<const kanariv::types::Track>(*tracksRes))
            rep.tracks.push_back(std::move(t));
    }
    return Result{std::move(rep)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Preview&) {
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::TrackRate& cmd) {
    if (cmd.track_id == 0)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "track rate: invalid id")};
    if (cmd.rating < 0 || cmd.rating > 5)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "track rate: rating must be 0..5")};
    kanariv::db::Database& db = dynamic_cast<kanariv::db::Database&>(*db_);
    if (!db.getTrack(cmd.track_id))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NotFound, "track not found: " +
            std::to_string(cmd.track_id))};
    auto r = db.setRating(cmd.track_id, cmd.rating);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::LibraryMaintenance& cmd) {
    kanariv::db::Database& db = dynamic_cast<kanariv::db::Database&>(*db_);
    if (cmd.action == "report") {
        auto report = kanariv::db::libraryReport(db, cmd.library_id);
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
        kanariv::db::LibraryMaintenanceOptions opts{};
        opts.dry_run = cmd.dry_run;
        opts.prune_missing = true;
        auto report = kanariv::db::libraryApplyMaintenance(db, cmd.library_id, opts);
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
        "library maintenance: unknown action '" + cmd.action +
        "' (expected 'report' or 'prune')")};
}

} // namespace kanariv::service
