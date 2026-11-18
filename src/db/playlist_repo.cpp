/**
 * @file playlist_repo.cpp
 * @brief Playlist CRUD and membership ordering.
 * @ingroup kanariv_db
 */

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/db/smart.hpp>
#include <kanariv/fwd.hpp>
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

kanariv::utils::Expected<int64_t> Database::createPlaylist(std::string_view name, int type,
                                                           std::string_view smart_query) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "INSERT INTO playlists (name, type, smart_query) VALUES (?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, name);
    st.bindInt(2, type);
    if (smart_query.empty())
        st.bindNull(3);
    else
        st.bindText(3, smart_query);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    return sqlite3_last_insert_rowid(impl_->db_.get());
}

kanariv::utils::Expected<kanariv::types::Playlist> Database::getPlaylist(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(), "SELECT id, name, type, smart_query, created, modified, "
                                         "library_id FROM playlists WHERE id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, id);
    if (!st.step())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    kanariv::types::Playlist p;
    p.id = st.columnInt(0);
    p.name = st.columnText(1);
    p.type = static_cast<int>(st.columnInt(2));
    p.smart_query = st.columnText(3);
    p.created = st.columnInt(4);
    p.modified = st.columnInt(5);
    p.library_id = st.columnInt(6);
    return p;
}

