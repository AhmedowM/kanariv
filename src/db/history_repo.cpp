/**
 * @file history_repo.cpp
 * @brief Playback history, bookmarks and lyrics persistence.
 * @ingroup kanariv_db
 */

#include <sqlite3.h>

#include <cstdint>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
#include <kanariv/utils/time.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db_impl.hpp"

namespace kanariv::db {

kanariv::utils::Expected<std::vector<kanariv::types::HistoryEntry>>
Database::queryHistory(const kanariv::types::HistoryQuery* q) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = "SELECT id, track_id, started_at, completed_at, position_ms, "
                      "completion_pct, queue_id FROM history WHERE 1=1";
    if (q && q->track_id)
        sql += " AND track_id=?";
    if (q && q->queue_id)
        sql += " AND queue_id=?";
    sql += " ORDER BY started_at DESC";
    bool has_lim = q && q->limit > 0;
    bool has_off = q && q->offset > 0;
    if (has_lim) {
        sql += " LIMIT ?";
        if (has_off)
            sql += " OFFSET ?";
    } else if (has_off)
        sql += " LIMIT -1 OFFSET ?";
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), sql); !e)
        return std::unexpected{e.error()};
    int idx = 1;
    if (q) {
        if (q->track_id)
            st.bindInt(idx++, *q->track_id);
        if (q->queue_id)
            st.bindInt(idx++, *q->queue_id);
        if (has_lim) {
            st.bindInt(idx++, q->limit);
            if (has_off)
                st.bindInt(idx++, q->offset);
        } else if (has_off)
            st.bindInt(idx++, q->offset);
    }
    std::vector<kanariv::types::HistoryEntry> out;
    while (st.step()) {
        kanariv::types::HistoryEntry e;
        e.id = st.columnInt(0);
        e.track_id = st.columnInt(1);
        e.started_at = st.columnInt(2);
        e.completed_at = st.columnInt(3);
        e.position_ms = st.columnInt(4);
        e.completion_pct = st.columnDouble(5);
        e.queue_id = st.columnInt(6);
        out.push_back(e);
    }
    return out;
}

kanariv::utils::Expected<void> Database::clearHistory() {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    char* err = nullptr;
    int rc = sqlite3_exec(impl_->db_.get(), "DELETE FROM history", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? std::string(err) : "clear history failed";
        if (err)
            sqlite3_free(err);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    if (err)
        sqlite3_free(err);
    return {};
}

kanariv::utils::Expected<void> Database::bookmarkAdd(int64_t tid, int64_t pos,
                                                     std::string_view note) {
    if (tid == 0 || pos < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "INSERT INTO bookmarks "
                                              "(track_id, position_ms, note, created) "
                                              "VALUES (?,?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    st.bindInt(2, pos);
    if (note.empty())
        st.bindNull(3);
    else
        st.bindText(3, note);
    st.bindInt(4, kanariv::utils::unixMillis());
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

kanariv::utils::Expected<std::vector<kanariv::types::Bookmark>>
Database::listBookmarks(int64_t tid) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "SELECT id, track_id, position_ms, note, created FROM bookmarks "
                            "WHERE track_id=? ORDER BY created, id");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    std::vector<kanariv::types::Bookmark> out;
    while (st.step()) {
        kanariv::types::Bookmark b;
        b.id = st.columnInt(0);
        b.track_id = st.columnInt(1);
        b.position_ms = st.columnInt(2);
        b.note = st.columnText(3);
        b.created = st.columnInt(4);
        out.push_back(std::move(b));
    }
    return out;
}

kanariv::utils::Expected<kanariv::types::Bookmark> Database::bookmarkGet(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "SELECT id, track_id, position_ms, note, created "
                                              "FROM bookmarks WHERE id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, id);
    if (!st.step())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    kanariv::types::Bookmark b;
    b.id = st.columnInt(0);
    b.track_id = st.columnInt(1);
    b.position_ms = st.columnInt(2);
    b.note = st.columnText(3);
    b.created = st.columnInt(4);
    return b;
}

kanariv::utils::Expected<void> Database::bookmarkRemove(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "DELETE FROM bookmarks WHERE id=?"); !e)
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

kanariv::utils::Expected<void> Database::lyricsSet(int64_t tid, std::string_view lrc_text,
                                                   bool is_synced, std::string_view source) {
    if (tid == 0 || lrc_text.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    // Upsert: delete any existing lyrics for this track, then insert.
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "DELETE FROM lyrics WHERE track_id=?"); !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    (void)st.stepDone();
    st.reset();
    if (auto e =
            st.prepare(impl_->db_.get(),
                       "INSERT INTO lyrics (track_id, lrc_text, is_synced, source, fetched_at) "
                       "VALUES (?,?,?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    st.bindText(2, lrc_text);
    st.bindInt(3, is_synced ? 1 : 0);
    if (source.empty())
        st.bindNull(4);
    else
        st.bindText(4, source);
    st.bindInt(5, kanariv::utils::unixMillis());
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

kanariv::utils::Expected<kanariv::types::Lyrics> Database::lyricsGet(int64_t tid) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(),
                       "SELECT id, track_id, lrc_text, is_synced, source, fetched_at FROM lyrics "
                       "WHERE track_id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    if (!st.step())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no lyrics for track")};
    kanariv::types::Lyrics l;
    l.id = st.columnInt(0);
    l.track_id = st.columnInt(1);
    l.lrc_text = st.columnText(2);
    l.is_synced = st.columnInt(3) != 0;
    l.source = st.columnText(4);
    l.fetched_at = st.columnInt(5);
    return l;
}

kanariv::utils::Expected<void> Database::lyricsRemove(int64_t tid) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "DELETE FROM lyrics WHERE track_id=?"); !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    if (sqlite3_changes(impl_->db_.get()) == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no lyrics for track")};
    return {};
}
} // namespace kanariv::db
