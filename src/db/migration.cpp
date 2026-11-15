#include <sqlite3.h>

#include <array>
#include <db/migration.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <tuple>

namespace kanariv::db::internal {

namespace {

using kanariv::utils::makeError;
using kanariv::utils::StatusCode;

/**
 * @brief Connection-level pragmas applied before any migration transaction.
 * @details `journal_mode` and `page_size` cannot change inside a transaction,
 * so these run once on the raw handle in `Database::open()`.
 */
inline constexpr std::string_view kConnectionPragmas = "PRAGMA journal_mode=WAL;"
                                                       "PRAGMA synchronous=NORMAL;"
                                                       "PRAGMA cache_size=-8000;"
                                                       "PRAGMA busy_timeout=5000;"
                                                       "PRAGMA page_size=4096;"
                                                       "PRAGMA foreign_keys=ON;";

/**
 * @brief Base schema: every table, index and trigger that exists in version 1.
 * @details `CREATE ... IF NOT EXISTS` makes this step safe for databases that
 * were created before versioning: tables missing because the old single-script
 * initialisation aborted are created here. The queue uniqueness index is not
 * part of this step -- legacy rows may hold duplicate positions and are
 * normalised by version 2 first. The row-update trigger is created by version 4.
 */
inline constexpr std::string_view kBaseSchema =
    "CREATE TABLE IF NOT EXISTS libraries ("
    "id INTEGER PRIMARY KEY,"
    "path TEXT NOT NULL UNIQUE,"
    "name TEXT,"
    "date_added DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "last_scanned DATETIME,"
    "auto_scan BOOLEAN DEFAULT 1,"
    "recursive BOOLEAN DEFAULT 1,"
    "extensions TEXT DEFAULT 'mp3,flac,ogg,wav,m4a'"
    ");"
    "CREATE TABLE IF NOT EXISTS tracks ("
    "id INTEGER PRIMARY KEY,"
    "fingerprint BLOB(32) NOT NULL UNIQUE,"
    "path TEXT NOT NULL,"
    "deleted_at DATETIME,"
    "size INTEGER,"
    "mtime INTEGER,"
    "duration REAL,"
    "sample_rate INTEGER,"
    "channels INTEGER,"
    "bitrate INTEGER,"
    "title TEXT,"
    "artist TEXT,"
    "album TEXT,"
    "album_artist TEXT,"
    "genre TEXT,"
    "year INTEGER,"
    "track_num INTEGER,"
    "disc_num INTEGER,"
    "cover_art_path TEXT,"
    "rating INTEGER DEFAULT 0,"
    "play_count INTEGER DEFAULT 0,"
    "last_played DATETIME,"
    "date_added DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "last_scanned DATETIME,"
    "dirty BOOLEAN DEFAULT 0,"
    "library_id INTEGER DEFAULT 1 REFERENCES libraries(id),"
    "source_kind INTEGER NOT NULL DEFAULT 0,"
    "source_key TEXT NOT NULL DEFAULT '',"
    "source_available BOOLEAN NOT NULL DEFAULT 1,"
    "source_display_name TEXT,"
    "replay_gain_track REAL DEFAULT 0,"
    "replay_gain_album REAL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tracks_fingerprint ON tracks(fingerprint);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_artist_album ON tracks(artist, album);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_library ON tracks(library_id);"
    "CREATE TABLE IF NOT EXISTS playlists ("
    "id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL,"
    "type INTEGER NOT NULL DEFAULT 0,"
    "smart_query TEXT,"
    "created DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "modified DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "library_id INTEGER DEFAULT 1 REFERENCES libraries(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS playlist_items ("
    "playlist_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,"
    "track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,"
    "position INTEGER NOT NULL,"
    "added DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "PRIMARY KEY (playlist_id, track_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_playlist_items_pos ON playlist_items(playlist_id, position);"
    "CREATE TABLE IF NOT EXISTS queue ("
    "id INTEGER PRIMARY KEY,"
    "queue_id INTEGER NOT NULL DEFAULT 1,"
    "track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,"
    "position INTEGER NOT NULL,"
    "added DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "UNIQUE(queue_id, position)"
    ");"
    "INSERT OR IGNORE INTO libraries (id, path, name) VALUES (1, '', 'default');"
    "CREATE TABLE IF NOT EXISTS queues ("
    "id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL,"
    "repeat_mode INTEGER DEFAULT 0,"
    "temp INTEGER DEFAULT 0,"
    "library_id INTEGER DEFAULT 1 REFERENCES libraries(id)"
    ");"
    "INSERT OR IGNORE INTO queues (id, name, repeat_mode, library_id) VALUES (1, 'default', 0, 1);"
    "CREATE TABLE IF NOT EXISTS history ("
    "id INTEGER PRIMARY KEY,"
    "track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,"
    "started_at DATETIME NOT NULL,"
    "completed_at DATETIME,"
    "position_ms INTEGER,"
    "completion_pct REAL,"
    "queue_id INTEGER DEFAULT 1"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_history_track_started ON history(track_id, started_at);"
    "CREATE TABLE IF NOT EXISTS bookmarks ("
    "id INTEGER PRIMARY KEY,"
    "track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,"
    "position_ms INTEGER NOT NULL,"
    "note TEXT,"
    "created DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE TABLE IF NOT EXISTS lyrics ("
    "id INTEGER PRIMARY KEY,"
    "track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,"
    "lrc_text TEXT,"
    "is_synced BOOLEAN DEFAULT 0,"
    "source TEXT,"
    "fetched_at DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE TABLE IF NOT EXISTS eq_presets ("
    "id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL UNIQUE,"
    "bands_json TEXT NOT NULL"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
    "title, artist, album, album_artist, genre,"
    "content='tracks', content_rowid='id', tokenize='porter unicode61'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS tracks_ai AFTER INSERT ON tracks BEGIN "
    "INSERT INTO tracks_fts(rowid, title, artist, album, album_artist, genre) "
    "VALUES (new.id, new.title, new.artist, new.album, new.album_artist, new.genre);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS tracks_ad AFTER DELETE ON tracks BEGIN "
    "INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, album_artist, genre) "
    "VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist, old.genre);"
    "END;";

/**
 * @brief Renumbers every queue to full gap positions, then enforces uniqueness.
 * @details The stored ordering value is internal (public queue APIs take
 * zero-based indices), so renumbering preserves observable order while giving
 * every row a unique `(queue_id, position)`. The rank is computed once into a
 * temporary table so the correlated UPDATE cannot see its own intermediate rows.
 */
inline constexpr std::string_view kQueuePositionUniqueness =
    "CREATE TEMP TABLE IF NOT EXISTS queue_rank AS "
    "SELECT id, (ROW_NUMBER() OVER (PARTITION BY queue_id ORDER BY position, id) - 1) * 1000000000 "
    "AS pos FROM queue;"
    "UPDATE queue SET position = (SELECT pos FROM queue_rank WHERE queue_rank.id = queue.id);"
    "DROP TABLE queue_rank;"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_queue_queue_pos ON queue(queue_id, position);";

/**
 * @brief Row-update trigger scoped to the columns `tracks_fts` indexes.
 * @details `UPDATE OF` fires only when a statement names one of these columns,
 * so play-count, rating, dirty and scan writes no longer rewrite FTS rows.
 * The trigger is dropped first because `CREATE TRIGGER IF NOT EXISTS` would
 * keep an older, unscoped definition on an existing database.
 */
inline constexpr std::string_view kScopedFtsUpdateTrigger =
    "DROP TRIGGER IF EXISTS tracks_au;"
    "CREATE TRIGGER tracks_au AFTER UPDATE OF title, artist, album, album_artist, genre ON tracks "
    "BEGIN "
    "INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, album_artist, genre) "
    "VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist, old.genre);"
    "INSERT INTO tracks_fts(rowid, title, artist, album, album_artist, genre) "
    "VALUES (new.id, new.title, new.artist, new.album, new.album_artist, new.genre);"
    "END;";

/**
 * @brief Version 6: drops indexes that duplicate a table constraint.
 * @details `tracks.fingerprint` and `queue(queue_id, position)` already carry
 * `UNIQUE` constraints, so their separate indexes only add write
 * amplification. `idx_tracks_path` stays non-unique on purpose: identity is
 * `(source_kind, source_key)` since version 3, and making path unique would
 * reject rows instead of letting the scanner resolve them.
 */
inline constexpr std::string_view kDropRedundantIndexes =
    "DROP INDEX IF EXISTS idx_tracks_fingerprint;"
    "DROP INDEX IF EXISTS idx_queue_queue_pos;";

std::expected<void, kanariv::utils::Error> execSql(sqlite3* db, std::string_view sql,
                                                   std::string_view context) {
    char* err = nullptr;
    SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
        return std::unexpected{makeError(
            StatusCode::Corrupt, std::string(context) + ": " + (err ? err : sqlite3_errmsg(db)))};
    return {};
}

std::expected<void, kanariv::utils::Error> setUserVersion(sqlite3* db, int version,
                                                          std::string_view context) {
    return execSql(db, "PRAGMA user_version = " + std::to_string(version), context);
}

std::expected<bool, kanariv::utils::Error> hasColumn(sqlite3* db, std::string_view table,
                                                     std::string_view column) {
    std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return std::unexpected{makeError(StatusCode::Corrupt, sqlite3_errmsg(db))};
    StmtGuard guard{raw};
    while (sqlite3_step(raw) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(raw, 1));
        if (name && column == name)
            return true;
    }
    return false;
}

std::expected<void, kanariv::utils::Error> ensureColumn(sqlite3* db, std::string_view table,
                                                        std::string_view column,
                                                        std::string_view alterSql) {
    auto exists = hasColumn(db, table, column);
    if (!exists)
        return std::unexpected{exists.error()};
    if (*exists)
        return {};
    return execSql(db, alterSql, "add column " + std::string(table) + '.' + std::string(column));
}

/**
 * @brief Version 3: columns and identity backfill for pre-source-identity files.
 * @details Idempotent because legacy databases at `user_version == 0` may
 * already hold any subset of these columns from the old unconditional ALTERs.
 */
std::expected<void, kanariv::utils::Error> migrateSourceIdentity(sqlite3* db) {
    for (const auto& [table, column, alter] :
         std::array<std::tuple<std::string_view, std::string_view, std::string_view>, 7>{
             {std::tuple{"queues", "temp", "ALTER TABLE queues ADD COLUMN temp INTEGER DEFAULT 0"},
              std::tuple{"tracks", "source_kind",
                         "ALTER TABLE tracks ADD COLUMN source_kind INTEGER NOT NULL DEFAULT 0"},
              std::tuple{"tracks", "source_key",
                         "ALTER TABLE tracks ADD COLUMN source_key TEXT NOT NULL DEFAULT ''"},
              std::tuple{
                  "tracks", "source_available",
                  "ALTER TABLE tracks ADD COLUMN source_available BOOLEAN NOT NULL DEFAULT 1"},
              std::tuple{"tracks", "source_display_name",
                         "ALTER TABLE tracks ADD COLUMN source_display_name TEXT"},
              std::tuple{"tracks", "replay_gain_track",
                         "ALTER TABLE tracks ADD COLUMN replay_gain_track REAL DEFAULT 0"},
              std::tuple{"tracks", "replay_gain_album",
                         "ALTER TABLE tracks ADD COLUMN replay_gain_album REAL DEFAULT 0"}}}) {
        if (auto r = ensureColumn(db, table, column, alter); !r)
            return r;
    }
    // Existing rows were path-only. Give them an explicit source identity so
    // (source_kind, source_key) deduplication works without rewriting paths.
    if (auto r = execSql(db,
                         "UPDATE tracks SET source_key=path, source_display_name=path "
                         "WHERE source_kind=0 AND source_key=''",
                         "backfill track source identity");
        !r) {
        return r;
    }
    return execSql(
        db, "CREATE INDEX IF NOT EXISTS idx_tracks_source ON tracks(source_kind, source_key)",
        "create track source index");
}

} // namespace

std::expected<void, kanariv::utils::Error> configureConnection(sqlite3* db) {
    if (!db)
        return std::unexpected{makeError(StatusCode::InvalidArg, "null db")};
    return execSql(db, kConnectionPragmas, "connection pragmas");
}

std::span<const MigrationStep> schemaMigrations() noexcept {
    static constexpr std::array<MigrationStep, 6> steps{{
        MigrationStep{1, "base schema", kBaseSchema, nullptr},
        MigrationStep{2, "queue position uniqueness", kQueuePositionUniqueness, nullptr},
        MigrationStep{3, "track source identity", {}, &migrateSourceIdentity},
        MigrationStep{4, "scoped fts update trigger", kScopedFtsUpdateTrigger, nullptr},
        // Older scans wrote a mis-scaled loudness heuristic into both gain
        // columns; the scanner no longer produces gain values at all (M10), so
        // the stale numbers are cleared rather than reinterpreted. The columns
        // are reserved for tag-imported values, which nothing writes yet.
        MigrationStep{5, "clear mis-scaled replay gain",
                      "UPDATE tracks SET replay_gain_track=0, replay_gain_album=0", nullptr},
        MigrationStep{6, "drop redundant indexes", kDropRedundantIndexes, nullptr},
    }};
    return steps;
}

std::expected<int, kanariv::utils::Error> readUserVersion(sqlite3* db) {
    if (!db)
        return std::unexpected{makeError(StatusCode::InvalidArg, "null db")};
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &raw, nullptr) != SQLITE_OK)
        return std::unexpected{makeError(StatusCode::Corrupt, sqlite3_errmsg(db))};
    StmtGuard guard{raw};
    if (sqlite3_step(raw) != SQLITE_ROW)
        return std::unexpected{makeError(StatusCode::Corrupt, "cannot read user_version")};
    return sqlite3_column_int(raw, 0);
}

