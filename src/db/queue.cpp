#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <expected>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::db {

constexpr int bindQueuePosition = 3;
constexpr int bindQueueRepeatMode = 2;
constexpr int bindQueueLibraryId = 3;
constexpr int bindQueueId = 1;
constexpr int bindQueueTrackId = 2;
constexpr int defaultQueueId = 1;
constexpr int defaultRepeatMode = 0;
constexpr int colQueueId = 0;
constexpr int colQueueQueueId = 1;
constexpr int colQueueTrackId = 2;
constexpr int colQueuePosition = 3;
constexpr int colQueueAdded = 4;
constexpr int colPos = 0;
constexpr int colQueuesId = 0;
constexpr int colQueuesName = 1;
constexpr int colQueuesRepeatMode = 2;
constexpr int colQueuesTemp = 3;
constexpr int colQueuesLibraryId = 4;

auto queueEnqueueLocked(sqlite3* db, int64_t qid, int64_t tid, int64_t pos)
    -> std::expected<void, kanariv::utils::Error> {
    if (tid == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    }
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    if (pos < 0) {
        SqliteStatement ms;
        if (auto e =
                ms.prepare(db, "SELECT COALESCE(MAX(position), -1)+1 FROM queue WHERE queue_id=?");
            e) {
            ms.bindInt(bindQueueId, qid);
            if (ms.step()) {
                pos = ms.columnInt(colPos);
            }
        } else {
            return std::unexpected{e.error()};
        }
        pos = std::max<int64_t>(pos, 0);
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
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    st.bindInt(bindQueueTrackId, tid);
    st.bindInt(bindQueuePosition, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    return {};
}

auto queueDequeueLocked(sqlite3* db, int64_t qid)
    -> std::expected<kanariv::types::QueueItem, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT id, queue_id, track_id, position, added FROM queue "
                            "WHERE queue_id=? ORDER BY position LIMIT 1");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    bool hasRow = st.step();
    if (!hasRow) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    kanariv::types::QueueItem it;
    it.id = st.columnInt(colQueueId);
    it.queue_id = st.columnInt(colQueueQueueId);
    it.track_id = st.columnInt(colQueueTrackId);
    it.position = st.columnInt(colQueuePosition);
    it.added = st.columnInt(colQueueAdded);
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

auto queueRemoveLocked(sqlite3* db, int64_t qid, int64_t pos)
    -> std::expected<void, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=? AND position=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    st.bindInt(2, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    if (sqlite3_changes(db) == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
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

auto queueClearLocked(sqlite3* db, int64_t qid) -> std::expected<void, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    return {};
}

auto queueListLocked(sqlite3* db, int64_t qid)
    -> std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT id, queue_id, track_id, position, added FROM queue WHERE "
                            "queue_id=? ORDER BY position");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    std::vector<kanariv::types::QueueItem> out;
    while (st.step()) {
        kanariv::types::QueueItem it;
        it.id = st.columnInt(colQueueId);
        it.queue_id = st.columnInt(colQueueQueueId);
        it.track_id = st.columnInt(colQueueTrackId);
        it.position = st.columnInt(colQueuePosition);
        it.added = st.columnInt(colQueueAdded);
        out.push_back(it);
    }
    return out;
}

auto queueCountLocked(sqlite3* db, int64_t qid) -> size_t {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return 0;
    }
    SqliteStatement st;
    auto e = st.prepare(db, "SELECT COUNT(*) FROM queue WHERE queue_id=?");
    if (!e) {
        return 0;
    }
    st.bindInt(bindQueueId, qid);
    size_t cnt = 0;
    if (st.step()) {
        cnt = static_cast<size_t>(st.columnInt(0));
    }
    return cnt;
}

