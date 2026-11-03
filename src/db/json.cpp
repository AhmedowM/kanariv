#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/statement.hpp>
#include <db/transaction.hpp>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace kanariv::db {

constexpr int bindFingerprint = 1;
constexpr int bindPath = 2;
constexpr int bindSize = 3;
constexpr int bindMtime = 4;
constexpr int bindDuration = 5;
constexpr int bindSampleRate = 6;
constexpr int bindChannels = 7;
constexpr int bindBitrate = 8;
constexpr int bindTitle = 9;
constexpr int bindArtist = 10;
constexpr int bindAlbum = 11;
constexpr int bindAlbumArtist = 12;
constexpr int bindGenre = 13;
constexpr int bindYear = 14;
constexpr int bindTrackNum = 15;
constexpr int bindDiscNum = 16;
constexpr int bindCoverArtPath = 17;
constexpr int bindRating = 18;
constexpr int bindPlayCount = 19;
constexpr int bindLastPlayed = 20;
constexpr int bindDateAdded = 21;
constexpr int bindLastScanned = 22;
constexpr int bindDirty = 23;
constexpr int bindLibraryId = 24;
constexpr int bindSourceKind = 25;
constexpr int bindSourceKey = 26;
constexpr int bindSourceAvailable = 27;
constexpr int bindSourceDisplayName = 28;
constexpr int colDeletedAt = 29;
constexpr int defaultVolumeId = 1;

using kanariv::utils::Json;

auto exportJson(contracts::IDatabase& db_, const std::filesystem::path& outPath)
    -> std::expected<void, kanariv::utils::Error> {
    auto& db = dynamic_cast<Database&>(db_);
    auto tracks = db.listTracks(nullptr);
    if (!tracks) {
        return std::unexpected{tracks.error()};
    }
    Json root;
    root["tracks"] = Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (auto& t : *tracks) {
        root["tracks"].push_back(kanariv::utils::trackToJson(t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    std::ofstream f(outPath, std::ios::binary);
    if (!f) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open output")};
    }
    f << root.dump(2);
    if (!f) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "write failed")};
    }
    return {};
}

