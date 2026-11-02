#include <sqlite3.h>

#include <array>
#include <algorithm>
#include <kanariv/db/database.hpp>
#include <kanariv/db/smart.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <db/database_access.hpp>
#include <db/fts.hpp>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <db/write_thread.hpp>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kanariv::db {

/**
 * @brief Closes an owned SQLite handle (defined in core.cpp).
 * @ingroup kanariv_db
 */
struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept;
};

inline constexpr std::string_view schema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA cache_size=-8000;"
    "PRAGMA busy_timeout=5000;"
    "PRAGMA page_size=4096;"
    "PRAGMA foreign_keys=ON;"
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
    "source_display_name TEXT"
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
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_queue_queue_pos ON queue(queue_id, position);"
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
    "END;"
    "CREATE TRIGGER IF NOT EXISTS tracks_au AFTER UPDATE ON tracks BEGIN "
    "INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, album_artist, genre) "
    "VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist, old.genre);"
    "INSERT INTO tracks_fts(rowid, title, artist, album, album_artist, genre) "
    "VALUES (new.id, new.title, new.artist, new.album, new.album_artist, new.genre);"
    "END;"
    "CREATE TABLE IF NOT EXISTS engine_state (id INTEGER PRIMARY KEY CHECK(id=1), "
    "shuffle_enabled INTEGER DEFAULT 0, repeat_mode INTEGER DEFAULT 0, shuffle_perm BLOB, "
    "cursor_pos INTEGER DEFAULT 0, current_track_id INTEGER DEFAULT 0, volume REAL DEFAULT 1.0, "
    "active_queue_id INTEGER DEFAULT 1, pre_mute_volume REAL DEFAULT 0.5, "
    "position_ms INTEGER DEFAULT 0, "
    "updated DATETIME DEFAULT CURRENT_TIMESTAMP);"
    "INSERT OR IGNORE INTO engine_state(id) VALUES (1);";

inline constexpr std::string_view schemaDefaultLibrary =
    "INSERT OR IGNORE INTO libraries (id, path, name) VALUES (1, '', 'default');";

inline constexpr std::string_view insertTrackSql =
    "INSERT INTO tracks (fingerprint, path, size, mtime, duration, sample_rate, channels, "
    "bitrate, title, artist, album, album_artist, genre, year, track_num, disc_num, "
    "cover_art_path, rating, play_count, last_played, date_added, last_scanned, dirty, "
    "library_id, source_kind, source_key, source_available, source_display_name) VALUES "
    "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
inline constexpr std::string_view updateTrackSql =
    "UPDATE tracks SET fingerprint=?, path=?, size=?, mtime=?, duration=?, sample_rate=?, "
    "channels=?, bitrate=?, title=?, artist=?, album=?, album_artist=?, genre=?, year=?, "
    "track_num=?, disc_num=?, cover_art_path=?, rating=?, play_count=?, last_played=?, "
    "date_added=?, last_scanned=?, dirty=?, library_id=?, source_kind=?, source_key=?, "
    "source_available=?, source_display_name=?, deleted_at=? WHERE id=?";

// Private implementation: the SQLite handle, statement cache, locks and the
// background writer live here so the public header stays include-light.
// Declaration order is load-bearing: reverse destruction stops the writer
// before the handle is closed and finalizes cached statements first.
struct Database::Impl {
    explicit Impl(std::size_t batchSize = 256) : writer_(batchSize) {}

    std::unique_ptr<sqlite3, SqliteCloser> db_;
    /// Non-owning log sink from DatabaseOptions (null = silent). See DatabaseOptions::logger.
    kanariv::utils::Logger* logger_{nullptr};
    // Lock order: dbMutex_ -> cacheMutex_.
    mutable std::shared_mutex dbMutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<SqliteStatement>> stmtCache_;
    mutable std::mutex cacheMutex_;
    WriterThread writer_;

    // Raw locking surface
    std::expected<SqliteStatement*, kanariv::utils::Error>
    getCachedForUse(std::string_view sql) const;
    sqlite3* handle() const noexcept;
    sqlite3* handleLocked() const noexcept;
    std::shared_mutex& mutex() const noexcept;

    // Track / library locked helpers
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByPathLocked(std::string_view path);
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findBySourceLocked(const kanariv::types::SourceRef& source);
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByFingerprintLocked(const std::array<uint8_t, 32>& fp);
    std::expected<int64_t, kanariv::utils::Error> insertTrackLocked(const kanariv::types::Track& t);
    std::expected<void, kanariv::utils::Error> updateTrackLocked(const kanariv::types::Track& t);
    std::expected<void, kanariv::utils::Error> deleteTrackLocked(int64_t id);
    std::expected<kanariv::types::Track, kanariv::utils::Error> getTrackLocked(int64_t id);
    std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error> libraryListLocked();
    std::expected<void, kanariv::utils::Error> libraryUpdateLocked(const kanariv::types::Library& l);