kanariv::utils::Expected<void> Database::updatePlaylist(const kanariv::types::Playlist& p) {
    if (p.id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(), "UPDATE playlists SET name=?, type=?, smart_query=?, "
                                         "library_id=?, modified=CURRENT_TIMESTAMP WHERE id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, p.name);
    st.bindInt(2, p.type);
    if (p.smart_query.empty())
        st.bindNull(3);
    else
        st.bindText(3, p.smart_query);
    st.bindInt(4, p.library_id);
    st.bindInt(5, p.id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

kanariv::utils::Expected<void> Database::deletePlaylist(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "DELETE FROM playlists WHERE id=?"); !e)
        return std::unexpected{e.error()};
    st.bindInt(1, id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

kanariv::utils::Expected<void> Database::renamePlaylist(int64_t id, std::string_view newName) {
    if (id == 0 || newName.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "UPDATE playlists SET name=?, modified=CURRENT_TIMESTAMP WHERE id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, newName);
    st.bindInt(2, id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

kanariv::utils::Expected<int64_t>
Database::createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = kanariv::db::DbTransaction::begin(impl_->db_.get());
    if (!txRes)
        return std::unexpected{txRes.error()};
    auto tx = std::move(*txRes);
    auto pidRes = impl_->createPlaylistLocked(name);
    if (!pidRes)
        return std::unexpected{pidRes.error()};
    int64_t pid = *pidRes;
    for (int64_t tid : trackIds) {
        auto r = impl_->playlistAddTrackLocked(pid, tid);
        if (!r)
            return std::unexpected{r.error()};
    }
    if (auto c = tx.commit(); !c)
        return std::unexpected{c.error()};
    return pid;
}

kanariv::utils::Expected<int64_t>
Database::Impl::createPlaylistLocked(std::string_view name, int type,
                                     std::string_view smart_query) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(db_.get(), "INSERT INTO playlists (name, type, smart_query) VALUES (?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, name);
    st.bindInt(2, type);
    if (smart_query.empty())
        st.bindNull(3);
    else
        st.bindText(3, smart_query);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(db_.get()))};
    return sqlite3_last_insert_rowid(db_.get());
}

kanariv::utils::Expected<void> Database::Impl::playlistAddTrackLocked(int64_t pid, int64_t tid,
                                                                      int64_t pos) {
    if (pid == 0 || tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    if (pos < 0) {
        SqliteStatement ms;
        if (auto e = ms.prepare(db_.get(),
                                "SELECT COALESCE(MAX(position), -1)+1 FROM playlist_items WHERE "
                                "playlist_id=?");
            e) {
            ms.bindInt(1, pid);
            if (ms.step())
                pos = ms.columnInt(0);
        }
        if (pos < 0)
            pos = 0;
    } else {
        SqliteStatement ss;
        if (auto e = ss.prepare(db_.get(), "UPDATE playlist_items SET position=position+1 WHERE "
                                           "playlist_id=? AND position>=?");
            e) {
            ss.bindInt(1, pid);
            ss.bindInt(2, pos);
            (void)ss.stepDone();
        }
    }
    SqliteStatement st;
    if (auto e = st.prepare(db_.get(),
                            "INSERT INTO playlist_items (playlist_id, track_id, position) VALUES "
                            "(?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, pid);
    st.bindInt(2, tid);
    st.bindInt(3, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(db_.get()))};
    return {};
}

kanariv::utils::Expected<std::vector<kanariv::types::Playlist>> Database::listPlaylists() {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(), "SELECT id, name, type, smart_query, created, modified, "
                                         "library_id FROM playlists ORDER BY id");
        !e)
        return std::unexpected{e.error()};
    std::vector<kanariv::types::Playlist> out;
    while (st.step()) {
        kanariv::types::Playlist p;
        p.id = st.columnInt(0);
        p.name = st.columnText(1);
        p.type = static_cast<int>(st.columnInt(2));
        p.smart_query = st.columnText(3);
        p.created = st.columnInt(4);
        p.modified = st.columnInt(5);
        p.library_id = st.columnInt(6);
        out.push_back(std::move(p));
    }
    return out;
}

kanariv::utils::Expected<void> Database::playlistAddTrack(int64_t pid, int64_t tid, int64_t pos) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->playlistAddTrackLocked(pid, tid, pos);
}

kanariv::utils::Expected<void> Database::playlistRemoveTrack(int64_t pid, int64_t tid) {
    if (pid == 0 || tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "DELETE FROM playlist_items WHERE playlist_id=? AND track_id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, pid);
    st.bindInt(2, tid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

kanariv::utils::Expected<void> Database::playlistReorder(int64_t pid, int64_t from, int64_t to) {
    if (pid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (from == to)
        return {};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto tx = DbTransaction::begin(impl_->db_.get());
    if (!tx)
        return std::unexpected{tx.error()};
    SqliteStatement sel;
    if (auto e =
            sel.prepare(impl_->db_.get(),
                        "SELECT track_id FROM playlist_items WHERE playlist_id=? AND position=?");
        !e)
        return std::unexpected{e.error()};
    sel.bindInt(1, pid);
    sel.bindInt(2, from);
    if (!sel.step())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    int64_t track_id = sel.columnInt(0);
    const bool forward = from < to;
    SqliteStatement shift;
    if (auto e = shift.prepare(impl_->db_.get(),
                               forward ? "UPDATE playlist_items SET position=position-1 WHERE "
                                         "playlist_id=? AND position>? AND position<=?"
                                       : "UPDATE playlist_items SET position=position+1 WHERE "
                                         "playlist_id=? AND position>=? AND position<?");
        !e)
        return std::unexpected{e.error()};
    shift.bindInt(1, pid);
    shift.bindInt(2, forward ? from : to);
    shift.bindInt(3, forward ? to : from);
    if (int rc = shift.stepDone(); rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    SqliteStatement us;
    if (auto e =
            us.prepare(impl_->db_.get(),
                       "UPDATE playlist_items SET position=? WHERE playlist_id=? AND track_id=?");
        !e)
        return std::unexpected{e.error()};
    us.bindInt(1, to);
    us.bindInt(2, pid);
    us.bindInt(3, track_id);
    if (int rc = us.stepDone(); rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    auto committed = tx->commit();
    if (!committed)
        return std::unexpected{committed.error()};
    return {};
}

kanariv::utils::Expected<std::vector<kanariv::types::TrackRow>>
Database::playlistGetTracks(int64_t pid) {
    if (pid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    // Resolve the playlist row first (acquires its own shared lock). Manual
    // playlists fall through to the playlist_items join below; smart
    // playlists (non-zero type with a smart_query) are evaluated against the
    // library via listTracks().
    auto pl = getPlaylist(pid);
    if (!pl)
        return std::unexpected{pl.error()};
    if (pl->type != 0 && !pl->smart_query.empty()) {
        auto q = parseSmartQuery(pl->smart_query);
        if (!q)
            return std::unexpected{q.error()};
        return listTracks(*q);
    }
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) +
                      " JOIN playlist_items pi ON pi.track_id=tracks.id "
                      "WHERE pi.playlist_id=? ORDER BY pi.position";
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), sql); !e)
        return std::unexpected{e.error()};
    st.bindInt(1, pid);
    std::vector<kanariv::types::TrackRow> out;
    while (st.step()) {
        kanariv::types::TrackRow t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}
} // namespace kanariv::db