auto importJson(contracts::IDatabase& db_, const std::filesystem::path& inPath)
    -> std::expected<void, kanariv::utils::Error> {
    auto& db = dynamic_cast<Database&>(db_);
    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open input")};
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "empty file")};
    }
    Json root;
    try {
        root = Json::parse(content);
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
    if (!root.contains("tracks")) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    }
    const Json tracks = root["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (!tracks.isArray()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    }
    std::unique_lock lk{DatabaseAccess::mutex(db)};
    sqlite3* h = DatabaseAccess::handleLocked(db);
    if (h == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    auto txRes = DbTransaction::begin(h);
    if (!txRes) {
        return std::unexpected{txRes.error()};
    }
    DbTransaction tx = std::move(*txRes);
    bool corrupt = false;
    for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
        auto jt = tracks.at(ti);
        if (!jt) {
            corrupt = true;
            break;
        }
        auto tr = kanariv::utils::trackFromJson(*jt);
        if (!tr) {
            corrupt = true;
            break;
        }
        kanariv::types::Track t = *tr;
        SqliteStatement stmt;
        if (auto e =
                stmt.prepare(h, "INSERT INTO tracks (fingerprint, path, size, mtime, duration, "
                                "sample_rate, channels, bitrate, title, artist, album, "
                                "album_artist, genre, year, track_num, disc_num, "
                                "cover_art_path, rating, play_count, last_played, date_added, "
                                "last_scanned, dirty, library_id, source_kind, source_key, "
                                "source_available, source_display_name, deleted_at) VALUES "
                                "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
            !e) {
            corrupt = true;
            break;
        }
        auto fpSpan = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(t.fingerprint.data()), t.fingerprint.size()};
        stmt.bindBlob(bindFingerprint, fpSpan);
        stmt.bindText(bindPath, t.path);
        stmt.bindInt(bindSize, t.size);
        stmt.bindInt(bindMtime, t.mtime);
        stmt.bindDouble(bindDuration, t.duration);
        stmt.bindInt(bindSampleRate, t.sample_rate);
        stmt.bindInt(bindChannels, t.channels);
        stmt.bindInt(bindBitrate, t.bitrate);
        stmt.bindText(bindTitle, t.title);
        stmt.bindText(bindArtist, t.artist);
        stmt.bindText(bindAlbum, t.album);
        stmt.bindText(bindAlbumArtist, t.album_artist);
        stmt.bindText(bindGenre, t.genre);
        stmt.bindInt(bindYear, t.year);
        stmt.bindInt(bindTrackNum, t.track_num);
        stmt.bindInt(bindDiscNum, t.disc_num);
        stmt.bindText(bindCoverArtPath, t.cover_art_path);
        stmt.bindInt(bindRating, t.rating);
        stmt.bindInt(bindPlayCount, t.play_count);
        stmt.bindInt(bindLastPlayed, t.last_played);
        if (t.date_added != 0) {
            stmt.bindInt(bindDateAdded, t.date_added);
        } else {
            stmt.bindNull(bindDateAdded);
        }
        stmt.bindInt(bindLastScanned, t.last_scanned);
        stmt.bindInt(bindDirty, t.dirty ? 1 : 0);
        stmt.bindInt(bindLibraryId, t.library_id != 0 ? t.library_id : defaultVolumeId);
        const auto source = kanariv::types::normalizedSource(t);
        stmt.bindInt(bindSourceKind, static_cast<int>(source.kind));
        stmt.bindText(bindSourceKey, source.key);
        stmt.bindInt(bindSourceAvailable, source.available ? 1 : 0);
        stmt.bindText(bindSourceDisplayName, source.display_name);
        if (t.deleted_at != 0) {
            stmt.bindInt(colDeletedAt, t.deleted_at);
        } else {
            stmt.bindNull(colDeletedAt);
        }
        int rc = stmt.stepDone();
        if (rc == SQLITE_CONSTRAINT) {
            SqliteStatement sel;
            if (auto e = sel.prepare(h, "SELECT id FROM tracks WHERE fingerprint=?"); !e) {
                corrupt = true;
                break;
            }
            sel.bindBlob(1, fpSpan);
            if (sel.step()) {
                int64_t existing = sel.columnInt(0);
                SqliteStatement upd;
                if (auto e =
                        upd.prepare(h, "UPDATE tracks SET path=?, size=?, mtime=?, duration=?, "
                                       "sample_rate=?, channels=?, bitrate=?, title=?, artist=?, "
                                       "album=?, album_artist=?, genre=?, year=?, track_num=?, "
                                       "disc_num=?, cover_art_path=?, rating=?, play_count=?, "
                                       "last_played=?, date_added=?, last_scanned=?, dirty=?, "
                                       "library_id=?, source_kind=?, source_key=?, "
                                       "source_available=?, source_display_name=?, deleted_at=? "
                                       "WHERE id=?");
                    !e) {
                    corrupt = true;
                    break;
                }
                upd.bindText(bindPath, t.path);
                upd.bindInt(bindSize, t.size);
                upd.bindInt(bindMtime, t.mtime);
                upd.bindDouble(bindDuration, t.duration);
                upd.bindInt(bindSampleRate, t.sample_rate);
                upd.bindInt(bindChannels, t.channels);
                upd.bindInt(bindBitrate, t.bitrate);
                upd.bindText(bindTitle, t.title);
                upd.bindText(bindArtist, t.artist);
                upd.bindText(bindAlbum, t.album);
                upd.bindText(bindAlbumArtist, t.album_artist);
                upd.bindText(bindGenre, t.genre);
                upd.bindInt(bindYear, t.year);
                upd.bindInt(bindTrackNum, t.track_num);
                upd.bindInt(bindDiscNum, t.disc_num);
                upd.bindText(bindCoverArtPath, t.cover_art_path);
                upd.bindInt(bindRating, t.rating);
                upd.bindInt(bindPlayCount, t.play_count);
                upd.bindInt(bindLastPlayed, t.last_played);
                upd.bindInt(bindDateAdded, t.date_added);
                upd.bindInt(bindLastScanned, t.last_scanned);
                upd.bindInt(bindDirty, t.dirty ? 1 : 0);
                upd.bindInt(bindLibraryId, t.library_id != 0 ? t.library_id : defaultVolumeId);
                const auto updatedSource = kanariv::types::normalizedSource(t);
                upd.bindInt(bindSourceKind, static_cast<int>(updatedSource.kind));
                upd.bindText(bindSourceKey, updatedSource.key);
                upd.bindInt(bindSourceAvailable, updatedSource.available ? 1 : 0);
                upd.bindText(bindSourceDisplayName, updatedSource.display_name);
                if (t.deleted_at != 0) {
                    upd.bindInt(colDeletedAt, t.deleted_at);
                } else {
                    upd.bindNull(colDeletedAt);
                }
                upd.bindInt(bindFingerprint, existing);
                (void)upd.stepDone();
            } else {
                SqliteStatement sel2;
                if (auto e = sel2.prepare(h, "SELECT id FROM tracks WHERE path=?"); e) {
                    sel2.bindText(1, t.path);
                    if (sel2.step()) {
                    }
                }
            }
        } else if (rc != SQLITE_DONE) {
            corrupt = true;
            break;
        }
    }
    if (corrupt) {
        (void)tx.rollback();
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "import corrupt")};
    }
    if (auto c = tx.commit(); !c) {
        return std::unexpected{c.error()};
    }
    return {};
}

} // namespace kanariv::db
