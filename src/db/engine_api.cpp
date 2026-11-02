#include <sqlite3.h>

#include <kanariv/db/database.hpp>
#include <kanariv/utils/error.hpp>
#include <cstring>
#include <db/database_access.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace kanariv::db {

std::expected<bool, kanariv::utils::Error> Database::queueExists(int64_t qid) {
    auto queue = getQueue(qid);
    if (!queue) {
        if (queue.error().code == kanariv::utils::StatusCode::NotFound)
            return false;
        return std::unexpected(queue.error());
    }
    return true;
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Database::playbackStats() {
    auto stats = getStats();
    if (!stats)
        return std::unexpected(stats.error());
    return kanariv::types::PlaybackStats{
        stats->num_tracks,    stats->num_playlists, stats->num_queue_items,  stats->num_history,
        stats->num_bookmarks, stats->num_libraries, stats->total_duration_ms};
}

bool Database::isOpen() const noexcept {
    return DatabaseAccess::handle(const_cast<Database&>(*this)) != nullptr;
}

std::expected<kanariv::types::Track, kanariv::utils::Error> Database::queueTrackAt(int64_t qid,
                                                                                 int64_t position) {
    if (qid == 0)
        qid = 1;
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    int64_t trackId = 0;
    {
        std::shared_lock lock(DatabaseAccess::mutex(*this));
        sqlite3_stmt* raw = nullptr;
        const char* sql =
            "SELECT track_id FROM queue WHERE queue_id=? ORDER BY position LIMIT 1 OFFSET ?";
        if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK)
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                            sqlite3_errmsg(handle)));
        internal::StmtGuard statement(raw);
        sqlite3_bind_int64(raw, 1, qid);
        sqlite3_bind_int64(raw, 2, position);
        if (sqlite3_step(raw) != SQLITE_ROW)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "not found"));
        trackId = sqlite3_column_int64(raw, 0);
    }
    return getTrack(trackId);
}

std::expected<kanariv::types::EngineState, kanariv::utils::Error> Database::loadEngineState() {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::shared_lock lock(DatabaseAccess::mutex(*this));
    const char* sqlNew = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, "
                         "volume, shuffle_perm, active_queue_id FROM engine_state WHERE id=1";
    const char* sqlOld = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, "
                         "volume, shuffle_perm FROM engine_state WHERE id=1";
    sqlite3_stmt* raw = nullptr;
    bool hasActiveQueue = true;
    if (sqlite3_prepare_v2(handle, sqlNew, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite3_finalize(raw);
        raw = nullptr;
        hasActiveQueue = false;
        if (sqlite3_prepare_v2(handle, sqlOld, -1, &raw, nullptr) != SQLITE_OK)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    }
    internal::StmtGuard statement(raw);
    kanariv::types::EngineState state;
    int rc = sqlite3_step(raw);
    if (rc == SQLITE_DONE)
        return state;
    if (rc != SQLITE_ROW)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "load failed"));
    state.shuffle_enabled = sqlite3_column_int(raw, 0);
    state.repeat_mode = static_cast<kanariv::types::RepeatMode>(sqlite3_column_int(raw, 1));
    state.cursor_pos = sqlite3_column_int64(raw, 2);
    state.current_track_id = sqlite3_column_int64(raw, 3);
    state.volume = static_cast<float>(sqlite3_column_double(raw, 4));
    if (state.volume < 0.0f || state.volume > 1.0f)
        state.volume = 1.0f;
    if (hasActiveQueue) {
        state.active_queue_id = sqlite3_column_int64(raw, 6);
        if (state.active_queue_id <= 0)
            state.active_queue_id = 1;
    }
    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(raw, 5));
    const int blobBytes = sqlite3_column_bytes(raw, 5);
    if (blob && blobBytes >= static_cast<int>(sizeof(int64_t)) && state.shuffle_enabled) {
        const size_t count = static_cast<size_t>(blobBytes) / sizeof(int64_t);
        state.shuffle_perm.resize(count);
        std::memcpy(state.shuffle_perm.data(), blob, count * sizeof(int64_t));
    }

    sqlite3_stmt* optional = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT pre_mute_volume FROM engine_state WHERE id=1", -1,
                           &optional, nullptr) == SQLITE_OK) {
        internal::StmtGuard optionalStatement(optional);
        if (sqlite3_step(optional) == SQLITE_ROW) {
            const float preMute = static_cast<float>(sqlite3_column_double(optional, 0));
            if (preMute > 0.0f && preMute <= 1.0f)
                state.pre_mute_volume = preMute;
        }
    }

    optional = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT position_ms FROM engine_state WHERE id=1", -1, &optional,
                           nullptr) == SQLITE_OK) {
        internal::StmtGuard optionalStatement(optional);
        if (sqlite3_step(optional) == SQLITE_ROW) {
            state.position_ms = sqlite3_column_int64(optional, 0);
            if (state.position_ms < 0)
                state.position_ms = 0;
        }
    }

    sqlite3_stmt* queue = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT id FROM queues WHERE id=?", -1, &queue, nullptr) ==
        SQLITE_OK) {
        internal::StmtGuard queueStatement(queue);
        sqlite3_bind_int64(queue, 1, state.active_queue_id);
        if (sqlite3_step(queue) != SQLITE_ROW)
            state.active_queue_id = 1;
    }
    return state;
}

