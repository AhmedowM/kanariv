#include "tag_writer.hpp"

#include <apefile.h>
#include <apetag.h>
#include <asfattribute.h>
#include <asffile.h>
#include <asftag.h>
#include <fileref.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mp4item.h>
#include <mp4tag.h>
#include <mpcfile.h>
#include <mpegfile.h>
#include <opusfile.h>
#include <tag.h>
#include <textidentificationframe.h>
#include <tstring.h>
#include <vorbisfile.h>
#include <wavfile.h>
#include <wavpackfile.h>
#include <xiphcomment.h>

#include <algorithm>
#include <kanariv/db/database.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/utils/error.hpp>
#include <cctype>
#include <cstdint>
#include <media/fingerprint.hpp>
#include <string>
#include <system_error>

namespace kanariv::service::detail {
namespace {

TagLib::String toTagString(const std::string& s) {
    return TagLib::String(s, TagLib::String::UTF8);
}

// ID3v2 text frame: always remove first, re-add only when non-empty so
// clearing a field drops the frame (re-reads as empty).
void setId3v2Text(TagLib::ID3v2::Tag* t2, const char* id, const std::string& v) {
    t2->removeFrames(id);
    if (v.empty())
        return;
    auto* f = new TagLib::ID3v2::TextIdentificationFrame(id, TagLib::String::UTF8);
    f->setText(toTagString(v));
    t2->addFrame(f);
}

void setXiphText(TagLib::Ogg::XiphComment* xc, const char* key, const std::string& v) {
    // addField with an empty value removes the field; replace=true drops dupes.
    xc->addField(key, toTagString(v), true);
}

void setXiphExtras(TagLib::Ogg::XiphComment* xc, const kanariv::types::Track& t) {
    setXiphText(xc, "ALBUMARTIST", t.album_artist);
    setXiphText(xc, "DISCNUMBER", t.disc_num > 0 ? std::to_string(t.disc_num) : std::string{});
}

void setId3v2Extras(TagLib::ID3v2::Tag* t2, const kanariv::types::Track& t) {
    setId3v2Text(t2, "TPE2", t.album_artist);
    if (t.disc_num > 0)
        setId3v2Text(t2, "TPOS", std::to_string(t.disc_num));
    else
        t2->removeFrames("TPOS");
}

void setApeExtras(TagLib::APE::Tag* ape, const kanariv::types::Track& t) {
    if (t.album_artist.empty())
        ape->removeItem("Album Artist");
    else
        ape->addValue("Album Artist", toTagString(t.album_artist), true);
    if (t.disc_num > 0)
        ape->addValue("Disc", toTagString(std::to_string(t.disc_num)), true);
    else
        ape->removeItem("Disc");
}

// Forward declarations for helpers used by setFormatSpecificExtras.
void setMp4Extras(TagLib::MP4::Tag& tag, const kanariv::types::Track& t);
void setAsfExtras(TagLib::ASF::Tag& tag, const kanariv::types::Track& t);

// Opens a TagLib::FileRef for the given path.
TagLib::FileRef openTagFile(const std::filesystem::path& p) {
    return TagLib::FileRef(p.generic_string().c_str());
}

// Sets the common tag fields shared by all supported formats.
void setBasicTags(TagLib::Tag& tag, const kanariv::types::Track& t) {
    tag.setTitle(toTagString(t.title));
    tag.setArtist(toTagString(t.artist));
    tag.setAlbum(toTagString(t.album));
    tag.setGenre(toTagString(t.genre));
    tag.setYear(t.year < 0 ? 0 : static_cast<unsigned int>(t.year));
    tag.setTrack(t.track_num < 0 ? 0 : static_cast<unsigned int>(t.track_num));
}

// Applies format-specific extra fields (album artist, disc number) based on
// the concrete TagLib file/tag types discovered via dynamic_cast.
void setFormatSpecificExtras(TagLib::File& file, TagLib::Tag& tag,
                             const kanariv::types::Track& t) {
    bool id3v2Done = false;
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(&file)) {
        if (auto* t2 = mpeg->ID3v2Tag(true)) {
            setId3v2Extras(t2, t);
            id3v2Done = true;
        }
    } else if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(&file)) {
        // No create accessor: relies on the generic setters above having
        // materialized the chunk. Null-safe skip otherwise.
        if (auto* t2 = wav->ID3v2Tag()) {
            setId3v2Extras(t2, t);
            id3v2Done = true;
        }
    } else if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(&file)) {
        if (auto* xc = flac->xiphComment(true))
            setXiphExtras(xc, t);
    } else if (auto* vf = dynamic_cast<TagLib::Ogg::Vorbis::File*>(&file)) {
        setXiphExtras(vf->tag(), t);
    } else if (auto* of = dynamic_cast<TagLib::Ogg::Opus::File*>(&file)) {
        setXiphExtras(of->tag(), t);
    } else if (auto* apef = dynamic_cast<TagLib::APE::File*>(&file)) {
        if (auto* ape = apef->APETag(true))
            setApeExtras(ape, t);
    } else if (auto* mpc = dynamic_cast<TagLib::MPC::File*>(&file)) {
        if (auto* ape = mpc->APETag(true))
            setApeExtras(ape, t);
    } else if (auto* wv = dynamic_cast<TagLib::WavPack::File*>(&file)) {
        if (auto* ape = wv->APETag(true))
            setApeExtras(ape, t);
    }
    if (!id3v2Done) {
        if (auto* t2 = dynamic_cast<TagLib::ID3v2::Tag*>(&tag)) {
            setId3v2Extras(t2, t);
            id3v2Done = true;
        }
    }
    if (auto* m4 = dynamic_cast<TagLib::MP4::Tag*>(&tag)) {
        setMp4Extras(*m4, t);
    } else if (auto* asf = dynamic_cast<TagLib::ASF::Tag*>(&tag)) {
        setAsfExtras(*asf, t);
    }
}

