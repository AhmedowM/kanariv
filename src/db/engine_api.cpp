#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <db/database_access.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <functional>
#include <kanariv/db/database.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace kanariv::db {

constexpr int colShuffleEnabled = 0;
constexpr int colRepeatMode = 1;
constexpr int colCursorPos = 2;
constexpr int colCurrentTrackId = 3;
constexpr int colVolume = 4;
constexpr int colShufflePerm = 5;
constexpr int colActiveQueueId = 6;
constexpr int colHistoryTrackId = 1;
constexpr int colHistoryStartedAt = 2;
constexpr int colHistoryCompletedAt = 3;
constexpr int colHistoryPositionMs = 4;
constexpr int colHistoryCompletionPct = 5;
constexpr int colHistoryQueueId = 6;
constexpr int colHistoryTitle = 7;
constexpr int colHistoryArtist = 8;
constexpr int colHistoryPath = 9;
constexpr int colHistoryDuration = 10;

constexpr int bindShuffleEnabled = 1;
constexpr int bindRepeatMode = 2;
constexpr int bindShufflePerm = 3;
constexpr int bindCursorPos = 4;
constexpr int bindCurrentTrackId = 5;
constexpr int bindVolume = 6;
constexpr int bindActiveQueueId = 7;
constexpr int bindPositionMs = 8;
constexpr int bindPlayCount = 1;
constexpr int bindLastPlayed = 2;
constexpr int bindTrackId = 3;
constexpr int bindHistoryTrackId = 1;
constexpr int bindHistoryStartedAt = 2;
constexpr int bindHistoryCompletedAt = 3;
constexpr int bindHistoryPositionMs = 4;
constexpr int bindHistoryCompletionPct = 5;
constexpr int bindHistoryQueueId = 6;

constexpr float minVolume = 0.0f;
constexpr float maxVolume = 1.0f;
constexpr float defaultVolume = 1.0f;
constexpr int64_t defaultQueueId = 1;
constexpr int64_t defaultActiveQueueId = 1;

auto Database::queueExists(int64_t qid) -> std::expected<bool, kanariv::utils::Error> {
    auto queue = getQueue(qid);
    if (!queue) {
        if (queue.error().code == kanariv::utils::StatusCode::NotFound) {
            return false;
        }
        return std::unexpected(queue.error());
    }
    return true;
}

auto Database::playbackStats()
    -> std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> {
    auto stats = getStats();
    if (!stats) {
        return std::unexpected(stats.error());
    }
    return kanariv::types::PlaybackStats{.num_tracks = stats->num_tracks,
                                         .num_playlists = stats->num_playlists,
                                         .num_queue_items = stats->num_queue_items,
                                         .num_history = stats->num_history,
                                         .num_bookmarks = stats->num_bookmarks,
                                         .num_libraries = stats->num_libraries,
                                         .total_duration_ms = stats->total_duration_ms};
}

auto Database::isOpen() const noexcept -> bool {
    return DatabaseAccess::handle(const_cast<Database&>(*this)) != nullptr;
}

auto Database::queueTrackAt(int64_t qid, int64_t position)
    -> std::expected<kanariv::types::Track, kanariv::utils::Error> {
    if (qid == 0) {
        qid = 1;
    }
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    int64_t trackId = 0;
    {
        std::shared_lock lock(DatabaseAccess::mutex(*this));
        sqlite3_stmt* raw = nullptr;
        const char* sql =
            "SELECT track_id FROM queue WHERE queue_id=? ORDER BY position LIMIT 1 OFFSET ?";
        if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                             sqlite3_errmsg(handle)));
        }
        internal::StmtGuard statement(raw);
        sqlite3_bind_int64(raw, 1, qid);
        sqlite3_bind_int64(raw, 2, position);
        if (sqlite3_step(raw) != SQLITE_ROW) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "not found"));
        }
        trackId = sqlite3_column_int64(raw, 0);
    }
    return getTrack(trackId);
}

// Forward declarations for loadEngineState helpers.
static auto tryPrepareEngineState(sqlite3* handle, bool& hasActiveQueue) -> sqlite3_stmt*;
static void readEngineStateMainColumns(sqlite3_stmt* raw, kanariv::types::EngineState& state,
                                       bool hasActiveQueue);
static void readOptionalFloatColumn(sqlite3* handle, const char* sql,
                                    std::function<void(float)> setter);
static void readOptionalInt64Column(sqlite3* handle, const char* sql,
                                    std::function<void(int64_t)> setter);