    // Playlist / queue locked helpers
    std::expected<int64_t, kanariv::utils::Error>
    createPlaylistLocked(std::string_view name, int type = 0, std::string_view smart_query = {});
    std::expected<void, kanariv::utils::Error> playlistAddTrackLocked(int64_t pid, int64_t tid,
                                                                     int64_t pos = -1);
    std::expected<void, kanariv::utils::Error> queueEnqueueLocked(int64_t qid, int64_t tid,
                                                                 int64_t pos = -1);
    std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeueLocked(int64_t qid);
    std::size_t queueCountLocked(int64_t qid);
};

// Free functions for track binding (static, no impl_ needed)
void bindTrackForInsert(SqliteStatement& st, const kanariv::types::Track& t);
void bindTrackForUpdate(SqliteStatement& st, const kanariv::types::Track& t);

void SqliteCloser::operator()(sqlite3* db) const noexcept {
    if (db)
        sqlite3_close(db);
}

Database::Database() : impl_(std::make_unique<Impl>()) {}

Database::Database(const DatabaseOptions& opts)
    : impl_(std::make_unique<Impl>(opts.writeBatchSize)) {
    impl_->logger_ = opts.logger;
}

// Impl's declaration order stops the writer before the handle closes and
// finalizes cached statements before the connection goes away.
Database::~Database() = default;

Database::Database(Database&& o) noexcept : impl_(std::move(o.impl_)) {}

Database& Database::operator=(Database&& o) noexcept {
    if (this != &o)
        impl_ = std::move(o.impl_);
    return *this;
}

kanariv::utils::Logger* Database::logger() const noexcept {
    return impl_ ? impl_->logger_ : nullptr;
}

std::expected<SqliteStatement*, kanariv::utils::Error>
Database::Impl::getCachedForUse(std::string_view sql) const {
    // Caller must hold cacheMutex_; see method docs.
    std::string key(sql);
    auto it = stmtCache_.find(key);
    if (it != stmtCache_.end()) {
        it->second->reset();
        return it->second.get();
    }
    auto up = std::make_unique<SqliteStatement>();
    if (auto e = up->prepare(db_.get(), sql); !e)
        return std::unexpected(e.error());
    SqliteStatement* raw = up.get();
    stmtCache_.emplace(std::move(key), std::move(up));
    return raw;
}

std::expected<std::unique_ptr<Database>, kanariv::utils::Error>
Database::open(std::string_view path, const DatabaseOptions& opts) {
    std::string dbPath = path.empty() ? ":memory:" : std::string(path);
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(dbPath.c_str(), &raw,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw)
            sqlite3_close(raw);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, msg)};
    }
    char* err = nullptr;
    internal::SqliteErrGuard errGuard{err};
    rc = sqlite3_exec(raw, std::string(schema).c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? std::string(err) : sqlite3_errmsg(raw);
        // Gracefully handle existing duplicate queue positions on migration:
        // UNIQUE(queue_id, position) creation may fail if old DB has duplicates.
        // Treat as non-fatal -- open still succeeds; queue ops will normalize positions.
        bool isQueueUniqueMigration = msg.find("idx_queue_queue_pos") != std::string::npos ||
                                      msg.find("queue") != std::string::npos;
        bool isUniqueConstraint =
            msg.find("UNIQUE") != std::string::npos || msg.find("unique") != std::string::npos;
        if (!(isQueueUniqueMigration && isUniqueConstraint)) {
            sqlite3_close(raw);
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Corrupt, std::string("schema init failed: ") + msg)};
        }
        // else: non-fatal migration duplicate -- clear err for next exec
        if (err) {
            sqlite3_free(err);
            err = nullptr;
        }
    }
    rc = sqlite3_exec(raw, std::string(schemaDefaultLibrary).c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? std::string(err) : sqlite3_errmsg(raw);
        if (opts.logger)
            opts.logger->warn("schema default-library pass failed (non-fatal): {}", msg);
        if (err) {
            sqlite3_free(err);
            err = nullptr;
        }
    }
    // Migration: add active_queue_id column if missing (for existing DBs)
    // Migration: add pre_mute_volume column if missing (for existing DBs)
    // Migration: add position_ms column if missing (for existing DBs)
    // Migration: add queues.temp column if missing (for existing DBs)
    for (const char* mig :
         {"ALTER TABLE engine_state ADD COLUMN active_queue_id INTEGER DEFAULT 1",
          "ALTER TABLE engine_state ADD COLUMN pre_mute_volume REAL DEFAULT 0.5",
          "ALTER TABLE engine_state ADD COLUMN position_ms INTEGER DEFAULT 0",
          "ALTER TABLE queues ADD COLUMN temp INTEGER DEFAULT 0",
          "ALTER TABLE tracks ADD COLUMN source_kind INTEGER NOT NULL DEFAULT 0",
          "ALTER TABLE tracks ADD COLUMN source_key TEXT NOT NULL DEFAULT ''",
          "ALTER TABLE tracks ADD COLUMN source_available BOOLEAN NOT NULL DEFAULT 1",
          "ALTER TABLE tracks ADD COLUMN source_display_name TEXT"}) {
        char* migErr = nullptr;
        int migRc = sqlite3_exec(raw, mig, nullptr, nullptr, &migErr);
        if (migErr) {
            sqlite3_free(migErr);
            migErr = nullptr;
        }
        (void)migRc;
    }
    // Existing rows were path-only. Give them an explicit source identity so
    // (source_kind, source_key) deduplication works without rewriting paths.
    // Non-fatal when the columns already hold normalized values.
    {
        char* migErr = nullptr;
        int migRc = sqlite3_exec(raw,
                                 "UPDATE tracks SET source_key=path, source_display_name=path "
                                 "WHERE source_kind=0 AND source_key=''",
                                 nullptr, nullptr, &migErr);
        if (migErr) {
            sqlite3_free(migErr);
            migErr = nullptr;
        }
        (void)migRc;
    }
    // The source index is created only after the migration columns exist on
    // legacy databases; CREATE TABLE above covers fresh databases.
    {
        char* migErr = nullptr;
        int migRc = sqlite3_exec(raw,
                                 "CREATE INDEX IF NOT EXISTS idx_tracks_source ON "
                                 "tracks(source_kind, source_key)",
                                 nullptr, nullptr, &migErr);
        if (migErr) {
            sqlite3_free(migErr);
            migErr = nullptr;
        }
        (void)migRc;
    }
    auto db = std::make_unique<Database>(opts);
    db->impl_->db_.reset(raw);
    db->impl_->writer_.open(raw);
    if (db->impl_->logger_)
        db->impl_->logger_->info("database opened path={}", dbPath);
    return db;
}

