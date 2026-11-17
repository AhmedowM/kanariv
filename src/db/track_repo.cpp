/**
 * @file track_repo.cpp
 * @brief Track CRUD, path/source/fingerprint lookup and library-item refresh.
 * @ingroup kanariv_db
 */

#include <sqlite3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <db/fts.hpp>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <expected>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db_impl.hpp"

namespace kanariv::db {

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::findByPathLocked(std::string_view path) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::findBySourceLocked(const kanariv::types::SourceRef& source) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    if (source.key.empty()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "empty source key")};
    }
    std::string sql =
        std::string(internal::selectTracksCols) + " WHERE source_kind=? AND source_key=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, static_cast<int>(source.kind));
    st.bindText(2, source.key);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::findByFingerprintLocked(const std::array<uint8_t, 32>& fp) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<int64_t, kanariv::utils::Error>
Database::Impl::insertTrackLocked(const kanariv::types::TrackRow& t) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto sRes = getCachedForUse(insertTrackSql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    bindTrackForInsert(st, t);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::db::internal::sqliteError(db_.get(), rc)};
    return sqlite3_last_insert_rowid(db_.get());
}

std::expected<void, kanariv::utils::Error>
Database::Impl::updateTrackLocked(const kanariv::types::TrackRow& t) {
    if (t.id == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "id 0")};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto sRes = getCachedForUse(updateTrackSql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    bindTrackForUpdate(st, t);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(db_.get()))};
    if (sqlite3_changes(db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

std::expected<void, kanariv::utils::Error> Database::Impl::deleteTrackLocked(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "DELETE FROM tracks WHERE id=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, id);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(db_.get()))};
    if (sqlite3_changes(db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

void bindTrackForInsert(SqliteStatement& st, const kanariv::types::TrackRow& t) {
    st.bindBlob(1,
                std::span<const std::byte>{reinterpret_cast<const std::byte*>(t.fingerprint.data()),
                                           t.fingerprint.size()});
    st.bindText(2, t.path);
    st.bindInt(3, t.size);
    st.bindInt(4, t.mtime);
    st.bindDouble(5, t.duration);
    st.bindInt(6, t.sample_rate);
    st.bindInt(7, t.channels);
    st.bindInt(8, t.bitrate);
    st.bindText(9, t.title);
    st.bindText(10, t.artist);
    st.bindText(11, t.album);
    st.bindText(12, t.album_artist);
    st.bindText(13, t.genre);
    st.bindInt(14, t.year);
    st.bindInt(15, t.track_num);
    st.bindInt(16, t.disc_num);
    st.bindText(17, t.cover_art_path);
    st.bindInt(18, t.rating);
    st.bindInt(19, t.play_count);
    st.bindInt(20, t.last_played);
    if (t.date_added)
        st.bindInt(21, t.date_added);
    else
        st.bindNull(21);
    st.bindInt(22, t.last_scanned);
    st.bindInt(23, t.dirty ? 1 : 0);
    st.bindInt(24, t.library_id ? t.library_id : 1);
    const auto source = kanariv::types::normalizedSource(t);
    st.bindInt(25, static_cast<int>(source.kind));
    st.bindText(26, source.key);
    st.bindInt(27, source.available ? 1 : 0);
    st.bindText(28, source.display_name);
}

void bindTrackForUpdate(SqliteStatement& st, const kanariv::types::TrackRow& t) {
    st.bindBlob(1,
                std::span<const std::byte>{reinterpret_cast<const std::byte*>(t.fingerprint.data()),
                                           t.fingerprint.size()});
    st.bindText(2, t.path);
    st.bindInt(3, t.size);
    st.bindInt(4, t.mtime);
    st.bindDouble(5, t.duration);
    st.bindInt(6, t.sample_rate);
    st.bindInt(7, t.channels);
    st.bindInt(8, t.bitrate);
    st.bindText(9, t.title);
    st.bindText(10, t.artist);
    st.bindText(11, t.album);
    st.bindText(12, t.album_artist);
    st.bindText(13, t.genre);
    st.bindInt(14, t.year);
    st.bindInt(15, t.track_num);
    st.bindInt(16, t.disc_num);
    st.bindText(17, t.cover_art_path);
    st.bindInt(18, t.rating);
    st.bindInt(19, t.play_count);
    st.bindInt(20, t.last_played);
    st.bindInt(21, t.date_added);
    st.bindInt(22, t.last_scanned);
    st.bindInt(23, t.dirty ? 1 : 0);
    st.bindInt(24, t.library_id ? t.library_id : 1);
    const auto source = kanariv::types::normalizedSource(t);
    st.bindInt(25, static_cast<int>(source.kind));
    st.bindText(26, source.key);
    st.bindInt(27, source.available ? 1 : 0);
    st.bindText(28, source.display_name);
    st.bindInt(29, t.id);
}

std::expected<int64_t, kanariv::utils::Error>
Database::insertTrack(const kanariv::types::TrackRow& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->insertTrackLocked(t);
}

std::expected<void, kanariv::utils::Error>
Database::updateTrack(const kanariv::types::TrackRow& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->updateTrackLocked(t);
}

std::expected<void, kanariv::utils::Error> Database::deleteTrack(int64_t id) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->deleteTrackLocked(id);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error> Database::getTrack(int64_t id) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->getTrackLocked(id);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::getTrackLocked(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE id=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, id);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findByFingerprint(const std::array<uint8_t, 32>& fp) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findByPath(std::string_view path) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findBySource(const kanariv::types::SourceRef& source) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->findBySourceLocked(source);
}

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
Database::listTracks(std::optional<kanariv::types::TrackQuery> q) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE 1=1";
    if (q) {
        if (q->library_id)
            sql += " AND library_id=?";
        if (!q->artist.empty())
            sql += " AND artist=?";
        if (!q->album.empty())
            sql += " AND album=?";
        if (!q->genre.empty())
            sql += " AND genre=?";
        if (q->year)
            sql += " AND year=?";
        if (q->year_min)
            sql += " AND year>=?";
        if (q->year_max)
            sql += " AND year<=?";
        if (q->dirty)
            sql += " AND dirty=?";
        if (q->rating)
            sql += " AND rating=?";
        if (q->play_count_min)
            sql += " AND play_count>=?";
        if (q->play_count_max)
            sql += " AND play_count<=?";
        if (q->duration_min)
            sql += " AND duration>=?";
        if (q->duration_max)
            sql += " AND duration<=?";
        if (!q->search.empty())
            sql += " AND (title LIKE ? ESCAPE '\\' OR artist LIKE ? ESCAPE '\\' OR album LIKE "
                   "? ESCAPE '\\' OR genre LIKE ? ESCAPE '\\')";
    }
    // Build ORDER BY clause. Default: ORDER BY id ASC.
    std::string orderBy = " ORDER BY id";
    if (q && !q->sort.empty()) {
        // Whitelist of allowed sort columns to prevent SQL injection.
        static const std::unordered_map<std::string, std::string> sortCols{
            {"id", "id"},
            {"title", "title COLLATE NOCASE"},
            {"artist", "artist COLLATE NOCASE"},
            {"album", "album COLLATE NOCASE"},
            {"album_artist", "album_artist COLLATE NOCASE"},
            {"genre", "genre COLLATE NOCASE"},
            {"year", "year"},
            {"track_num", "track_num"},
            {"disc_num", "disc_num"},
            {"duration", "duration"},
            {"rating", "rating"},
            {"play_count", "play_count"},
            {"date_added", "date_added"},
        };
        auto it = sortCols.find(q->sort);
        if (it != sortCols.end()) {
            orderBy = " ORDER BY " + it->second;
            std::string dir = (q->order == "desc") ? " DESC" : " ASC";
            orderBy += dir;
        }
    }
    sql += orderBy;
    bool has_limit = q && q->limit > 0;
    bool has_offset = q && q->offset > 0;
    if (has_limit) {
        sql += " LIMIT ?";
        if (has_offset)
            sql += " OFFSET ?";
    } else if (has_offset)
        sql += " LIMIT -1 OFFSET ?";
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), sql); !e)
        return std::unexpected{e.error()};
    int idx = 1;
    std::string likePat;
    if (q) {
        if (q->library_id)
            st.bindInt(idx++, *q->library_id);
        if (!q->artist.empty())
            st.bindText(idx++, q->artist);
        if (!q->album.empty())
            st.bindText(idx++, q->album);
        if (!q->genre.empty())
            st.bindText(idx++, q->genre);
        if (q->year)
            st.bindInt(idx++, *q->year);
        if (q->year_min)
            st.bindInt(idx++, *q->year_min);
        if (q->year_max)
            st.bindInt(idx++, *q->year_max);
        if (q->dirty)
            st.bindInt(idx++, *q->dirty ? 1 : 0);
        if (q->rating)
            st.bindInt(idx++, *q->rating);
        if (q->play_count_min)
            st.bindInt(idx++, *q->play_count_min);
        if (q->play_count_max)
            st.bindInt(idx++, *q->play_count_max);
        if (q->duration_min)
            st.bindDouble(idx++, *q->duration_min);
        if (q->duration_max)
            st.bindDouble(idx++, *q->duration_max);
        if (!q->search.empty()) {
            std::string esc = internal::escapeLike(q->search);
            likePat = "%" + esc + "%";
            st.bindText(idx++, likePat);
            st.bindText(idx++, likePat);
            st.bindText(idx++, likePat);
            st.bindText(idx++, likePat);
        }
        if (has_limit) {
            st.bindInt(idx++, q->limit);
            if (has_offset)
                st.bindInt(idx++, q->offset);
        } else if (has_offset)
            st.bindInt(idx++, q->offset);
    }
    std::vector<kanariv::types::TrackRow> out;
    while (st.step()) {
        kanariv::types::TrackRow t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

std::expected<void, kanariv::utils::Error> Database::setDirty(int64_t id, int dirty) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "UPDATE tracks SET dirty=? WHERE id=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, dirty);
    st.bindInt(2, id);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

std::expected<void, kanariv::utils::Error> Database::setRating(int64_t id, int32_t rating) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (rating < 0 || rating > 5)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "rating must be 0..5")};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "UPDATE tracks SET rating=? WHERE id=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, rating);
    st.bindInt(2, id);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}
} // namespace kanariv::db