static void validateActiveQueue(sqlite3* handle, kanariv::types::EngineState& state);

auto Database::loadEngineState()
    -> std::expected<kanariv::types::EngineState, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::shared_lock lock(DatabaseAccess::mutex(*this));

    bool hasActiveQueue = true;
    sqlite3_stmt* raw = tryPrepareEngineState(handle, hasActiveQueue);
    if (raw == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    }
    internal::StmtGuard statement(raw);

    kanariv::types::EngineState state;
    int rc = sqlite3_step(raw);
    if (rc == SQLITE_DONE) {
        return state;
    }
    if (rc != SQLITE_ROW) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "load failed"));
    }

    readEngineStateMainColumns(raw, state, hasActiveQueue);

    readOptionalFloatColumn(handle, "SELECT pre_mute_volume FROM engine_state WHERE id=1",
                            [&state](float v) -> void { state.pre_mute_volume = v; });

    readOptionalInt64Column(handle, "SELECT position_ms FROM engine_state WHERE id=1",
                            [&state](int64_t v) -> void {
                                v = std::max<int64_t>(v, 0);
                                state.position_ms = v;
                            });

    validateActiveQueue(handle, state);

    return state;
}

// Tries the new engine_state schema (with active_queue_id); falls back to the
// old schema if the column does not exist. Returns a prepared statement or
// nullptr on total failure. Sets hasActiveQueue to false when using old schema.
static auto tryPrepareEngineState(sqlite3* handle, bool& hasActiveQueue) -> sqlite3_stmt* {
    const char* sqlNew = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, "
                         "volume, shuffle_perm, active_queue_id FROM engine_state WHERE id=1";
    const char* sqlOld = "SELECT shuffle_enabled, repeat_mode, cursor_pos, current_track_id, "
                         "volume, shuffle_perm FROM engine_state WHERE id=1";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, sqlNew, -1, &raw, nullptr) == SQLITE_OK) {
        return raw;
    }
    sqlite3_finalize(raw);
    raw = nullptr;
    hasActiveQueue = false;
    if (sqlite3_prepare_v2(handle, sqlOld, -1, &raw, nullptr) == SQLITE_OK) {
        return raw;
    }
    return nullptr;
}

// Reads the fixed columns from a stepped engine_state row, including the
// shuffle_perm blob when present and enabled.
static void readEngineStateMainColumns(sqlite3_stmt* raw, kanariv::types::EngineState& state,
                                       bool hasActiveQueue) {
    state.shuffle_enabled = sqlite3_column_int(raw, colShuffleEnabled);
    state.repeat_mode = static_cast<kanariv::types::RepeatMode>(sqlite3_column_int(raw, colRepeatMode));
    state.cursor_pos = sqlite3_column_int64(raw, colCursorPos);
    state.current_track_id = sqlite3_column_int64(raw, colCurrentTrackId);
    state.volume = static_cast<float>(sqlite3_column_double(raw, colVolume));
    if (state.volume < minVolume || state.volume > maxVolume) {
        state.volume = defaultVolume;
    }
    if (hasActiveQueue) {
        state.active_queue_id = sqlite3_column_int64(raw, colActiveQueueId);
        if (state.active_queue_id <= 0) {
            state.active_queue_id = defaultActiveQueueId;
        }
    }
    const auto* blob = static_cast<const std::byte*>(sqlite3_column_blob(raw, colShufflePerm));
    const int blobBytes = sqlite3_column_bytes(raw, colShufflePerm);
    if (blob != nullptr && blobBytes >= static_cast<int>(sizeof(int64_t)) && state.shuffle_enabled != 0) {
        const size_t count = static_cast<size_t>(blobBytes) / sizeof(int64_t);
        state.shuffle_perm.resize(count);
        std::memcpy(state.shuffle_perm.data(), blob, count * sizeof(int64_t));
    }
}

// Reads one optional float column; applies it only when in [0, 1].
static void readOptionalFloatColumn(sqlite3* handle, const char* sql,
                                    std::function<void(float)> setter) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    internal::StmtGuard guard(stmt);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto v = static_cast<float>(sqlite3_column_double(stmt, 0));
        if (v > 0.0f && v <= 1.0f) {
            setter(v);
        }
    }
}

// Reads one optional int64 column; applies it only when present.
static void readOptionalInt64Column(sqlite3* handle, const char* sql,
                                    std::function<void(int64_t)> setter) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    internal::StmtGuard guard(stmt);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        setter(sqlite3_column_int64(stmt, 0));
    }
}