sqlite3* Database::Impl::handle() const noexcept {
    return db_.get();
}

std::shared_mutex& Database::Impl::mutex() const noexcept {
    return dbMutex_;
}

sqlite3* Database::Impl::handleLocked() const noexcept {
    return db_.get();
}

std::expected<void, kanariv::utils::Error> Database::flush() {
    return impl_->writer_.flush();
}

std::expected<void, kanariv::utils::Error>
Database::queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(impl_->db_.get(), "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Busy,
                                                        err ? std::string(err) : "BEGIN failed")};
    for (int64_t tid : tids) {
        auto r = kanariv::db::queueEnqueueLocked(impl_->db_.get(), qid, tid, -1);
        if (!r) {
            sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            return std::unexpected{r.error()};
        }
    }
    char* cErr = nullptr;
    internal::SqliteErrGuard cGuard{cErr};
    rc = sqlite3_exec(impl_->db_.get(), "COMMIT", nullptr, nullptr, &cErr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, cErr ? std::string(cErr) : "commit failed")};
    }
    return {};
}

std::expected<void, kanariv::utils::Error>
Database::queueEnqueueBatch(int64_t qid, const std::vector<int64_t>& tids) {
    return queueEnqueueBatch(qid, std::span<const int64_t>(tids.data(), tids.size()));
}

std::expected<void, kanariv::utils::Error> Database::queueReplaceAll(int64_t qid,
                                                                    std::span<const int64_t> ids) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(impl_->db_.get(), "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Busy,
                                                        err ? std::string(err) : "BEGIN failed")};
    auto clr = kanariv::db::queueClearLocked(impl_->db_.get(), qid);
    if (!clr) {
        sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected{clr.error()};
    }
    for (int64_t id : ids) {
        auto r = kanariv::db::queueEnqueueLocked(impl_->db_.get(), qid, id, -1);
        if (!r) {
            sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            return std::unexpected{r.error()};
        }
    }
    char* cErr = nullptr;
    internal::SqliteErrGuard cGuard{cErr};
    rc = sqlite3_exec(impl_->db_.get(), "COMMIT", nullptr, nullptr, &cErr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, cErr ? std::string(cErr) : "commit failed")};
    }
    return {};
}

std::expected<void, kanariv::utils::Error>
Database::queueReplaceAll(int64_t qid, const std::vector<int64_t>& ids) {
    return queueReplaceAll(qid, std::span<const int64_t>(ids.data(), ids.size()));
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::Impl::findByPathLocked(std::string_view path) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    std::unique_lock cacheLk{cacheMutex_};
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
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
    std::unique_lock cacheLk{cacheMutex_};
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, static_cast<int>(source.kind));
    st.bindText(2, source.key);
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::Impl::findByFingerprintLocked(const std::array<uint8_t, 32>& fp) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    std::unique_lock cacheLk{cacheMutex_};
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<int64_t, kanariv::utils::Error>
Database::Impl::insertTrackLocked(const kanariv::types::Track& t) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::unique_lock cacheLk{cacheMutex_};
    auto sRes = getCachedForUse(insertTrackSql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    bindTrackForInsert(st, t);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE) {
        int ec = sqlite3_extended_errcode(db_.get());
        if (ec == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT)
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::AlreadyExists, sqlite3_errmsg(db_.get()))};
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(db_.get()))};
    }
    return sqlite3_last_insert_rowid(db_.get());
}

std::expected<void, kanariv::utils::Error>
Database::Impl::updateTrackLocked(const kanariv::types::Track& t) {
    if (t.id == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "id 0")};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::unique_lock cacheLk{cacheMutex_};
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
    std::unique_lock cacheLk{cacheMutex_};
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