std::expected<void, kanariv::utils::Error>
Database::saveEngineState(const kanariv::types::EngineState& state) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction)
        return std::unexpected(transaction.error());
    const char* sqlNew =
        "UPDATE engine_state SET shuffle_enabled=?, repeat_mode=?, shuffle_perm=?, "
        "cursor_pos=?, current_track_id=?, volume=?, active_queue_id=?, position_ms=?, "
        "updated=CURRENT_TIMESTAMP WHERE id=1";
    const char* sqlOld =
        "UPDATE engine_state SET shuffle_enabled=?, repeat_mode=?, shuffle_perm=?, "
        "cursor_pos=?, current_track_id=?, volume=?, updated=CURRENT_TIMESTAMP WHERE id=1";
    sqlite3_stmt* raw = nullptr;
    bool hasNewColumns = true;
    if (sqlite3_prepare_v2(handle, sqlNew, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite3_finalize(raw);
        raw = nullptr;
        hasNewColumns = false;
        if (sqlite3_prepare_v2(handle, sqlOld, -1, &raw, nullptr) != SQLITE_OK)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    }
    internal::StmtGuard statement(raw);
    if (state.shuffle_perm.size() >
        static_cast<size_t>(std::numeric_limits<int>::max() / static_cast<int>(sizeof(int64_t))))
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem, "perm too large"));
    sqlite3_bind_int(raw, 1, state.shuffle_enabled);
    sqlite3_bind_int(raw, 2, static_cast<int>(state.repeat_mode));
    if (state.shuffle_enabled && !state.shuffle_perm.empty())
        sqlite3_bind_blob(raw, 3, state.shuffle_perm.data(),
                          static_cast<int>(state.shuffle_perm.size() * sizeof(int64_t)),
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(raw, 3);
    sqlite3_bind_int64(raw, 4, state.cursor_pos);
    sqlite3_bind_int64(raw, 5, state.current_track_id);
    sqlite3_bind_double(raw, 6, static_cast<double>(state.volume));
    if (hasNewColumns) {
        sqlite3_bind_int64(raw, 7, state.active_queue_id > 0 ? state.active_queue_id : 1);
        sqlite3_bind_int64(raw, 8, state.position_ms >= 0 ? state.position_ms : 0);
    }
    if (sqlite3_step(raw) != SQLITE_DONE)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    sqlite3_stmt* optional = nullptr;
    if (sqlite3_prepare_v2(handle, "UPDATE engine_state SET pre_mute_volume=? WHERE id=1", -1,
                           &optional, nullptr) == SQLITE_OK) {
        internal::StmtGuard optionalStatement(optional);
        sqlite3_bind_double(optional, 1, static_cast<double>(state.pre_mute_volume));
        (void)sqlite3_step(optional);
    }
    return transaction->commit();
}

std::expected<void, kanariv::utils::Error>
Database::saveEngineShuffle(bool enabled, std::span<const int64_t> permutation,
                            int64_t cursorPosition) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    if (permutation.size() >
        static_cast<size_t>(std::numeric_limits<int>::max() / static_cast<int>(sizeof(int64_t))))
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem, "perm too large"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction)
        return std::unexpected(transaction.error());
    sqlite3_stmt* raw = nullptr;
    const char* sql = "UPDATE engine_state SET shuffle_perm=?, cursor_pos=?, "
                      "shuffle_enabled=? WHERE id=1";
    if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    internal::StmtGuard statement(raw);
    if (enabled && !permutation.empty())
        sqlite3_bind_blob(raw, 1, permutation.data(),
                          static_cast<int>(permutation.size() * sizeof(int64_t)), SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(raw, 1);
    sqlite3_bind_int64(raw, 2, cursorPosition);
    sqlite3_bind_int(raw, 3, enabled ? 1 : 0);
    if (sqlite3_step(raw) != SQLITE_DONE)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    return transaction->commit();
}