// Sets MP4-specific album artist and disc number items.
void setMp4Extras(TagLib::MP4::Tag& tag, const kanariv::types::Track& t) {
    if (t.album_artist.empty())
        tag.removeItem("aART");
    else {
        TagLib::StringList sl;
        sl.append(toTagString(t.album_artist));
        tag.setItem("aART", TagLib::MP4::Item(sl));
    }
    if (t.disc_num > 0)
        tag.setItem("disk", TagLib::MP4::Item(t.disc_num, 0));
    else
        tag.removeItem("disk");
}

// Sets ASF-specific album artist and disc number attributes.
void setAsfExtras(TagLib::ASF::Tag& tag, const kanariv::types::Track& t) {
    if (t.album_artist.empty())
        tag.removeItem("WM/AlbumArtist");
    else
        tag.setAttribute("WM/AlbumArtist",
                         TagLib::ASF::Attribute(toTagString(t.album_artist)));
    if (t.disc_num > 0)
        tag.setAttribute("WM/PartOfSet",
                         TagLib::ASF::Attribute(toTagString(std::to_string(t.disc_num))));
    else
        tag.removeItem("WM/PartOfSet");
}

// Saves the TagLib file ref to disk.
bool saveTagFile(TagLib::FileRef& f) {
    return f.save();
}

} // namespace

bool canWriteFileTags(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // NOTE: no .tta — TrueAudio files carry a muxer-written APE tag that
    // FFmpeg prefers over ID3v2, and TagLib's TrueAudio class exposes no APE
    // API, so a write would be invisible to our own reader. DB-only, like
    // .tak/.shorten and the telephony formats.
    return ext == ".mp3" || ext == ".flac" || ext == ".m4a" || ext == ".mp4" || ext == ".ogg" ||
           ext == ".oga" || ext == ".opus" || ext == ".wma" || ext == ".asf" || ext == ".wav" ||
           ext == ".aiff" || ext == ".aif" || ext == ".ape" || ext == ".mpc" || ext == ".wv";
}

kanariv::utils::Expected<void> writeFileTags(const std::filesystem::path& p,
                                             const kanariv::types::Track& t) {
    if (!canWriteFileTags(p)) {
        return std::unexpected(kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "tag write not supported for this format: " + p.extension().string()));
    }
    TagLib::FileRef f = openTagFile(p);
    if (f.isNull() || !f.file() || !f.tag()) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                        "taglib cannot open file for tag write"));
    }
    setBasicTags(*f.tag(), t);
    setFormatSpecificExtras(*f.file(), *f.tag(), t);
    if (!saveTagFile(f)) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                        "taglib failed to save file tags"));
    }
    return {};
}

kanariv::utils::Expected<kanariv::types::Track> reregisterTrackRow(kanariv::contracts::IDatabase& db,
                                                                   kanariv::types::Track t) {
    auto fpRes = kanariv::media::internal::computeFingerprint(t.path);
    if (!fpRes)
        return std::unexpected{fpRes.error()};
    t.fingerprint = *fpRes;
    std::error_code ec;
    if (auto sz = std::filesystem::file_size(t.path, ec); !ec)
        t.size = static_cast<int64_t>(sz);
    if (auto mt = std::filesystem::last_write_time(t.path, ec); !ec)
        t.mtime = static_cast<int64_t>(mt.time_since_epoch().count());
    if (auto meta = kanariv::media::extractMetadata(t.path); meta) {
        if (meta->duration > 0)
            t.duration = meta->duration;
        t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
        t.channels = static_cast<uint32_t>(meta->channels);
        t.bitrate = meta->bitrate;
    }
    if (auto upd = db.updateTrack(t); !upd)
        return std::unexpected{upd.error()};
    return t;
}

} // namespace kanariv::service::detail