std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error>
Database::Impl::libraryListLocked() {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(db_.get(), "SELECT id, path, name, date_added, last_scanned, auto_scan, "
                                  "recursive, extensions FROM libraries ORDER BY id");
        !e)
        return std::unexpected{e.error()};
    std::vector<kanariv::types::Library> out;
    while (st.step()) {
        kanariv::types::Library l;
        l.id = st.columnInt(0);
        l.path = st.columnText(1);
        l.name = st.columnText(2);
        l.date_added = st.columnInt(3);
        l.last_scanned = st.columnInt(4);
        l.auto_scan = st.columnInt(5) != 0;
        l.recursive = st.columnInt(6) != 0;
        l.extensions = st.columnText(7);
        out.push_back(std::move(l));
    }
    return out;
}

std::expected<void, kanariv::utils::Error>
Database::Impl::libraryUpdateLocked(const kanariv::types::Library& l) {
    if (l.id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(db_.get(), "UPDATE libraries SET path=?, name=?, last_scanned=?, "
                                       "auto_scan=?, recursive=?, extensions=? WHERE id=?");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, l.path);
    if (l.name.empty())
        st.bindNull(2);
    else
        st.bindText(2, l.name);
    st.bindInt(3, l.last_scanned);
    st.bindInt(4, l.auto_scan ? 1 : 0);
    st.bindInt(5, l.recursive ? 1 : 0);
    st.bindText(6, l.extensions);
    st.bindInt(7, l.id);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(db_.get()))};
    if (sqlite3_changes(db_.get()) == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return {};
}

void bindTrackForInsert(SqliteStatement& st, const kanariv::types::Track& t) {
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

void bindTrackForUpdate(SqliteStatement& st, const kanariv::types::Track& t) {
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
    if (t.deleted_at)
        st.bindInt(29, t.deleted_at);
    else
        st.bindNull(29);
    st.bindInt(30, t.id);
}

std::expected<int64_t, kanariv::utils::Error> Database::insertTrack(const kanariv::types::Track& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->insertTrackLocked(t);
}

std::expected<void, kanariv::utils::Error> Database::updateTrack(const kanariv::types::Track& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->updateTrackLocked(t);
}

std::expected<void, kanariv::utils::Error> Database::deleteTrack(int64_t id) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->deleteTrackLocked(id);
}