// Ensures active_queue_id refers to an existing queue; resets to 1 if not.
static void validateActiveQueue(sqlite3* handle, kanariv::types::EngineState& state) {
    sqlite3_stmt* queue = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT id FROM queues WHERE id=?", -1, &queue, nullptr) !=
        SQLITE_OK) {
        return;
    }
    internal::StmtGuard queueStatement(queue);
    sqlite3_bind_int64(queue, 1, state.active_queue_id);
    if (sqlite3_step(queue) != SQLITE_ROW) {
        state.active_queue_id = 1;
    }
}

auto Database::saveEngineState(const kanariv::types::EngineState& state)
    -> std::expected<void, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
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
        if (sqlite3_prepare_v2(handle, sqlOld, -1, &raw, nullptr) != SQLITE_OK) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        }
    }
    internal::StmtGuard statement(raw);
    if (state.shuffle_perm.size() >
        static_cast<size_t>(std::numeric_limits<int>::max() / static_cast<int>(sizeof(int64_t)))) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem, "perm too large"));
    }
    sqlite3_bind_int(raw, bindShuffleEnabled, state.shuffle_enabled);
    sqlite3_bind_int(raw, bindRepeatMode, static_cast<int>(state.repeat_mode));
    if (state.shuffle_enabled != 0 && !state.shuffle_perm.empty()) {
        sqlite3_bind_blob(raw, bindShufflePerm, state.shuffle_perm.data(),
                          static_cast<int>(state.shuffle_perm.size() * sizeof(int64_t)),
                          SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(raw, bindShufflePerm);
    }
    sqlite3_bind_int64(raw, bindCursorPos, state.cursor_pos);
    sqlite3_bind_int64(raw, bindCurrentTrackId, state.current_track_id);
    sqlite3_bind_double(raw, bindVolume, static_cast<double>(state.volume));
    if (hasNewColumns) {
        sqlite3_bind_int64(raw, bindActiveQueueId, state.active_queue_id > 0 ? state.active_queue_id : defaultActiveQueueId);
        sqlite3_bind_int64(raw, bindPositionMs, state.position_ms >= 0 ? state.position_ms : 0);
    }
    if (sqlite3_step(raw) != SQLITE_DONE) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    }
    sqlite3_stmt* optional = nullptr;
    if (sqlite3_prepare_v2(handle, "UPDATE engine_state SET pre_mute_volume=? WHERE id=1", -1,
                           &optional, nullptr) == SQLITE_OK) {
        internal::StmtGuard optionalStatement(optional);
        sqlite3_bind_double(optional, 1, static_cast<double>(state.pre_mute_volume));
        (void)sqlite3_step(optional);
    }
    return transaction->commit();
}

auto Database::saveEngineShuffle(bool enabled, std::span<const int64_t> permutation,
                                 int64_t cursorPosition)
    -> std::expected<void, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    if (permutation.size() >
        static_cast<size_t>(std::numeric_limits<int>::max() / static_cast<int>(sizeof(int64_t)))) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem, "perm too large"));
    }
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    sqlite3_stmt* raw = nullptr;
    const char* sql = "UPDATE engine_state SET shuffle_perm=?, cursor_pos=?, "
                      "shuffle_enabled=? WHERE id=1";
    if (sqlite3_prepare_v2(handle, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    }
    internal::StmtGuard statement(raw);
    if (enabled && !permutation.empty()) {
        sqlite3_bind_blob(raw, 1, permutation.data(),
                          static_cast<int>(permutation.size() * sizeof(int64_t)), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(raw, 1);
    }
    sqlite3_bind_int64(raw, 2, cursorPosition);
    sqlite3_bind_int(raw, bindShufflePerm, enabled ? 1 : 0);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    }
    return transaction->commit();
}

auto Database::saveEngineCursor(int64_t cursorPosition)
    -> std::expected<void, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, "UPDATE engine_state SET cursor_pos=? WHERE id=1", -1, &raw,
                           nullptr) != SQLITE_OK) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
    }
    internal::StmtGuard statement(raw);
    sqlite3_bind_int64(raw, 1, cursorPosition);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "step failed"));
    }
    return transaction->commit();
}

