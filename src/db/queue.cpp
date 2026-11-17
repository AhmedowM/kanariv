#include <sqlite3.h>

#include <algorithm>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::db {

namespace {

/**
 * @brief Gap between queue position values (64-bit INTEGER column).
 * @approx 1e9 allows ~30 midpoint insertions between any two adjacent items
 * before a renormalization is required.
 */
constexpr int64_t positionGap = 1000000000LL;

/**
 * @brief Maps a stored `queues.repeat_mode` integer to the wire enum.
 * @details The column keeps its INTEGER type; only the in-memory representation
 * is typed. Out-of-range legacy or corrupt values fall back to
 * `RepeatMode::Off` so storage can never invent a playback policy.
 */
[[nodiscard]] kanariv::types::RepeatMode repeatModeFromStored(int value) noexcept {
    switch (value) {
    case static_cast<int>(kanariv::types::RepeatMode::All):
        return kanariv::types::RepeatMode::All;
    case static_cast<int>(kanariv::types::RepeatMode::One):
        return kanariv::types::RepeatMode::One;
    default:
        return kanariv::types::RepeatMode::Off;
    }
}

/**
 * @brief Position of the item at a given 0-based index (ORDER BY position),
 * or -1 if the index is beyond the queue length.
 */
std::expected<int64_t, kanariv::utils::Error> positionAtIndex(sqlite3* db, int64_t qid,
                                                              int64_t index) {
    if (index < 0)
        return int64_t{-1};
    SqliteStatement st;
    if (auto e = st.prepare(db,
            "SELECT position FROM queue WHERE queue_id=? ORDER BY position "
            "LIMIT 1 OFFSET ?");
        !e) {
        return std::unexpected{e.error()};
    }
    st.bindInt(1, qid);
    st.bindInt(2, index);
    if (!st.step())
        return -1;
    return st.columnInt(0);
}

/**
 * @brief Writes dense positions (base, base+GAP, ...) to ids in vector order.
 * @details The order of the writes is the vector order, so callers run a
 * parking pass (a base above every current position) before the final pass
 * when UNIQUE(queue_id, position) could otherwise see a transient collision.
 */
std::expected<void, kanariv::utils::Error>
assignQueuePositions(sqlite3* db, const std::vector<int64_t>& ids, int64_t base) {
    SqliteStatement upd;
    if (auto e = upd.prepare(db, "UPDATE queue SET position=? WHERE id=?"); !e)
        return std::unexpected{e.error()};
    for (size_t i = 0; i < ids.size(); ++i) {
        upd.reset();
        upd.bindInt(1, base + static_cast<int64_t>(i) * positionGap);
        upd.bindInt(2, ids[i]);
        if (upd.stepDone() != SQLITE_DONE)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                              sqlite3_errmsg(db))};
    }
    return {};
}

/**
 * @brief Highest stored position in a queue (0 for an empty queue).
 */
std::expected<int64_t, kanariv::utils::Error> maxQueuePositionLocked(sqlite3* db, int64_t qid) {
    SqliteStatement maxStmt;
    if (auto e =
            maxStmt.prepare(db, "SELECT COALESCE(MAX(position), 0) FROM queue WHERE queue_id=?");
        !e) {
        return std::unexpected{e.error()};
    }
    maxStmt.bindInt(1, qid);
    if (!maxStmt.step())
        return int64_t{0};
    return maxStmt.columnInt(0);
}

/**
 * @brief Renumber all queue positions to restore full gaps (0, GAP, 2*GAP, ...).
 * @details Called only when midpoint insertion runs out of gap space
 * (~30 halvings). Runs in two phases because UNIQUE(queue_id, position) is
 * checked per updated row: phase one parks every row above all current values
 * (so no later move can hit a row that was not renumbered yet), phase two
 * assigns the dense final values. Both phases are ordered by the current
 * position, so queue order is unchanged.
 */
std::expected<void, kanariv::utils::Error> renormalizeQueueLocked(sqlite3* db, int64_t qid) {
    SqliteStatement sel;
    if (auto e = sel.prepare(db, "SELECT id FROM queue WHERE queue_id=? ORDER BY position"); !e) {
        return std::unexpected{e.error()};
    }
    sel.bindInt(1, qid);
    std::vector<int64_t> ids;
    while (sel.step())
        ids.push_back(sel.columnInt(0));

    auto maxPos = maxQueuePositionLocked(db, qid);
    if (!maxPos)
        return std::unexpected{maxPos.error()};
    const int64_t tempBase =
        std::max<int64_t>(*maxPos, 0) + (static_cast<int64_t>(ids.size()) + 1) * positionGap;

    // Phase 1: park above every current value. Phase 2: dense final values.
    if (auto parked = assignQueuePositions(db, ids, tempBase); !parked)
        return parked;
    return assignQueuePositions(db, ids, 0);
}

} // namespace

