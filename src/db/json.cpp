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
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace kanariv::db {

using kanariv::utils::Json;

std::expected<void, kanariv::utils::Error> exportJson(Database& db,
                                                      const std::filesystem::path& outPath) {
    auto tracks = db.listTracks();
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

std::expected<void, kanariv::utils::Error> importJson(Database& db,
                                                      const std::filesystem::path& inPath) {
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

    // Prepared once and reused for every row. Every failure is reported with
    // its row number and aborts the whole import (the transaction rolls back),
    // so no row is ever dropped silently.
    SqliteStatement insertStmt;
    if (auto e =
            insertStmt.prepare(h, "INSERT INTO tracks (fingerprint, path, size, mtime, duration, "
                                  "sample_rate, channels, bitrate, title, artist, album, "
                                  "album_artist, genre, year, track_num, disc_num, "
                                  "cover_art_path, rating, play_count, last_played, date_added, "
                                  "last_scanned, dirty, library_id, source_kind, source_key, "
                                  "source_available, source_display_name) VALUES "
                                  "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        !e) {
        return std::unexpected{e.error()};
    }
    SqliteStatement updateStmt;
    if (auto e = updateStmt.prepare(h, "UPDATE tracks SET path=?, size=?, mtime=?, duration=?, "
                                       "sample_rate=?, channels=?, bitrate=?, title=?, artist=?, "
                                       "album=?, album_artist=?, genre=?, year=?, track_num=?, "
                                       "disc_num=?, cover_art_path=?, rating=?, play_count=?, "
                                       "last_played=?, date_added=?, last_scanned=?, dirty=?, "
                                       "library_id=?, source_kind=?, source_key=?, "
                                       "source_available=?, source_display_name=? "
                                       "WHERE id=?");
        !e) {
        return std::unexpected{e.error()};
    }
    SqliteStatement findByFingerprint;
    if (auto e = findByFingerprint.prepare(h, "SELECT id FROM tracks WHERE fingerprint=?"); !e)
        return std::unexpected{e.error()};

    std::size_t inserted = 0;
    std::size_t updated = 0;
    for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
        const auto rowError = [ti](std::string_view detail) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                             "import row " + std::to_string(ti) +
                                                                 ": " + std::string(detail))};
        };
        auto jt = tracks.at(ti);
        if (!jt)
            return rowError("invalid track object");
        auto tr = kanariv::utils::trackFromJson(*jt);
        if (!tr)
            return rowError(tr.error().message);
        kanariv::types::TrackRow t = *tr;
        // A row without a path cannot be a track: reject it explicitly instead
        // of storing an unusable empty-path row.
        if (t.path.empty())
            return rowError("missing path");
        const auto fpSpan = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(t.fingerprint.data()), t.fingerprint.size()};

        insertStmt.reset();
        insertStmt.bindBlob(1, fpSpan);
        insertStmt.bindText(2, t.path);
        insertStmt.bindInt(3, t.size);
        insertStmt.bindInt(4, t.mtime);
        insertStmt.bindDouble(5, t.duration);
        insertStmt.bindInt(6, t.sample_rate);
        insertStmt.bindInt(7, t.channels);
        insertStmt.bindInt(8, t.bitrate);
        insertStmt.bindText(9, t.title);
        insertStmt.bindText(10, t.artist);
        insertStmt.bindText(11, t.album);
        insertStmt.bindText(12, t.album_artist);
        insertStmt.bindText(13, t.genre);
        insertStmt.bindInt(14, t.year);
        insertStmt.bindInt(15, t.track_num);
        insertStmt.bindInt(16, t.disc_num);
        insertStmt.bindText(17, t.cover_art_path);
        insertStmt.bindInt(18, t.rating);
        insertStmt.bindInt(19, t.play_count);
        insertStmt.bindInt(20, t.last_played);
        if (t.date_added)
            insertStmt.bindInt(21, t.date_added);
        else
            insertStmt.bindNull(21);
        insertStmt.bindInt(22, t.last_scanned);
        insertStmt.bindInt(23, t.dirty ? 1 : 0);
        insertStmt.bindInt(24, t.library_id ? t.library_id : 1);
        const auto source = kanariv::types::normalizedSource(t);
        insertStmt.bindInt(25, static_cast<int>(source.kind));
        insertStmt.bindText(26, source.key);
        insertStmt.bindInt(27, source.available ? 1 : 0);
        insertStmt.bindText(28, source.display_name);
        const int insertRc = insertStmt.stepDone();
        insertStmt.reset();
        if (insertRc == SQLITE_DONE) {
            ++inserted;
            continue;
        }
        if (insertRc != SQLITE_CONSTRAINT)
            return rowError(sqlite3_errmsg(h));

        // The only UNIQUE column is fingerprint: import updates the existing
        // row in place. A constraint without a matching row is a real error.
        findByFingerprint.reset();
        findByFingerprint.bindBlob(1, fpSpan);
        const bool found = findByFingerprint.step();
        const int64_t existing = found ? findByFingerprint.columnInt(0) : 0;
        findByFingerprint.reset();
        if (!found)
            return rowError("duplicate fingerprint has no matching row");

        updateStmt.reset();
        updateStmt.bindText(1, t.path);
        updateStmt.bindInt(2, t.size);
        updateStmt.bindInt(3, t.mtime);
        updateStmt.bindDouble(4, t.duration);
        updateStmt.bindInt(5, t.sample_rate);
        updateStmt.bindInt(6, t.channels);
        updateStmt.bindInt(7, t.bitrate);
        updateStmt.bindText(8, t.title);
        updateStmt.bindText(9, t.artist);
        updateStmt.bindText(10, t.album);
        updateStmt.bindText(11, t.album_artist);
        updateStmt.bindText(12, t.genre);
        updateStmt.bindInt(13, t.year);
        updateStmt.bindInt(14, t.track_num);
        updateStmt.bindInt(15, t.disc_num);
        updateStmt.bindText(16, t.cover_art_path);
        updateStmt.bindInt(17, t.rating);
        updateStmt.bindInt(18, t.play_count);
        updateStmt.bindInt(19, t.last_played);
        updateStmt.bindInt(20, t.date_added);
        updateStmt.bindInt(21, t.last_scanned);
        updateStmt.bindInt(22, t.dirty ? 1 : 0);
        updateStmt.bindInt(23, t.library_id ? t.library_id : 1);
        const auto updatedSource = kanariv::types::normalizedSource(t);
        updateStmt.bindInt(24, static_cast<int>(updatedSource.kind));
        updateStmt.bindText(25, updatedSource.key);
        updateStmt.bindInt(26, updatedSource.available ? 1 : 0);
        updateStmt.bindText(27, updatedSource.display_name);
        updateStmt.bindInt(28, existing);
        const int updateRc = updateStmt.stepDone();
        updateStmt.reset();
        if (updateRc != SQLITE_DONE)
            return rowError(std::string("update failed: ") + sqlite3_errmsg(h));
        ++updated;
    }
    if (auto c = tx.commit(); !c)
        return std::unexpected{c.error()};
    if (auto* logger = db.logger())
        logger->info("json import complete inserted={} updated={}", inserted, updated);
    return {};
}

std::expected<void, kanariv::utils::Error>
Database::exportJson(const std::filesystem::path& outPath) {
    return kanariv::db::exportJson(*this, outPath);
}

std::expected<void, kanariv::utils::Error>
Database::importJson(const std::filesystem::path& inPath) {
    return kanariv::db::importJson(*this, inPath);
}

} // namespace kanariv::db
