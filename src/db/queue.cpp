#include <sqlite3.h>

#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::db {

std::expected<void, kanariv::utils::Error> queueEnqueueLocked(sqlite3* db, int64_t qid, int64_t tid,
                                                             int64_t pos) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    if (pos < 0) {
        SqliteStatement ms;
        if (auto e =
                ms.prepare(db, "SELECT COALESCE(MAX(position), -1)+1 FROM queue WHERE queue_id=?");
            e) {
            ms.bindInt(1, qid);
            if (ms.step())
                pos = ms.columnInt(0);
        } else {
            return std::unexpected{e.error()};
        }
        if (pos < 0)
            pos = 0;
    } else {
        SqliteStatement ss;
        if (auto e = ss.prepare(
                db, "UPDATE queue SET position=position+1 WHERE queue_id=? AND position>=?");
            e) {
            ss.bindInt(1, qid);
            ss.bindInt(2, pos);
            (void)ss.stepDone();
        } else {
            return std::unexpected{e.error()};
        }
    }
    SqliteStatement st;
    auto e = st.prepare(db, "INSERT INTO queue (queue_id, track_id, position) VALUES (?,?,?)");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    st.bindInt(2, tid);
    st.bindInt(3, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    return {};
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeueLocked(sqlite3* db,
                                                                                 int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT id, queue_id, track_id, position, added FROM queue "
                            "WHERE queue_id=? ORDER BY position LIMIT 1");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    bool hasRow = st.step();
    if (!hasRow) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    kanariv::types::QueueItem it;
    it.id = st.columnInt(0);
    it.queue_id = st.columnInt(1);
    it.track_id = st.columnInt(2);
    it.position = st.columnInt(3);
    it.added = st.columnInt(4);
    {
        SqliteStatement del;
        if (auto de = del.prepare(db, "DELETE FROM queue WHERE id=?"); de) {
            del.bindInt(1, it.id);
            (void)del.stepDone();
        }
    }
    {
        SqliteStatement sh;
        if (auto se = sh.prepare(
                db, "UPDATE queue SET position=position-1 WHERE queue_id=? AND position>?");
            se) {
            sh.bindInt(1, qid);
            sh.bindInt(2, it.position);
            (void)sh.stepDone();
        }
    }
    return it;
}

std::expected<void, kanariv::utils::Error> queueRemoveLocked(sqlite3* db, int64_t qid, int64_t pos) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=? AND position=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    st.bindInt(2, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    if (sqlite3_changes(db) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    {
        SqliteStatement sh;
        if (auto se = sh.prepare(
                db, "UPDATE queue SET position=position-1 WHERE queue_id=? AND position>?");
            se) {
            sh.bindInt(1, qid);
            sh.bindInt(2, pos);
            (void)sh.stepDone();
        }
    }
    return {};
}

std::expected<void, kanariv::utils::Error> queueClearLocked(sqlite3* db, int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    return {};
}

std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
queueListLocked(sqlite3* db, int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT id, queue_id, track_id, position, added FROM queue WHERE "
                            "queue_id=? ORDER BY position");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    std::vector<kanariv::types::QueueItem> out;
    while (st.step()) {
        kanariv::types::QueueItem it;
        it.id = st.columnInt(0);
        it.queue_id = st.columnInt(1);
        it.track_id = st.columnInt(2);
        it.position = st.columnInt(3);
        it.added = st.columnInt(4);
        out.push_back(it);
    }
    return out;
}

size_t queueCountLocked(sqlite3* db, int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return 0;
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT COUNT(*) FROM queue WHERE queue_id=?");
    if (!e)
        return 0;
    st.bindInt(1, qid);
    size_t cnt = 0;
    if (st.step())
        cnt = static_cast<size_t>(st.columnInt(0));
    return cnt;
}

std::optional<size_t> queueIndexOfLocked(sqlite3* db, int64_t qid, int64_t trackId) {
    if (qid == 0)
        qid = 1;
    if (!db || trackId == 0)
        return std::nullopt;
    int64_t pos = -1;
    {
        SqliteStatement st;
        auto e = st.prepare(db, "SELECT MIN(position) FROM queue WHERE queue_id=? AND track_id=?");
        if (!e)
            return std::nullopt;
        st.bindInt(1, qid);
        st.bindInt(2, trackId);
        if (!st.step())
            return std::nullopt;
        if (sqlite3_column_type(st.get(), 0) == SQLITE_NULL)
            return std::nullopt;
        pos = st.columnInt(0);
    }
    {
        SqliteStatement st;
        auto e = st.prepare(db, "SELECT COUNT(*) FROM queue WHERE queue_id=? AND position<?");
        if (!e)
            return std::nullopt;
        st.bindInt(1, qid);
        st.bindInt(2, pos);
        if (!st.step())
            return std::nullopt;
        return static_cast<size_t>(st.columnInt(0));
    }
}

std::expected<kanariv::types::Queue, kanariv::utils::Error> getQueueLocked(sqlite3* db, int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e =
        st.prepare(db, "SELECT id, name, repeat_mode, temp, library_id FROM queues WHERE id=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    if (!st.step()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    kanariv::types::Queue q;
    q.id = st.columnInt(0);
    q.name = st.columnText(1);
    q.repeat_mode = static_cast<int32_t>(st.columnInt(2));
    q.temp = st.columnInt(3) != 0;
    q.library_id = st.columnInt(4);
    return q;
}

std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> listQueuesLocked(sqlite3* db) {
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e =
        st.prepare(db, "SELECT id, name, repeat_mode, temp, library_id FROM queues ORDER BY id");
    if (!e)
        return std::unexpected{e.error()};
    std::vector<kanariv::types::Queue> out;
    while (st.step()) {
        kanariv::types::Queue q;
        q.id = st.columnInt(0);
        q.name = st.columnText(1);
        q.repeat_mode = static_cast<int32_t>(st.columnInt(2));
        q.temp = st.columnInt(3) != 0;
        q.library_id = st.columnInt(4);
        out.push_back(std::move(q));
    }
    return out;
}

std::expected<int64_t, kanariv::utils::Error> createQueueLocked(sqlite3* db, std::string_view name,
                                                               int64_t library_id) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "INSERT INTO queues (name, repeat_mode, library_id) VALUES (?,?,?)");
    if (!e)
        return std::unexpected{e.error()};
    st.bindText(1, name);
    st.bindInt(2, 0);
    st.bindInt(3, library_id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    return sqlite3_last_insert_rowid(db);
}

std::expected<void, kanariv::utils::Error> deleteQueueLocked(sqlite3* db, int64_t qid) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    {
        SqliteStatement st;
        if (auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=?"); e) {
            st.bindInt(1, qid);
            (void)st.stepDone();
        } else {
            return std::unexpected{e.error()};
        }
    }
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queues WHERE id=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    if (sqlite3_changes(db) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

std::expected<void, kanariv::utils::Error> setQueueRepeatLocked(sqlite3* db, int64_t qid,
                                                               int repeat_mode) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "UPDATE queues SET repeat_mode=? WHERE id=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, repeat_mode);
    st.bindInt(2, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    if (sqlite3_changes(db) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

std::expected<void, kanariv::utils::Error> setQueueTempLocked(sqlite3* db, int64_t qid, bool temp) {
    if (qid == 0)
        qid = 1;
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    auto e = st.prepare(db, "UPDATE queues SET temp=? WHERE id=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, temp ? 1 : 0);
    st.bindInt(2, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    if (sqlite3_changes(db) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

} // namespace kanariv::db