auto queueIndexOfLocked(sqlite3* db, int64_t qid, int64_t trackId) -> std::optional<size_t> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr || trackId == 0) {
        return std::nullopt;
    }
    int64_t pos = -1;
    {
        SqliteStatement st;
        auto e = st.prepare(db, "SELECT MIN(position) FROM queue WHERE queue_id=? AND track_id=?");
        if (!e) {
            return std::nullopt;
        }
        st.bindInt(bindQueueId, qid);
        st.bindInt(2, trackId);
        if (!st.step()) {
            return std::nullopt;
        }
        if (sqlite3_column_type(st.get(), 0) == SQLITE_NULL) {
            return std::nullopt;
        }
        pos = st.columnInt(colPos);
    }
    {
        SqliteStatement st;
        auto e = st.prepare(db, "SELECT COUNT(*) FROM queue WHERE queue_id=? AND position<?");
        if (!e) {
            return std::nullopt;
        }
        st.bindInt(bindQueueId, qid);
        st.bindInt(bindQueuePosition, pos);
        if (!st.step()) {
            return std::nullopt;
        }
        return static_cast<size_t>(st.columnInt(colPos));
    }
}

auto getQueueLocked(sqlite3* db, int64_t qid)
    -> std::expected<kanariv::types::Queue, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e =
        st.prepare(db, "SELECT id, name, repeat_mode, temp, library_id FROM queues WHERE id=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    if (!st.step()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    kanariv::types::Queue q;
    q.id = st.columnInt(colQueueId);
    q.name = st.columnText(colQueueQueueId);
    q.repeat_mode = static_cast<int32_t>(st.columnInt(colQueueTrackId));
    q.temp = st.columnInt(colQueuePosition) != 0;
    q.library_id = st.columnInt(colQueueAdded);
    return q;
}

auto listQueuesLocked(sqlite3* db)
    -> std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> {
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e =
        st.prepare(db, "SELECT id, name, repeat_mode, temp, library_id FROM queues ORDER BY id");
    if (!e) {
        return std::unexpected{e.error()};
    }
    std::vector<kanariv::types::Queue> out;
    while (st.step()) {
        kanariv::types::Queue q;
        q.id = st.columnInt(colQueuesId);
        q.name = st.columnText(colQueuesName);
        q.repeat_mode = static_cast<int32_t>(st.columnInt(colQueuesRepeatMode));
        q.temp = st.columnInt(colQueuesTemp) != 0;
        q.library_id = st.columnInt(colQueuesLibraryId);
        out.push_back(std::move(q));
    }
    return out;
}

auto createQueueLocked(sqlite3* db, std::string_view name, int64_t library_id)
    -> std::expected<int64_t, kanariv::utils::Error> {
    if (name.empty()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "INSERT INTO queues (name, repeat_mode, library_id) VALUES (?,?,?)");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindText(1, name);
    st.bindInt(bindQueueRepeatMode, defaultRepeatMode);
    st.bindInt(bindQueueLibraryId, library_id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    return sqlite3_last_insert_rowid(db);
}

auto deleteQueueLocked(sqlite3* db, int64_t qid) -> std::expected<void, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    {
        SqliteStatement st;
        if (auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=?"); e) {
            st.bindInt(bindQueueId, qid);
            (void)st.stepDone();
        } else {
            return std::unexpected{e.error()};
        }
    }
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queues WHERE id=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(bindQueueId, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    if (sqlite3_changes(db) == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    return {};
}

auto setQueueRepeatLocked(sqlite3* db, int64_t qid, int repeat_mode)
    -> std::expected<void, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "UPDATE queues SET repeat_mode=? WHERE id=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(1, repeat_mode);
    st.bindInt(2, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    if (sqlite3_changes(db) == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    return {};
}

auto setQueueTempLocked(sqlite3* db, int64_t qid, bool temp)
    -> std::expected<void, kanariv::utils::Error> {
    if (qid == 0) {
        qid = defaultQueueId;
    }
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    SqliteStatement st;
    auto e = st.prepare(db, "UPDATE queues SET temp=? WHERE id=?");
    if (!e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(1, temp ? 1 : 0);
    st.bindInt(2, qid);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    }
    if (sqlite3_changes(db) == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    }
    return {};
}

} // namespace kanariv::db