auto Database::markPlayed(int64_t trackId, int64_t startedAt, int64_t completedAt,
                          int64_t positionMs, double completionPct, int64_t queueId)
    -> std::expected<void, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    auto transaction = DbTransaction::begin(handle);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    {
        sqlite3_stmt* select = nullptr;
        if (sqlite3_prepare_v2(handle, "SELECT play_count FROM tracks WHERE id=?", -1, &select,
                               nullptr) != SQLITE_OK) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        }
        internal::StmtGuard selectStatement(select);
        sqlite3_bind_int64(select, bindPlayCount, trackId);
        if (sqlite3_step(select) != SQLITE_ROW) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "track not found"));
        }
        const int64_t playCount = sqlite3_column_int64(select, 0) + 1;
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(handle, "UPDATE tracks SET play_count=?, last_played=? WHERE id=?",
                               -1, &update, nullptr) != SQLITE_OK) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        }
        internal::StmtGuard updateStatement(update);
        sqlite3_bind_int64(update, bindPlayCount, playCount);
        sqlite3_bind_int64(update, bindLastPlayed, completedAt);
        sqlite3_bind_int64(update, bindTrackId, trackId);
        if (sqlite3_step(update) != SQLITE_DONE) {
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                             "track update failed"));
        }
    }
    {
        sqlite3_stmt* insert = nullptr;
        const char* sql = "INSERT INTO history (track_id, started_at, completed_at, position_ms, "
                          "completion_pct, queue_id) VALUES (?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(handle, sql, -1, &insert, nullptr) != SQLITE_OK) {
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "prepare failed"));
        }
        internal::StmtGuard insertStatement(insert);
        sqlite3_bind_int64(insert, bindHistoryTrackId, trackId);
        sqlite3_bind_int64(insert, bindHistoryStartedAt, startedAt);
        sqlite3_bind_int64(insert, bindHistoryCompletedAt, completedAt);
        sqlite3_bind_int64(insert, bindHistoryPositionMs, positionMs);
        sqlite3_bind_double(insert, bindHistoryCompletionPct, completionPct);
        sqlite3_bind_int64(insert, bindHistoryQueueId, queueId > 0 ? queueId : defaultQueueId);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                             "history insert failed"));
        }
    }
    return transaction->commit();
}

auto Database::engineHistoryList(int limit)
    -> std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::shared_lock lock(DatabaseAccess::mutex(*this));
    std::string sql = "SELECT h.id, h.track_id, h.started_at, h.completed_at, h.position_ms, "
                      "h.completion_pct, h.queue_id, t.title, t.artist, t.path, t.duration "
                      "FROM history h JOIN tracks t ON t.id=h.track_id "
                      "ORDER BY h.started_at DESC";
    if (limit > 0) {
        sql += " LIMIT ?";
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(handle)));
    }
    internal::StmtGuard statement(raw);
    if (limit > 0) {
        sqlite3_bind_int(raw, 1, limit);
    }
    std::vector<kanariv::types::HistoryEntry> entries;
    while (sqlite3_step(raw) == SQLITE_ROW) {
        kanariv::types::HistoryEntry entry;
        entry.id = sqlite3_column_int64(raw, colHistoryTrackId);
        entry.track_id = sqlite3_column_int64(raw, colHistoryTrackId);
        entry.started_at = sqlite3_column_int64(raw, colHistoryStartedAt);
        if (sqlite3_column_type(raw, colHistoryCompletedAt) != SQLITE_NULL) {
            entry.completed_at = sqlite3_column_int64(raw, colHistoryCompletedAt);
        }
        entry.position_ms = sqlite3_column_int64(raw, colHistoryPositionMs);
        entry.completion_pct = sqlite3_column_double(raw, colHistoryCompletionPct);
        entry.queue_id = sqlite3_column_int64(raw, colHistoryQueueId);
        auto text = [raw](int column) -> const char* {
            const auto* value = sqlite3_column_text(raw, column);
            return value ? reinterpret_cast<const char*>(value)
                         : ""; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        };
        entry.title = text(colHistoryTitle);
        entry.artist = text(colHistoryArtist);
        entry.path = text(colHistoryPath);
        entry.duration = sqlite3_column_double(raw, colHistoryDuration);
        entries.push_back(std::move(entry));
    }
    return entries;
}

auto Database::engineHistoryClear() -> std::expected<void, kanariv::utils::Error> {
    sqlite3* handle = DatabaseAccess::handle(*this);
    if (handle == nullptr) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db"));
    }
    std::unique_lock lock(DatabaseAccess::mutex(*this));
    char* error = nullptr;
    internal::SqliteErrGuard errorGuard(error);
    if (sqlite3_exec(handle, "DELETE FROM history", nullptr, nullptr, &error) != SQLITE_OK) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         error ? error : "clear history failed"));
    }
    return {};
}

} // namespace kanariv::db