std::expected<void, kanariv::utils::Error> Database::saveEngineCursor(int64_t cursorPosition) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction)
        return std::unexpected(transaction.error());
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, "UPDATE engine_state SET cursor_pos=? WHERE id=1", -1, &raw,
                           nullptr) != SQLITE_OK)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    internal::StmtGuard statement(raw);
    sqlite3_bind_int64(raw, 1, cursorPosition);
    if (sqlite3_step(raw) != SQLITE_DONE)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    return transaction->commit();
}

std::expected<void, kanariv::utils::Error>
Database::markPlayed(int64_t trackId, int64_t startedAt, int64_t completedAt, int64_t positionMs,
                     double completionPct, int64_t queueId) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction)
        return std::unexpected(transaction.error());
    {
        sqlite3_stmt* select = nullptr;
        if (sqlite3_prepare_v2(handle, "SELECT play_count FROM tracks WHERE id=?", -1, &select,
                               nullptr) != SQLITE_OK)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        internal::StmtGuard selectStatement(select);
        sqlite3_bind_int64(select, 1, trackId);
        if (sqlite3_step(select) != SQLITE_ROW)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "track not found"));
        const int64_t playCount = sqlite3_column_int64(select, 0) + 1;
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(handle, "UPDATE tracks SET play_count=?, last_played=? WHERE id=?",
                               -1, &update, nullptr) != SQLITE_OK)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        internal::StmtGuard updateStatement(update);
        sqlite3_bind_int64(update, 1, playCount);
        sqlite3_bind_int64(update, 2, completedAt);
        sqlite3_bind_int64(update, 3, trackId);
        if (sqlite3_step(update) != SQLITE_DONE)
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                            "track update failed"));
    }
    {
        sqlite3_stmt* insert = nullptr;
        const char* sql = "INSERT INTO history (track_id, started_at, completed_at, position_ms, "
                          "completion_pct, queue_id) VALUES (?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(handle, sql, -1, &insert, nullptr) != SQLITE_OK)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        internal::StmtGuard insertStatement(insert);
        sqlite3_bind_int64(insert, 1, trackId);
        sqlite3_bind_int64(insert, 2, startedAt);
        sqlite3_bind_int64(insert, 3, completedAt);
        sqlite3_bind_int64(insert, 4, positionMs);
        sqlite3_bind_double(insert, 5, completionPct);
        sqlite3_bind_int64(insert, 6, queueId > 0 ? queueId : 1);
        if (sqlite3_step(insert) != SQLITE_DONE)
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                            "history insert failed"));
    }
    return transaction->commit();
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Database::engineHistoryList(int limit) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::shared_lock lock(DatabaseAccess::mutex(*this));
    std::string sql = "SELECT h.id, h.track_id, h.started_at, h.completed_at, h.position_ms, "
                      "h.completion_pct, h.queue_id, t.title, t.artist, t.path, t.duration "
                      "FROM history h JOIN tracks t ON t.id=h.track_id "
                      "ORDER BY h.started_at DESC";
    if (limit > 0)
        sql += " LIMIT ?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, sqlite3_errmsg(handle)));
    internal::StmtGuard statement(raw);
    if (limit > 0)
        sqlite3_bind_int(raw, 1, limit);
    std::vector<kanariv::types::HistoryEntry> entries;
    while (sqlite3_step(raw) == SQLITE_ROW) {
        kanariv::types::HistoryEntry entry;
        entry.id = sqlite3_column_int64(raw, 0);
        entry.track_id = sqlite3_column_int64(raw, 1);
        entry.started_at = sqlite3_column_int64(raw, 2);
        if (sqlite3_column_type(raw, 3) != SQLITE_NULL)
            entry.completed_at = sqlite3_column_int64(raw, 3);
        entry.position_ms = sqlite3_column_int64(raw, 4);
        entry.completion_pct = sqlite3_column_double(raw, 5);
        entry.queue_id = sqlite3_column_int64(raw, 6);
        auto text = [raw](int column) {
            const auto* value = sqlite3_column_text(raw, column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };
        entry.title = text(7);
        entry.artist = text(8);
        entry.path = text(9);
        entry.duration = sqlite3_column_double(raw, 10);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::expected<void, kanariv::utils::Error> Database::engineHistoryClear() {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    char* error = nullptr;
    internal::SqliteErrGuard errorGuard(error);
    if (sqlite3_exec(handle, "DELETE FROM history", nullptr, nullptr, &error) != SQLITE_OK)
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        error ? error : "clear history failed"));
    return {};
}

} // namespace kanariv::db