std::expected<void, kanariv::utils::Error> queueEnqueueLocked(sqlite3* db, int64_t qid, int64_t tid,
                                                              int64_t index) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    qid = normalizeQueueId(qid);

    int64_t newPos;
    if (isAppendIndex(index)) {
        // Append: place at MAX(position) + GAP (or 0 for an empty queue).
        SqliteStatement ms;
        if (auto e =
                ms.prepare(db, "SELECT COALESCE(MAX(position), -1) FROM queue WHERE queue_id=?");
            e) {
            ms.bindInt(1, qid);
            if (ms.step())
                newPos = ms.columnInt(0);
            else
                return std::unexpected{
                    kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                              "max position query failed")};
        } else {
            return std::unexpected{e.error()};
        }
        newPos = (newPos < 0) ? 0 : newPos + positionGap;
    } else if (index < 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "queue insertion index below -1")};
    } else {
        // Insert at index `index`: find neighbours at indices index-1 and index.
        if (index > static_cast<int64_t>(queueCountLocked(db, qid)))
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "queue insertion index past end")};
        auto prev = positionAtIndex(db, qid, index - 1);
        auto next = positionAtIndex(db, qid, index);
        if (!prev || !next)
            return std::unexpected{!prev ? prev.error() : next.error()};

        int64_t prevPos = *prev;
        int64_t nextPos = *next;

        if (prevPos >= 0 && nextPos >= 0) {
            // Both neighbours exist: use midpoint. Renormalize if gap exhausted.
            if (nextPos - prevPos <= 1) {
                if (auto e = renormalizeQueueLocked(db, qid); !e)
                    return std::unexpected{e.error()};
                // Retry neighbour lookup after renormalization.
                prev = positionAtIndex(db, qid, index - 1);
                next = positionAtIndex(db, qid, index);
                if (!prev || !next)
                    return std::unexpected{!prev ? prev.error() : next.error()};
                prevPos = *prev;
                nextPos = *next;
            }
            newPos = (prevPos + nextPos) / 2;
        } else if (prevPos >= 0) {
            // Only predecessor: append after it.
            newPos = prevPos + positionGap;
        } else if (nextPos >= 0) {
            // Only successor: insert before it.
            newPos = nextPos - positionGap;
        } else {
            // Empty queue.
            newPos = 0;
        }
    }

    SqliteStatement st;
    auto e = st.prepare(db, "INSERT INTO queue (queue_id, track_id, position) VALUES (?,?,?)");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    st.bindInt(2, tid);
    st.bindInt(3, newPos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    return {};
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeueLocked(sqlite3* db,
                                                                                 int64_t qid) {
    qid = normalizeQueueId(qid);
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
    // No position shift: gaps are intentional and ordering is by position value.
    return it;
}

std::expected<void, kanariv::utils::Error> queueRemoveLocked(sqlite3* db, int64_t qid,
                                                             int64_t index) {
    qid = normalizeQueueId(qid);
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    if (index < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "queue index below zero")};
    auto pos = positionAtIndex(db, qid, index);
    if (!pos)
        return std::unexpected{pos.error()};
    if (*pos < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    SqliteStatement st;
    auto e = st.prepare(db, "DELETE FROM queue WHERE queue_id=? AND position=?");
    if (!e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    st.bindInt(2, *pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    if (sqlite3_changes(db) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    // No position shift: gaps are intentional and ordering is by position value.
    return {};
}

std::expected<void, kanariv::utils::Error> queueClearLocked(sqlite3* db, int64_t qid) {
    qid = normalizeQueueId(qid);
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
    qid = normalizeQueueId(qid);
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
    qid = normalizeQueueId(qid);
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
    qid = normalizeQueueId(qid);
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

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
queueTracksLocked(sqlite3* db, int64_t qid) {
    qid = normalizeQueueId(qid);
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    // The CTE exposes only track_id/position, so the canonical unqualified
    // track column list stays unambiguous while duplicates survive the join.
    const std::string sql =
        std::string("WITH ordered AS (SELECT track_id, position FROM queue WHERE queue_id=? "
                    "ORDER BY position) ") +
        std::string(internal::selectTracksCols) +
        " JOIN ordered ON tracks.id = ordered.track_id ORDER BY ordered.position";
    SqliteStatement st;
    if (auto e = st.prepare(db, sql); !e)
        return std::unexpected{e.error()};
    st.bindInt(1, qid);
    std::vector<kanariv::types::TrackRow> out;
    while (st.step()) {
        kanariv::types::TrackRow t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

std::expected<void, kanariv::utils::Error> queueMoveLocked(sqlite3* db, int64_t qid,
                                                           std::size_t from, std::size_t to) {
    qid = normalizeQueueId(qid);
    if (!db)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement sel;
    if (auto e = sel.prepare(db, "SELECT id FROM queue WHERE queue_id=? ORDER BY position"); !e)
        return std::unexpected{e.error()};
    sel.bindInt(1, qid);
    std::vector<int64_t> ids;
    while (sel.step())
        ids.push_back(sel.columnInt(0));
    if (from >= ids.size() || to >= ids.size())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "queue move: index out of range")};
    if (from == to)
        return {};
    const int64_t moved = ids[from];
    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(from));
    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(to), moved);
    auto maxPos = maxQueuePositionLocked(db, qid);
    if (!maxPos)
        return std::unexpected{maxPos.error()};
    const int64_t tempBase =
        std::max<int64_t>(*maxPos, 0) + (static_cast<int64_t>(ids.size()) + 1) * positionGap;
    if (auto parked = assignQueuePositions(db, ids, tempBase); !parked)
        return parked;
    return assignQueuePositions(db, ids, 0);
}

std::expected<kanariv::types::Queue, kanariv::utils::Error> getQueueLocked(sqlite3* db, int64_t qid) {
    qid = normalizeQueueId(qid);
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
    q.repeat_mode = repeatModeFromStored(static_cast<int>(st.columnInt(2)));
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
        q.repeat_mode = repeatModeFromStored(static_cast<int>(st.columnInt(2)));
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
    st.bindInt(2, static_cast<int>(kanariv::types::RepeatMode::Off));
    st.bindInt(3, library_id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(db))};
    return sqlite3_last_insert_rowid(db);
}

std::expected<void, kanariv::utils::Error> deleteQueueLocked(sqlite3* db, int64_t qid) {
    qid = normalizeQueueId(qid);
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
    qid = normalizeQueueId(qid);
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
    qid = normalizeQueueId(qid);
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
