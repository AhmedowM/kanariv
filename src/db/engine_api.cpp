#include <sqlite3.h>

#include <cstddef>
#include <cstring>
#include <db/database_access.hpp>
#include <db/queue.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <mutex>
#include <optional>
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
                                                                                   int64_t index) {
    qid = normalizeQueueId(qid);
    if (index < 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "not found"));
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    int64_t trackId = 0;
    {
        std::unique_lock lock(DatabaseAccess::mutex(*this));
        sqlite3_stmt* raw = nullptr;
        const char* sql =
            "SELECT track_id FROM queue WHERE queue_id=? ORDER BY position LIMIT 1 OFFSET ?";
        if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                             sqlite3_errmsg(handle))};
        internal::StmtGuard statement(raw);
        sqlite3_bind_int64(raw, 1, qid);
        sqlite3_bind_int64(raw, 2, index);
        if (sqlite3_step(raw) != SQLITE_ROW)
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "not found"));
        trackId = sqlite3_column_int64(raw, 0);
    }
    return getTrack(trackId);
}

std::optional<kanariv::types::EngineState> Database::legacyEngineState() {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::nullopt;
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    sqlite3_stmt* raw = nullptr;
    const char* sql = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, volume, "
                      "shuffle_perm, active_queue_id, pre_mute_volume, position_ms "
                      "FROM engine_state WHERE id=1";
    if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite3_finalize(raw);
        raw = nullptr;
        // Pre-source-identity databases lack the last three columns.
        sql = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, volume, "
              "shuffle_perm FROM engine_state WHERE id=1";
        if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK) {
            sqlite3_finalize(raw);
            return std::nullopt;
        }
    }
    internal::StmtGuard statement(raw);
    if (sqlite3_step(raw) != SQLITE_ROW)
        return std::nullopt;
    kanariv::types::EngineState state;
    state.shuffle_enabled = sqlite3_column_int(raw, 0) != 0;
    const int repeat = sqlite3_column_int(raw, 1);
    if (repeat == static_cast<int>(kanariv::types::RepeatMode::One) ||
        repeat == static_cast<int>(kanariv::types::RepeatMode::All))
        state.repeat_mode = static_cast<kanariv::types::RepeatMode>(repeat);
    const int64_t cursor = sqlite3_column_int64(raw, 2);
    state.cursor_pos = cursor < 0 ? 0 : cursor;
    const int64_t trackId = sqlite3_column_int64(raw, 3);
    if (trackId > 0)
        state.current_track_id = trackId;
    const float volume = static_cast<float>(sqlite3_column_double(raw, 4));
    state.volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(raw, 5));
    const int blobBytes = sqlite3_column_bytes(raw, 5);
    if (blob && blobBytes >= static_cast<int>(sizeof(int64_t))) {
        const size_t count = static_cast<size_t>(blobBytes) / sizeof(int64_t);
        state.shuffle_perm.resize(count);
        std::memcpy(state.shuffle_perm.data(), blob, count * sizeof(int64_t));
    }
    if (sqlite3_column_count(raw) >= 9) {
        const int64_t qid = sqlite3_column_int64(raw, 6);
        const float preMute = static_cast<float>(sqlite3_column_double(raw, 7));
        if (preMute > 0.0f && preMute <= 1.0f)
            state.pre_mute_volume = preMute;
        const int64_t position = sqlite3_column_int64(raw, 8);
        state.position_ms = position < 0 ? 0 : position;
        if (qid > 0) {
            sqlite3_stmt* queue = nullptr;
            if (sqlite3_prepare_v2(handle, "SELECT name FROM queues WHERE id=?", -1, &queue,
                                   nullptr) == SQLITE_OK) {
                internal::StmtGuard queueStatement(queue);
                sqlite3_bind_int64(queue, 1, qid);
                if (sqlite3_step(queue) == SQLITE_ROW) {
                    const auto* name = sqlite3_column_text(queue, 0);
                    if (name)
                        state.queue_name = reinterpret_cast<const char*>(name);
                }
            }
        }
    }
    if (state.queue_name.empty())
        state.queue_name = "default";
    if (!state.current_track_id)
        return state;
    sqlite3_stmt* track = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT path FROM tracks WHERE id=?", -1, &track, nullptr) ==
        SQLITE_OK) {
        internal::StmtGuard trackStatement(track);
        sqlite3_bind_int64(track, 1, *state.current_track_id);
        if (sqlite3_step(track) == SQLITE_ROW) {
            const auto* path = sqlite3_column_text(track, 0);
            if (path)
                state.current_path = reinterpret_cast<const char*>(path);
        }
    }
    return state;
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
Database::listHistory(int limit) {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (!handle)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    std::string sql = "SELECT h.id, h.track_id, h.started_at, h.completed_at, h.position_ms, "
                      "h.completion_pct, h.queue_id, t.title, t.artist, t.path, t.duration "
                      "FROM history h JOIN tracks t ON t.id=h.track_id "
                      "ORDER BY h.started_at DESC";
    if (limit > 0)
        sql += " LIMIT ?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(handle)));
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

} // namespace kanariv::db