std::expected<void, kanariv::utils::Error> applyMigrations(sqlite3* db,
                                                           std::span<const MigrationStep> steps) {
    if (!db)
        return std::unexpected{makeError(StatusCode::InvalidArg, "null db")};
    auto stored = readUserVersion(db);
    if (!stored)
        return std::unexpected{stored.error()};
    int current = *stored;
    const int latest = steps.empty() ? 0 : steps.back().version;
    if (current > latest) {
        return std::unexpected{makeError(
            StatusCode::Corrupt, "database schema version " + std::to_string(current) +
                                     " is newer than supported version " + std::to_string(latest))};
    }
    for (const auto& step : steps) {
        if (step.version <= current)
            continue;
        const std::string context = "schema migration " + std::to_string(step.version) + " (" +
                                    std::string(step.name) + ")";
        auto tx = DbTransaction::begin(db);
        if (!tx) {
            return std::unexpected{makeError(tx.error().code, context + ": " + tx.error().message)};
        }
        // Any error return below destroys `tx`, which rolls the step back.
        if (step.apply) {
            if (auto r = step.apply(db); !r)
                return std::unexpected{
                    makeError(StatusCode::Corrupt, context + ": " + r.error().message)};
        } else if (!step.sql.empty()) {
            if (auto r = execSql(db, step.sql, context); !r)
                return std::unexpected{r.error()};
        }
        if (auto r = setUserVersion(db, step.version, context); !r)
            return std::unexpected{r.error()};
        if (auto committed = tx->commit(); !committed) {
            return std::unexpected{
                makeError(committed.error().code, context + ": " + committed.error().message)};
        }
        current = step.version;
    }
    return {};
}

} // namespace kanariv::db::internal