std::expected<kanariv::types::Track, kanariv::utils::Error> Database::getTrack(int64_t id) {
    std::shared_lock lk{impl_->dbMutex_};
    return impl_->getTrackLocked(id);
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::Impl::getTrackLocked(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE id=?";
    std::unique_lock cacheLk{cacheMutex_};
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, id);
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::findByFingerprint(const std::array<uint8_t, 32>& fp) {
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    std::unique_lock cacheLk{impl_->cacheMutex_};
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::findByPath(std::string_view path) {
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    std::unique_lock cacheLk{impl_->cacheMutex_};
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::Track t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
Database::findBySource(const kanariv::types::SourceRef& source) {
    std::shared_lock lk{impl_->dbMutex_};
    return impl_->findBySourceLocked(source);
}

std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
Database::listTracks(const kanariv::types::TrackQuery* q) {
    std::shared_lock lk{impl_->dbMutex_};
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
        static const std::unordered_map<std::string, std::string> kSortCols{
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
        auto it = kSortCols.find(q->sort);
        if (it != kSortCols.end()) {
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
    std::vector<kanariv::types::Track> out;
    while (st.step()) {
        kanariv::types::Track t;
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
    std::unique_lock cacheLk{impl_->cacheMutex_};
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
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "rating must be 0..5")};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "UPDATE tracks SET rating=? WHERE id=?";
    std::unique_lock cacheLk{impl_->cacheMutex_};
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

std::expected<int64_t, kanariv::utils::Error>
Database::createPlaylist(std::string_view name, int type, std::string_view smart_query) {
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

std::expected<kanariv::types::Playlist, kanariv::utils::Error> Database::getPlaylist(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::shared_lock lk{impl_->dbMutex_};
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

std::expected<void, kanariv::utils::Error>
Database::updatePlaylist(const kanariv::types::Playlist& p) {
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

std::expected<void, kanariv::utils::Error> Database::deletePlaylist(int64_t id) {
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

std::expected<void, kanariv::utils::Error> Database::renamePlaylist(int64_t id,
                                                                   std::string_view newName) {
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

std::expected<int64_t, kanariv::utils::Error>
Database::createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(impl_->db_.get(), "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Busy,
                                                        err ? std::string(err) : "BEGIN failed")};
    auto pidRes = impl_->createPlaylistLocked(name);
    if (!pidRes) {
        sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected{pidRes.error()};
    }
    int64_t pid = *pidRes;
    for (int64_t tid : trackIds) {
        auto r = impl_->playlistAddTrackLocked(pid, tid);
        if (!r) {
            sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            return std::unexpected{r.error()};
        }
    }
    char* cErr = nullptr;
    internal::SqliteErrGuard cGuard{cErr};
    rc = sqlite3_exec(impl_->db_.get(), "COMMIT", nullptr, nullptr, &cErr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(impl_->db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, cErr ? std::string(cErr) : "commit failed")};
    }
    return pid;
}

std::expected<int64_t, kanariv::utils::Error>
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

std::expected<void, kanariv::utils::Error>
Database::Impl::playlistAddTrackLocked(int64_t pid, int64_t tid, int64_t pos) {
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

std::expected<std::vector<kanariv::types::Playlist>, kanariv::utils::Error>
Database::listPlaylists() {
    std::shared_lock lk{impl_->dbMutex_};
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

std::expected<void, kanariv::utils::Error> Database::playlistAddTrack(int64_t pid, int64_t tid,
                                                                     int64_t pos) {
    if (pid == 0 || tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    if (pos < 0) {
        SqliteStatement ms;
        if (auto e = ms.prepare(
                impl_->db_.get(),
                "SELECT COALESCE(MAX(position), -1)+1 FROM playlist_items WHERE playlist_id=?");
            e) {
            ms.bindInt(1, pid);
            if (ms.step())
                pos = ms.columnInt(0);
        }
        if (pos < 0)
            pos = 0;
    } else {
        SqliteStatement ss;
        if (auto e =
                ss.prepare(impl_->db_.get(), "UPDATE playlist_items SET position=position+1 WHERE "
                                             "playlist_id=? AND position>=?");
            e) {
            ss.bindInt(1, pid);
            ss.bindInt(2, pos);
            (void)ss.stepDone();
        }
    }
    SqliteStatement st;
    if (auto e = st.prepare(
            impl_->db_.get(),
            "INSERT INTO playlist_items (playlist_id, track_id, position) VALUES (?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, pid);
    st.bindInt(2, tid);
    st.bindInt(3, pos);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

std::expected<void, kanariv::utils::Error> Database::playlistRemoveTrack(int64_t pid, int64_t tid) {
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

std::expected<void, kanariv::utils::Error> Database::playlistReorder(int64_t pid, int64_t from,
                                                                    int64_t to) {
    if (pid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (from == to)
        return {};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
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
    if (from < to) {
        SqliteStatement ss;
        if (auto e =
                ss.prepare(impl_->db_.get(), "UPDATE playlist_items SET position=position-1 WHERE "
                                             "playlist_id=? AND position>? AND position<=?");
            e) {
            ss.bindInt(1, pid);
            ss.bindInt(2, from);
            ss.bindInt(3, to);
            (void)ss.stepDone();
        }
    } else {
        SqliteStatement ss;
        if (auto e =
                ss.prepare(impl_->db_.get(), "UPDATE playlist_items SET position=position+1 WHERE "
                                             "playlist_id=? AND position>=? AND position<?");
            e) {
            ss.bindInt(1, pid);
            ss.bindInt(2, to);
            ss.bindInt(3, from);
            (void)ss.stepDone();
        }
    }
    SqliteStatement us;
    if (auto e =
            us.prepare(impl_->db_.get(),
                       "UPDATE playlist_items SET position=? WHERE playlist_id=? AND track_id=?");
        e) {
        us.bindInt(1, to);
        us.bindInt(2, pid);
        us.bindInt(3, track_id);
        (void)us.stepDone();
    }
    return {};
}

std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
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
        return listTracks(&*q);
    }
    std::shared_lock lk{impl_->dbMutex_};
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
    std::vector<kanariv::types::Track> out;
    while (st.step()) {
        kanariv::types::Track t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

std::expected<void, kanariv::utils::Error> Database::queueEnqueue(int64_t qid, int64_t tid,
                                                                 int64_t pos) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueEnqueueLocked(qid, tid, pos);
}

std::expected<void, kanariv::utils::Error>
Database::Impl::queueEnqueueLocked(int64_t qid, int64_t tid, int64_t pos) {
    return kanariv::db::queueEnqueueLocked(db_.get(), qid, tid, pos);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error> Database::queueDequeue(int64_t qid) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueDequeueLocked(qid);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
Database::Impl::queueDequeueLocked(int64_t qid) {
    return kanariv::db::queueDequeueLocked(db_.get(), qid);
}

std::expected<void, kanariv::utils::Error> Database::queueRemove(int64_t qid, int64_t pos) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueRemoveLocked(impl_->db_.get(), qid, pos);
}

std::expected<void, kanariv::utils::Error> Database::queueClear(int64_t qid) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueClearLocked(impl_->db_.get(), qid);
}

std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
Database::listQueueItems(int64_t qid) {
    if (qid == 0)
        qid = 1;
    std::shared_lock lk{impl_->dbMutex_};
    return kanariv::db::queueListLocked(impl_->db_.get(), qid);
}

std::expected<kanariv::types::Queue, kanariv::utils::Error> Database::getQueue(int64_t qid) {
    if (qid == 0)
        qid = 1;
    std::shared_lock lk{impl_->dbMutex_};
    return kanariv::db::getQueueLocked(impl_->db_.get(), qid);
}

std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> Database::listQueues() {
    std::shared_lock lk{impl_->dbMutex_};
    return kanariv::db::listQueuesLocked(impl_->db_.get());
}

std::expected<int64_t, kanariv::utils::Error> Database::createQueue(std::string_view name,
                                                                   int64_t library_id) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::createQueueLocked(impl_->db_.get(), name, library_id);
}

std::expected<void, kanariv::utils::Error> Database::deleteQueue(int64_t qid) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::deleteQueueLocked(impl_->db_.get(), qid);
}

std::expected<void, kanariv::utils::Error> Database::setQueueRepeat(int64_t qid, int repeat_mode) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueRepeatLocked(impl_->db_.get(), qid, repeat_mode);
}

std::expected<void, kanariv::utils::Error> Database::setQueueTemp(int64_t qid, bool temp) {
    if (qid == 0)
        qid = 1;
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueTempLocked(impl_->db_.get(), qid, temp);
}

std::size_t Database::queueCount(int64_t qid) {
    if (qid == 0)
        qid = 1;
    std::shared_lock lk{impl_->dbMutex_};
    return impl_->queueCountLocked(qid);
}

std::size_t Database::Impl::queueCountLocked(int64_t qid) {
    return kanariv::db::queueCountLocked(db_.get(), qid);
}

std::optional<size_t> Database::queueIndexOf(int64_t qid, int64_t trackId) {
    if (qid == 0)
        qid = 1;
    if (trackId == 0)
        return std::nullopt;
    std::shared_lock lk{impl_->dbMutex_};
    return kanariv::db::queueIndexOfLocked(impl_->db_.get(), qid, trackId);
}

std::expected<void, kanariv::utils::Error>
Database::historyAdd(const kanariv::types::HistoryEntry& e) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "INSERT INTO history (track_id, started_at, completed_at, "
                           "position_ms, completion_pct, queue_id) VALUES (?,?,?,?,?,?)";
    std::unique_lock cacheLk{impl_->cacheMutex_};
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, e.track_id);
    st.bindInt(2, e.started_at);
    if (e.completed_at)
        st.bindInt(3, e.completed_at);
    else
        st.bindNull(3);
    st.bindInt(4, e.position_ms);
    st.bindDouble(5, e.completion_pct);
    st.bindInt(6, e.queue_id ? e.queue_id : 1);
    int rc = st.stepDone();
    st.reset();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Database::listHistory(const kanariv::types::HistoryQuery* q) {
    std::shared_lock lk{impl_->dbMutex_};
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

std::expected<void, kanariv::utils::Error> Database::historyClear() {
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
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    if (err)
        sqlite3_free(err);
    return {};
}

std::expected<void, kanariv::utils::Error> Database::bookmarkAdd(int64_t tid, int64_t pos,
                                                                std::string_view note) {
    if (tid == 0 || pos < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "INSERT INTO bookmarks (track_id, position_ms, note) VALUES (?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    st.bindInt(2, pos);
    if (note.empty())
        st.bindNull(3);
    else
        st.bindText(3, note);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

std::expected<std::vector<kanariv::types::Bookmark>, kanariv::utils::Error>
Database::listBookmarks(int64_t tid) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(), "SELECT id, track_id, position_ms, note, "
                                         "CAST(strftime('%s', created) AS INTEGER) FROM bookmarks "
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

std::expected<kanariv::types::Bookmark, kanariv::utils::Error> Database::bookmarkGet(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(),
                            "SELECT id, track_id, position_ms, note, "
                            "CAST(strftime('%s', created) AS INTEGER) FROM bookmarks WHERE id=?");
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

std::expected<void, kanariv::utils::Error> Database::bookmarkRemove(int64_t id) {
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

std::expected<int64_t, kanariv::utils::Error> Database::libraryAdd(std::string_view path,
                                                                  std::string_view name) {
    if (path.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "INSERT INTO libraries (path, name) VALUES (?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindText(1, path);
    if (name.empty())
        st.bindNull(2);
    else
        st.bindText(2, name);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        sqlite3_errmsg(impl_->db_.get()))};
    return sqlite3_last_insert_rowid(impl_->db_.get());
}

std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error> Database::listLibraries() {
    std::shared_lock lk{impl_->dbMutex_};
    return impl_->libraryListLocked();
}

std::expected<void, kanariv::utils::Error> Database::libraryUpdate(const kanariv::types::Library& l) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->libraryUpdateLocked(l);
}

std::expected<void, kanariv::utils::Error> Database::libraryDelete(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e = st.prepare(impl_->db_.get(), "DELETE FROM libraries WHERE id=?"); !e)
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

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Database::getStats() {
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    kanariv::types::PlaybackStats s;
    std::unique_lock cacheLk{impl_->cacheMutex_};
    auto one = [&](std::string_view sql, int64_t& out) {
        auto sRes = impl_->getCachedForUse(sql);
        if (!sRes)
            return;
        SqliteStatement& st = *(*sRes);
        if (st.step())
            out = st.columnInt(0);
        st.reset();
    };
    one("SELECT COUNT(*) FROM tracks WHERE deleted_at IS NULL", s.num_tracks);
    one("SELECT COUNT(*) FROM playlists", s.num_playlists);
    one("SELECT COUNT(*) FROM queue", s.num_queue_items);
    one("SELECT COUNT(*) FROM history", s.num_history);
    one("SELECT COUNT(*) FROM bookmarks", s.num_bookmarks);
    one("SELECT COUNT(*) FROM libraries", s.num_libraries);
    if (auto sRes = impl_->getCachedForUse(
            "SELECT COALESCE(SUM(duration),0) FROM tracks WHERE deleted_at IS NULL");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            s.total_duration_ms = static_cast<int64_t>(st.columnDouble(0) * 1000);
        st.reset();
    }
    return s;
}

std::expected<kanariv::types::LibraryStats, kanariv::utils::Error>
Database::libraryStats(std::optional<int> topTracks,
                       std::optional<int> topAlbums,
                       std::optional<int> topGenres,
                       std::optional<int> topArtists) {
    std::shared_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    kanariv::types::LibraryStats d{};
    std::unique_lock cacheLk{impl_->cacheMutex_};

    // Total tracks, queues, playlists
    auto one = [&](std::string_view sql, auto& out) {
        auto sRes = impl_->getCachedForUse(sql);
        if (!sRes)
            return;
        SqliteStatement& st = *(*sRes);
        if (st.step())
            out = static_cast<std::size_t>(st.columnInt(0));
        st.reset();
    };

    one("SELECT COUNT(*) FROM tracks WHERE deleted_at IS NULL", d.tracks);
    one("SELECT COUNT(*) FROM queue", d.queues);
    one("SELECT COUNT(*) FROM playlists", d.playlists);

    // Total duration
    if (auto sRes = impl_->getCachedForUse(
            "SELECT COALESCE(SUM(duration),0) FROM tracks WHERE deleted_at IS NULL");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_duration_ms = static_cast<int64_t>(st.columnDouble(0) * 1000);
        st.reset();
    }

    // Total play time (sum of play_count * duration)
    if (auto sRes = impl_->getCachedForUse(
            "SELECT COALESCE(SUM(play_count * duration),0) FROM tracks WHERE deleted_at IS NULL");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_play_time_ms = static_cast<int64_t>(st.columnDouble(0) * 1000);
        st.reset();
    }

    // ------------------------------------------------------------------
    // Top-N listening reports
    // ------------------------------------------------------------------

    // Top tracks by play count (play_count DESC).
    if (topTracks) {
        d.top_tracks.emplace();
        if (*topTracks > 0) {
            std::string sql =
            std::string(internal::selectTracksCols) +
            " WHERE deleted_at IS NULL AND play_count > 0 ORDER BY play_count DESC LIMIT " +
            std::to_string(*topTracks);
        auto sRes = impl_->getCachedForUse(sql);
        if (sRes) {
            SqliteStatement& st = *(*sRes);
            while (st.step()) {
                kanariv::types::Track t;
                internal::fillTrackFromStmt(st.get(), t);
                d.top_tracks->push_back(std::move(t));
            }
            st.reset();
        }
    }
    }

    // Top albums by total plays (GROUP BY album).
    if (topAlbums && *topAlbums > 0) {
        d.top_albums.emplace();
        std::string sql = std::format(
            "SELECT album, COUNT(*), COALESCE(SUM(play_count),0) "
            "FROM tracks WHERE deleted_at IS NULL AND album IS NOT NULL AND album != '' "
            "GROUP BY album "
            "ORDER BY SUM(play_count) DESC, album ASC LIMIT {}",
            *topAlbums);
        auto sRes = impl_->getCachedForUse(sql);
        if (sRes) {
            SqliteStatement& st = *(*sRes);
            while (st.step()) {
                kanariv::types::TopEntity e;
                e.name = st.columnText(0);
                e.track_count = static_cast<std::size_t>(st.columnInt(1));
                e.total_plays = st.columnInt(2);
                d.top_albums->push_back(std::move(e));
            }
            st.reset();
        }
    }

    // Top genres by total plays (GROUP BY genre).
    if (topGenres && *topGenres > 0) {
        d.top_genres.emplace();
        std::string sql = std::format(
            "SELECT genre, COUNT(*), COALESCE(SUM(play_count),0) "
            "FROM tracks WHERE deleted_at IS NULL AND genre IS NOT NULL AND genre != '' "
            "GROUP BY genre "
            "ORDER BY SUM(play_count) DESC, genre ASC LIMIT {}",
            *topGenres);
        auto sRes = impl_->getCachedForUse(sql);
        if (sRes) {
            SqliteStatement& st = *(*sRes);
            while (st.step()) {
                kanariv::types::TopEntity e;
                e.name = st.columnText(0);
                e.track_count = static_cast<std::size_t>(st.columnInt(1));
                e.total_plays = st.columnInt(2);
                d.top_genres->push_back(std::move(e));
            }
            st.reset();
        }
    }

    // Top artists by total plays.
    // Multi-artist tracks are split so each individual artist is counted
    // separately with the full play_count of the track.
    if (topArtists && *topArtists > 0) {
        d.top_artists.emplace();
        // Fetch all (artist, play_count) pairs for live tracks with play_count > 0.
        std::string sql =
            "SELECT artist, play_count FROM tracks WHERE deleted_at IS NULL "
            "AND artist IS NOT NULL AND artist != '' AND play_count > 0";
        auto sRes = impl_->getCachedForUse(sql);
        if (sRes) {
            SqliteStatement& st = *(*sRes);
            // Accumulate per-artist play counts.
            std::map<std::string, std::pair<std::size_t, std::int64_t>> artistStats;
            while (st.step()) {
                std::string artistStr = st.columnText(0);
                if (artistStr.empty())
                    continue;
                std::int64_t playCount = st.columnInt(1);

                // Split multi-artist strings. Delimiters (safest first):
                //   ";"        semicolon (most common tag separator)
                //   ", "       comma+space (avoids breaking names like "The Beatles, The")
                //   " feat. ", " ft. ", " featuring ", " & "   surrounded by spaces
                std::size_t start = 0;
                while (start < artistStr.size()) {
                    // Find the next delimiter position.
                    std::size_t next = artistStr.size();
                    auto tryFind = [&](std::string_view delim) {
                        std::size_t p = artistStr.find(delim, start);
                        if (p != std::string::npos && p < next)
                            next = p;
                    };
                    tryFind(";");
                    tryFind(", ");
                    tryFind(" feat. ");
                    tryFind(" ft. ");
                    tryFind(" featuring ");
                    tryFind(" & ");

                    std::string part = artistStr.substr(start, next - start);
                    // Trim whitespace.
                    while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
                        part.erase(0, 1);
                    while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
                        part.pop_back();
                    if (!part.empty()) {
                        auto& acc = artistStats[part];
                        acc.first += 1;
                        acc.second += playCount;
                    }
                    if (next == artistStr.size())
                        break;
                    start = next + 1;
                }
            }
            st.reset();

            // Sort by total_plays DESC, then name ASC.
            std::vector<std::pair<std::string, std::pair<std::size_t, std::int64_t>>>
                sorted(artistStats.begin(), artistStats.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second.second != b.second.second)
                              return a.second.second > b.second.second;
                          return a.first < b.first;
                      });

            std::size_t limit = static_cast<std::size_t>(*topArtists);
            for (std::size_t i = 0; i < sorted.size() && i < limit; ++i) {
                kanariv::types::TopEntity e;
                e.name = sorted[i].first;
                e.track_count = sorted[i].second.first;
                e.total_plays = sorted[i].second.second;
                d.top_artists->push_back(std::move(e));
            }
        }
    }

    return d;
}

// DatabaseAccess forwarders -- defined here where Impl is complete.
sqlite3* DatabaseAccess::handle(Database& db) noexcept {
    return db.impl_->handle();
}

sqlite3* DatabaseAccess::handleLocked(Database& db) noexcept {
    return db.impl_->handleLocked();
}

std::shared_mutex& DatabaseAccess::mutex(Database& db) noexcept {
    return db.impl_->mutex();
}

std::expected<SqliteStatement*, kanariv::utils::Error>
DatabaseAccess::getCachedForUse(Database& db, std::string_view sql) {
    return db.impl_->getCachedForUse(sql);
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
DatabaseAccess::findByPathLocked(Database& db, std::string_view path) {
    return db.impl_->findByPathLocked(path);
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
DatabaseAccess::findByFingerprintLocked(Database& db, const std::array<uint8_t, 32>& fp) {
    return db.impl_->findByFingerprintLocked(fp);
}

std::expected<kanariv::types::Track, kanariv::utils::Error>
DatabaseAccess::getTrackLocked(Database& db, int64_t id) {
    return db.impl_->getTrackLocked(id);
}

std::expected<int64_t, kanariv::utils::Error>
DatabaseAccess::insertTrackLocked(Database& db, const kanariv::types::Track& t) {
    return db.impl_->insertTrackLocked(t);
}

std::expected<void, kanariv::utils::Error>
DatabaseAccess::updateTrackLocked(Database& db, const kanariv::types::Track& t) {
    return db.impl_->updateTrackLocked(t);
}

std::expected<void, kanariv::utils::Error> DatabaseAccess::deleteTrackLocked(Database& db,
                                                                            int64_t id) {
    return db.impl_->deleteTrackLocked(id);
}

std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error>
DatabaseAccess::libraryListLocked(Database& db) {
    return db.impl_->libraryListLocked();
}

std::expected<void, kanariv::utils::Error>
DatabaseAccess::libraryUpdateLocked(Database& db, const kanariv::types::Library& l) {
    return db.impl_->libraryUpdateLocked(l);
}

std::expected<int64_t, kanariv::utils::Error>
DatabaseAccess::createPlaylistLocked(Database& db, std::string_view name, int type,
                                     std::string_view smart_query) {
    return db.impl_->createPlaylistLocked(name, type, smart_query);
}

std::expected<void, kanariv::utils::Error>
DatabaseAccess::playlistAddTrackLocked(Database& db, int64_t pid, int64_t tid, int64_t pos) {
    return db.impl_->playlistAddTrackLocked(pid, tid, pos);
}

std::expected<void, kanariv::utils::Error>
DatabaseAccess::queueEnqueueLocked(Database& db, int64_t qid, int64_t tid, int64_t pos) {
    return db.impl_->queueEnqueueLocked(qid, tid, pos);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
DatabaseAccess::queueDequeueLocked(Database& db, int64_t qid) {
    return db.impl_->queueDequeueLocked(qid);
}

} // namespace kanariv::db
