#include <sqlite3.h>

#include <kanariv/db/database.hpp>
#include <kanariv/db/json.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
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
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace kanariv::db {

using kanariv::utils::Json;

std::expected<void, kanariv::utils::Error> exportJson(contracts::IDatabase& db_,
                                                     const std::filesystem::path& outPath) {
    Database& db = dynamic_cast<Database&>(db_);
    auto tracks = db.listTracks(nullptr);
    if (!tracks)
        return std::unexpected{tracks.error()};
    Json root;
    root["tracks"] = Json::array();
    for (auto& t : *tracks) {
        root["tracks"].push_back(kanariv::utils::trackToJson(t));
    }
    std::ofstream f(outPath, std::ios::binary);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open output")};
    f << root.dump(2);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "write failed")};
    return {};
}

std::expected<void, kanariv::utils::Error> importJson(contracts::IDatabase& db_,
                                                     const std::filesystem::path& inPath) {
    Database& db = dynamic_cast<Database&>(db_);
    std::ifstream f(inPath, std::ios::binary);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open input")};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "empty file")};
    Json root;
    try {
        root = Json::parse(content);
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
    if (!root.contains("tracks"))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    const Json tracks = root["tracks"];
    if (!tracks.isArray()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    }
    std::unique_lock lk{DatabaseAccess::mutex(db)};
    sqlite3* h = DatabaseAccess::handleLocked(db);
    if (!h)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = DbTransaction::begin(h);
    if (!txRes)
        return std::unexpected{txRes.error()};
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
        stmt.bindBlob(1, fpSpan);
        stmt.bindText(2, t.path);
        stmt.bindInt(3, t.size);
        stmt.bindInt(4, t.mtime);
        stmt.bindDouble(5, t.duration);
        stmt.bindInt(6, t.sample_rate);
        stmt.bindInt(7, t.channels);
        stmt.bindInt(8, t.bitrate);
        stmt.bindText(9, t.title);
        stmt.bindText(10, t.artist);
        stmt.bindText(11, t.album);
        stmt.bindText(12, t.album_artist);
        stmt.bindText(13, t.genre);
        stmt.bindInt(14, t.year);
        stmt.bindInt(15, t.track_num);
        stmt.bindInt(16, t.disc_num);
        stmt.bindText(17, t.cover_art_path);
        stmt.bindInt(18, t.rating);
        stmt.bindInt(19, t.play_count);
        stmt.bindInt(20, t.last_played);
        if (t.date_added)
            stmt.bindInt(21, t.date_added);
        else
            stmt.bindNull(21);
        stmt.bindInt(22, t.last_scanned);
        stmt.bindInt(23, t.dirty ? 1 : 0);
        stmt.bindInt(24, t.library_id ? t.library_id : 1);
        const auto source = kanariv::types::normalizedSource(t);
        stmt.bindInt(25, static_cast<int>(source.kind));
        stmt.bindText(26, source.key);
        stmt.bindInt(27, source.available ? 1 : 0);
        stmt.bindText(28, source.display_name);
        if (t.deleted_at)
            stmt.bindInt(29, t.deleted_at);
        else
            stmt.bindNull(29);
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
                upd.bindText(1, t.path);
                upd.bindInt(2, t.size);
                upd.bindInt(3, t.mtime);
                upd.bindDouble(4, t.duration);
                upd.bindInt(5, t.sample_rate);
                upd.bindInt(6, t.channels);
                upd.bindInt(7, t.bitrate);
                upd.bindText(8, t.title);
                upd.bindText(9, t.artist);
                upd.bindText(10, t.album);
                upd.bindText(11, t.album_artist);
                upd.bindText(12, t.genre);
                upd.bindInt(13, t.year);
                upd.bindInt(14, t.track_num);
                upd.bindInt(15, t.disc_num);
                upd.bindText(16, t.cover_art_path);
                upd.bindInt(17, t.rating);
                upd.bindInt(18, t.play_count);
                upd.bindInt(19, t.last_played);
                upd.bindInt(20, t.date_added);
                upd.bindInt(21, t.last_scanned);
                upd.bindInt(22, t.dirty ? 1 : 0);
                upd.bindInt(23, t.library_id ? t.library_id : 1);
                const auto updatedSource = kanariv::types::normalizedSource(t);
                upd.bindInt(24, static_cast<int>(updatedSource.kind));
                upd.bindText(25, updatedSource.key);
                upd.bindInt(26, updatedSource.available ? 1 : 0);
                upd.bindText(27, updatedSource.display_name);
                if (t.deleted_at)
                    upd.bindInt(28, t.deleted_at);
                else
                    upd.bindNull(28);
                upd.bindInt(29, existing);
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
    if (auto c = tx.commit(); !c)
        return std::unexpected{c.error()};
    return {};
}

} // namespace kanariv::db
