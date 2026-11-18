/**
 * @file dispatch_tag.cpp
 * @brief Tag read/write commands and embedded cover-art extraction.
 * @ingroup kanariv_service
 */

#include <attachedpictureframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <tstring.h>

#include <array>
#include <cstddef>
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
#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "service_impl.hpp"
#include "tag_writer.hpp"

namespace kanariv::service {

using kanariv::engine::detail::EngineAccess;
using namespace kanariv::ipc;

std::optional<std::pair<std::vector<std::byte>, std::string>>
extractEmbeddedCoverArt(const std::filesystem::path& path, kanariv::utils::Logger* logger) {
    // TagLib's FileRef takes a C-string in the local filesystem encoding, so
    // it cannot open names outside the ANSI code page. Use the UTF-8 form
    // (works for ASCII names, best effort otherwise) and never let the
    // conversion throw out of cover-art extraction.
    std::string pathStr;
    try {
        pathStr = kanariv::utils::detail::pathToUtf8(path);
    } catch (...) {
        return std::nullopt;
    }
    TagLib::FileRef file(pathStr.c_str());
    if (!file.file()) {
        if (logger)
            logger->debug("cover art: cannot open file {}", pathStr);
        return std::nullopt;
    }
    if (logger)
        logger->debug("cover art: file opened: {}", pathStr);

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
            logger->debug("cover art: found ID3v2 APIC front cover, {} bytes",
                          apic->picture().size());
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
        logger->debug("cover art: no cover art found in file {}",
                      kanariv::utils::detail::pathToUtf8(path));
    return std::nullopt;
}

kanariv::utils::Expected<kanariv::ipc::Result>
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
    auto trRes = storage_.tracks->getTrack(cmd.id);
    if (!trRes)
        return std::unexpected{trRes.error()};
    kanariv::types::TrackRow t = std::move(*trRes);
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
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "tag write not supported for this format: " +
                kanariv::utils::detail::pathToUtf8(
                    kanariv::utils::detail::pathFromUtf8(t.path).extension()))};
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
        if (auto s = EngineAccess::suspendCurrentTrack(eng); !s)
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
            (void)EngineAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
        return std::unexpected{wr.error()};
    }
    auto reg = detail::reregisterTrackRow(*storage_.tracks, t);
    if (!reg) {
        if (loaded)
            (void)EngineAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
        return std::unexpected{reg.error()};
    }
    if (loaded) {
        if (auto r = EngineAccess::resumeAfterTagWrite(eng, *reg, resumePos, wasPaused); !r)
            return std::unexpected{r.error()};
    }
    if (logger_)
        logger_->info("tag edit id={} field={} file={} suspended={}", cmd.id, cmd.field, t.path,
                      loaded);
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::TagBatchEdit& cmd) {
    static const std::array<std::string_view, 8> allowed{
        "title", "artist", "album", "album_artist", "genre", "year", "track_number", "disc_number"};

    TagBatchEditResult out{};
    for (const auto& entry : cmd.entries) {
        TagBatchEditEntryResult er{};
        er.field = entry.field;
        if (entry.path.has_value())
            er.path = *entry.path;

        // Resolve track id.
        int64_t id = 0;
        if (entry.id.has_value()) {
            id = *entry.id;
            er.id = id;
        } else if (entry.path.has_value()) {
            auto fpRes = storage_.tracks->findByPath(*entry.path);
            if (!fpRes) {
                er.ok = false;
                er.error = "track not found for path '" + *entry.path + "'";
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
            id = fpRes->id;
            er.id = id;
        } else {
            er.ok = false;
            er.error = "no id or path specified";
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }

        // Validate field.
        bool ok = false;
        for (auto a : allowed)
            if (a == entry.field) {
                ok = true;
                break;
            }
        if (!ok) {
            er.ok = false;
            er.error = "invalid field: " + entry.field;
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }

        if (cmd.dry_run) {
            er.ok = true;
            out.results.push_back(std::move(er));
            ++out.succeeded;
            continue;
        }

        // Fetch the track row, merge the field, write file tags, re-register.
        auto trRes = storage_.tracks->getTrack(id);
        if (!trRes) {
            er.ok = false;
            er.error = trRes.error().message;
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }
        kanariv::types::TrackRow t = std::move(*trRes);
        if (entry.field == "title")
            t.title = entry.value;
        else if (entry.field == "artist")
            t.artist = entry.value;
        else if (entry.field == "album")
            t.album = entry.value;
        else if (entry.field == "album_artist")
            t.album_artist = entry.value;
        else if (entry.field == "genre")
            t.genre = entry.value;
        else if (entry.field == "year") {
            try {
                size_t pos = 0;
                int v = std::stoi(entry.value, &pos);
                if (pos != entry.value.size())
                    throw std::invalid_argument("extra");
                t.year = v;
            } catch (...) {
                er.ok = false;
                er.error = "invalid year: " + entry.value;
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
        } else if (entry.field == "track_number") {
            try {
                size_t pos = 0;
                int v = std::stoi(entry.value, &pos);
                if (pos != entry.value.size())
                    throw std::invalid_argument("extra");
                t.track_num = v;
            } catch (...) {
                er.ok = false;
                er.error = "invalid track_number: " + entry.value;
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
        } else if (entry.field == "disc_number") {
            try {
                size_t pos = 0;
                int v = std::stoi(entry.value, &pos);
                if (pos != entry.value.size())
                    throw std::invalid_argument("extra");
                t.disc_num = v;
            } catch (...) {
                er.ok = false;
                er.error = "invalid disc_number: " + entry.value;
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
        }

        if (!detail::canWriteFileTags(t.path)) {
            er.ok = false;
            er.error = "tag write not supported for this format: " + t.path;
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }

        auto& eng = *engine_;
        bool loaded = (eng.state() == kanariv::types::PlaybackState::Playing ||
                       eng.state() == kanariv::types::PlaybackState::Paused) &&
                      eng.currentTrackId() == t.id;
        double resumePos = 0.0;
        bool wasPaused = false;
        if (loaded) {
            resumePos = eng.position();
            wasPaused = (eng.state() == kanariv::types::PlaybackState::Paused);
            if (auto s = EngineAccess::suspendCurrentTrack(eng); !s) {
                er.ok = false;
                er.error = s.error().message;
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
        }
        auto wr = detail::writeFileTags(t.path, t);
        if (!wr) {
            if (loaded)
                (void)EngineAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
            er.ok = false;
            er.error = wr.error().message;
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }
        auto reg = detail::reregisterTrackRow(*storage_.tracks, t);
        if (!reg) {
            if (loaded)
                (void)EngineAccess::resumeAfterTagWrite(eng, t, resumePos, wasPaused);
            er.ok = false;
            er.error = reg.error().message;
            out.results.push_back(std::move(er));
            ++out.failed;
            continue;
        }
        if (loaded) {
            if (auto r = EngineAccess::resumeAfterTagWrite(eng, *reg, resumePos, wasPaused); !r) {
                er.ok = false;
                er.error = r.error().message;
                out.results.push_back(std::move(er));
                ++out.failed;
                continue;
            }
        }
        er.ok = true;
        er.id = reg->id;
        out.results.push_back(std::move(er));
        ++out.succeeded;
    }
    return Result{std::move(out)};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::TagGet& cmd) {
    int64_t trackId = 0;
    if (cmd.id.has_value()) {
        trackId = *cmd.id;
    } else if (cmd.path.has_value()) {
        // Resolve path to track id.
        auto tr = storage_.tracks->findByPath(*cmd.path);
        if (!tr)
            return std::unexpected{tr.error()};
        trackId = tr->id;
    } else {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "tag get: id or path required")};
    }
    auto tr = storage_.tracks->getTrack(trackId);
    if (!tr) {
        if (tr.error().message.empty())
            return std::unexpected{kanariv::utils::makeError(
                tr.error().code, "track not found: " + std::to_string(trackId))};
        return std::unexpected{tr.error()};
    }
    kanariv::ipc::SingleTrack st{std::move(*tr)};
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
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                             "tag get: no cover art for track " +
                                                                 std::to_string(trackId))};
    }
    return Result{std::move(st)};
}
} // namespace kanariv::service
