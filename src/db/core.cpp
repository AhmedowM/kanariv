#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <db/database_access.hpp>
#include <db/fts.hpp>
#include <db/migration.hpp>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json_state_store.hpp>
#include <kanariv/db/smart.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    "source_available=?, source_display_name=? WHERE id=?";

// Private implementation: the SQLite handle, statement cache and locks live
// here so the public header stays include-light. Member order is load-bearing:
// cached statements are finalized before the handle is closed.
struct Database::Impl {
    std::unique_ptr<sqlite3, SqliteCloser> db_;
    /// Filesystem path the database was opened with (empty for :memory:/URI).
    std::filesystem::path path_;
    /// Non-owning log sink from DatabaseOptions (null = silent). See DatabaseOptions::logger.
    kanariv::utils::Logger* logger_{nullptr};
    // One mutex serialises the handle, the statement cache and every statement
    // taken from it: SQLite statements and the cache are not safe to touch
    // concurrently, so a shared/unique split only bought false parallelism.
    mutable std::mutex dbMutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<SqliteStatement>> stmtCache_;

    // Raw locking surface
    std::expected<SqliteStatement*, kanariv::utils::Error>
    getCachedForUse(std::string_view sql) const;
    sqlite3* handle() const noexcept;
    sqlite3* handleLocked() const noexcept;
    std::mutex& mutex() const noexcept;

    // TrackRow / library locked helpers
    std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findByPathLocked(std::string_view path);
    std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findBySourceLocked(const kanariv::types::SourceRef& source);
    std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findByFingerprintLocked(const std::array<uint8_t, 32>& fp);
    std::expected<int64_t, kanariv::utils::Error>
    insertTrackLocked(const kanariv::types::TrackRow& t);
    std::expected<void, kanariv::utils::Error> updateTrackLocked(const kanariv::types::TrackRow& t);
    std::expected<void, kanariv::utils::Error> deleteTrackLocked(int64_t id);
    std::expected<kanariv::types::TrackRow, kanariv::utils::Error> getTrackLocked(int64_t id);
    std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error> libraryListLocked();
    std::expected<void, kanariv::utils::Error>
    libraryUpdateLocked(const kanariv::types::Library& l);

    // Playlist / queue locked helpers
    std::expected<int64_t, kanariv::utils::Error>
    createPlaylistLocked(std::string_view name, int type = 0, std::string_view smart_query = {});
    std::expected<void, kanariv::utils::Error> playlistAddTrackLocked(int64_t pid, int64_t tid,
                                                                      int64_t pos = -1);
    std::expected<void, kanariv::utils::Error> queueEnqueueLocked(int64_t qid, int64_t tid,
                                                                  int64_t index = appendIndex);
    std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeueLocked(int64_t qid);
    std::size_t queueCountLocked(int64_t qid);
};

// Free functions for track binding (static, no impl_ needed)
void bindTrackForInsert(SqliteStatement& st, const kanariv::types::TrackRow& t);
void bindTrackForUpdate(SqliteStatement& st, const kanariv::types::TrackRow& t);

void SqliteCloser::operator()(sqlite3* db) const noexcept {
    if (db)
        sqlite3_close(db);
}

Database::Database() : impl_(std::make_unique<Impl>()) {}

Database::Database(const DatabaseOptions& opts) : impl_(std::make_unique<Impl>()) {
    impl_->logger_ = opts.logger;
}

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

const std::filesystem::path& Database::path() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->path_ : empty;
}

std::expected<SqliteStatement*, kanariv::utils::Error>
Database::Impl::getCachedForUse(std::string_view sql) const {
    // Caller must hold dbMutex_; see method docs.
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
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    // A configured path must name the file it looks like: only an explicit
    // opt-in may turn `file:...?mode=ro` into connection flags.
    if (opts.allowUri)
        flags |= SQLITE_OPEN_URI;
    int rc = sqlite3_open_v2(dbPath.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw)
            sqlite3_close(raw);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, msg)};
    }
    if (auto configured = internal::configureConnection(raw); !configured) {
        sqlite3_close(raw);
        return std::unexpected{
            kanariv::utils::makeError(configured.error().code, "database configuration failed: " +
                                                                   configured.error().message)};
    }
    // Versioned schema ladder: each step runs in its own transaction and a
    // failure is returned to the caller instead of being tolerated.
    if (auto migrated = internal::applyMigrations(raw, internal::schemaMigrations()); !migrated) {
        sqlite3_close(raw);
        return std::unexpected{kanariv::utils::makeError(
            migrated.error().code, "database migration failed: " + migrated.error().message)};
    }
    auto db = std::make_unique<Database>(opts);
    db->impl_->db_.reset(raw);
    // A URI is connection syntax, not a sibling-file location; in-memory
    // databases have no directory to keep the state sidecar in.
    if (dbPath != ":memory:" && !(opts.allowUri && dbPath.starts_with("file:")))
        db->impl_->path_ = std::filesystem::path(dbPath);
    if (db->impl_->logger_)
        db->impl_->logger_->info("database opened path={}", dbPath);
    return db;
}

sqlite3* Database::Impl::handle() const noexcept {
    return db_.get();
}

std::mutex& Database::Impl::mutex() const noexcept {
    return dbMutex_;
}

sqlite3* Database::Impl::handleLocked() const noexcept {
    return db_.get();
}

std::expected<void, kanariv::utils::Error>
Database::queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = kanariv::db::DbTransaction::begin(impl_->db_.get());
    if (!txRes)
        return std::unexpected{txRes.error()};
    auto tx = std::move(*txRes);
    for (int64_t tid : tids) {
        auto r = kanariv::db::queueEnqueueLocked(impl_->db_.get(), qid, tid, -1);
        if (!r)
            return std::unexpected{r.error()};
    }
    return tx.commit();
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::findByPathLocked(std::string_view path) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
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
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, static_cast<int>(source.kind));
    st.bindText(2, source.key);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::findByFingerprintLocked(const std::array<uint8_t, 32>& fp) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<int64_t, kanariv::utils::Error>
Database::Impl::insertTrackLocked(const kanariv::types::TrackRow& t) {
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
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
Database::Impl::updateTrackLocked(const kanariv::types::TrackRow& t) {
    if (t.id == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "id 0")};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
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

void bindTrackForInsert(SqliteStatement& st, const kanariv::types::TrackRow& t) {
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

void bindTrackForUpdate(SqliteStatement& st, const kanariv::types::TrackRow& t) {
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
    st.bindInt(29, t.id);
}

std::expected<int64_t, kanariv::utils::Error>
Database::insertTrack(const kanariv::types::TrackRow& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->insertTrackLocked(t);
}

std::expected<void, kanariv::utils::Error>
Database::updateTrack(const kanariv::types::TrackRow& t) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->updateTrackLocked(t);
}

std::expected<void, kanariv::utils::Error> Database::deleteTrack(int64_t id) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->deleteTrackLocked(id);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error> Database::getTrack(int64_t id) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->getTrackLocked(id);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::Impl::getTrackLocked(int64_t id) {
    if (id == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    if (!db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE id=?";
    auto sRes = getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindInt(1, id);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findByFingerprint(const std::array<uint8_t, 32>& fp) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE fingerprint=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindBlob(
        1, std::span<const std::byte>{reinterpret_cast<const std::byte*>(fp.data()), fp.size()});
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findByPath(std::string_view path) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string sql = std::string(internal::selectTracksCols) + " WHERE path=?";
    auto sRes = impl_->getCachedForUse(sql);
    if (!sRes)
        return std::unexpected{sRes.error()};
    SqliteStatement& st = *(*sRes);
    st.bindText(1, path);
    bool hasRow = st.step();
    kanariv::types::TrackRow t;
    if (hasRow)
        internal::fillTrackFromStmt(st.get(), t);
    st.reset();
    if (!hasRow)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound)};
    return t;
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
Database::findBySource(const kanariv::types::SourceRef& source) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->findBySourceLocked(source);
}

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
Database::listTracks(std::optional<kanariv::types::TrackQuery> q) {
    std::unique_lock lk{impl_->dbMutex_};
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
        static const std::unordered_map<std::string, std::string> sortCols{
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
        auto it = sortCols.find(q->sort);
        if (it != sortCols.end()) {
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
    std::vector<kanariv::types::TrackRow> out;
    while (st.step()) {
        kanariv::types::TrackRow t;
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
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "rating must be 0..5")};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    std::string_view sql = "UPDATE tracks SET rating=? WHERE id=?";
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
    p.type = (int)st.columnInt(2);
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
        p.type = (int)st.columnInt(2);
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

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
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

std::expected<void, kanariv::utils::Error> Database::queueEnqueue(int64_t qid, int64_t tid,
                                                                  int64_t index) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueEnqueueLocked(qid, tid, index);
}

std::expected<void, kanariv::utils::Error>
Database::Impl::queueEnqueueLocked(int64_t qid, int64_t tid, int64_t index) {
    return kanariv::db::queueEnqueueLocked(db_.get(), qid, tid, index);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
Database::queueDequeue(int64_t qid) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueDequeueLocked(qid);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
Database::Impl::queueDequeueLocked(int64_t qid) {
    return kanariv::db::queueDequeueLocked(db_.get(), qid);
}

std::expected<void, kanariv::utils::Error> Database::queueRemove(int64_t qid, int64_t index) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueRemoveLocked(impl_->db_.get(), qid, index);
}

std::expected<void, kanariv::utils::Error> Database::queueMove(int64_t qid, std::size_t from,
                                                               std::size_t to) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = kanariv::db::DbTransaction::begin(impl_->db_.get());
    if (!txRes)
        return std::unexpected{txRes.error()};
    auto tx = std::move(*txRes);
    auto moved = kanariv::db::queueMoveLocked(impl_->db_.get(), qid, from, to);
    if (!moved)
        return std::unexpected{moved.error()};
    return tx.commit();
}

std::expected<void, kanariv::utils::Error> Database::queueClear(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueClearLocked(impl_->db_.get(), qid);
}

std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
Database::listQueueItems(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueListLocked(impl_->db_.get(), qid);
}

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
Database::tracksForQueue(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueTracksLocked(impl_->db_.get(), qid);
}

std::expected<kanariv::types::Queue, kanariv::utils::Error> Database::getQueue(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::getQueueLocked(impl_->db_.get(), qid);
}

std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> Database::listQueues() {
    std::unique_lock lk{impl_->dbMutex_};
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
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::deleteQueueLocked(impl_->db_.get(), qid);
}

std::expected<void, kanariv::utils::Error>
Database::setQueueRepeat(int64_t qid, kanariv::types::RepeatMode mode) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueRepeatLocked(impl_->db_.get(), qid, static_cast<int>(mode));
}

std::expected<void, kanariv::utils::Error> Database::setQueueTemp(int64_t qid, bool temp) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueTempLocked(impl_->db_.get(), qid, temp);
}

std::expected<std::size_t, kanariv::utils::Error> Database::queueCount(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    return impl_->queueCountLocked(qid);
}

std::size_t Database::Impl::queueCountLocked(int64_t qid) {
    return kanariv::db::queueCountLocked(db_.get(), qid);
}

std::expected<std::optional<size_t>, kanariv::utils::Error>
Database::queueIndexOf(int64_t qid, int64_t trackId) {
    qid = kanariv::db::normalizeQueueId(qid);
    if (trackId == 0)
        return std::nullopt;
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
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

std::expected<void, kanariv::utils::Error> Database::clearHistory() {
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
    std::unique_lock lk{impl_->dbMutex_};
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
    std::unique_lock lk{impl_->dbMutex_};
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

std::expected<void, kanariv::utils::Error> Database::lyricsSet(int64_t tid,
                                                               std::string_view lrc_text,
                                                               bool is_synced,
                                                               std::string_view source) {
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
    if (auto e = st.prepare(impl_->db_.get(),
                            "INSERT INTO lyrics (track_id, lrc_text, is_synced, source) "
                            "VALUES (?,?,?,?)");
        !e)
        return std::unexpected{e.error()};
    st.bindInt(1, tid);
    st.bindText(2, lrc_text);
    st.bindInt(3, is_synced ? 1 : 0);
    if (source.empty())
        st.bindNull(4);
    else
        st.bindText(4, source);
    int rc = st.stepDone();
    if (rc != SQLITE_DONE)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         sqlite3_errmsg(impl_->db_.get()))};
    return {};
}

std::expected<kanariv::types::Lyrics, kanariv::utils::Error> Database::lyricsGet(int64_t tid) {
    if (tid == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    SqliteStatement st;
    if (auto e =
            st.prepare(impl_->db_.get(), "SELECT id, track_id, lrc_text, is_synced, source, "
                                         "CAST(strftime('%s', fetched_at) AS INTEGER) FROM lyrics "
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

std::expected<void, kanariv::utils::Error> Database::lyricsRemove(int64_t tid) {
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

std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error>
Database::listLibraries() {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->libraryListLocked();
}

std::expected<void, kanariv::utils::Error>
Database::libraryUpdate(const kanariv::types::Library& l) {
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
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    kanariv::types::PlaybackStats s;
    auto one = [&](std::string_view sql, int64_t& out) {
        auto sRes = impl_->getCachedForUse(sql);
        if (!sRes)
            return;
        SqliteStatement& st = *(*sRes);
        if (st.step())
            out = st.columnInt(0);
        st.reset();
    };
    one("SELECT COUNT(*) FROM tracks", s.num_tracks);
    one("SELECT COUNT(*) FROM playlists", s.num_playlists);
    one("SELECT COUNT(*) FROM queue", s.num_queue_items);
    one("SELECT COUNT(*) FROM history", s.num_history);
    one("SELECT COUNT(*) FROM bookmarks", s.num_bookmarks);
    one("SELECT COUNT(*) FROM libraries", s.num_libraries);
    if (auto sRes = impl_->getCachedForUse("SELECT COALESCE(SUM(duration),0) FROM tracks");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            s.total_duration_ms = (int64_t)(st.columnDouble(0) * 1000);
        st.reset();
    }
    return s;
}

std::expected<kanariv::types::LibraryStats, kanariv::utils::Error>
Database::libraryStats(std::optional<int> topTracks, std::optional<int> topAlbums,
                       std::optional<int> topGenres, std::optional<int> topArtists) {
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    kanariv::types::LibraryStats d{};

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

    one("SELECT COUNT(*) FROM tracks", d.tracks);
    one("SELECT COUNT(*) FROM queue", d.queues);
    one("SELECT COUNT(*) FROM playlists", d.playlists);

    // Total duration
    if (auto sRes = impl_->getCachedForUse("SELECT COALESCE(SUM(duration),0) FROM tracks");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_duration_ms = (int64_t)(st.columnDouble(0) * 1000);
        st.reset();
    }

    // Total play time (sum of play_count * duration)
    if (auto sRes = impl_->getCachedForUse(
            "SELECT COALESCE(SUM(play_count * duration),0) FROM tracks");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_play_time_ms = (int64_t)(st.columnDouble(0) * 1000);
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
                " WHERE play_count > 0 ORDER BY play_count DESC LIMIT " +
                std::to_string(*topTracks);
            auto sRes = impl_->getCachedForUse(sql);
            if (sRes) {
                SqliteStatement& st = *(*sRes);
                while (st.step()) {
                    kanariv::types::TrackRow t;
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
        std::string sql = std::format("SELECT album, COUNT(*), COALESCE(SUM(play_count),0) "
                                      "FROM tracks WHERE album IS NOT NULL AND album != '' "
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
        std::string sql = std::format("SELECT genre, COUNT(*), COALESCE(SUM(play_count),0) "
                                      "FROM tracks WHERE genre IS NOT NULL AND genre != '' "
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
        std::string sql = "SELECT artist, play_count FROM tracks "
                          "WHERE artist IS NOT NULL AND artist != '' AND play_count > 0";
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
            std::vector<std::pair<std::string, std::pair<std::size_t, std::int64_t>>> sorted(
                artistStats.begin(), artistStats.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
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

std::mutex& DatabaseAccess::mutex(Database& db) noexcept {
    return db.impl_->mutex();
}

std::expected<SqliteStatement*, kanariv::utils::Error>
DatabaseAccess::getCachedForUse(Database& db, std::string_view sql) {
    return db.impl_->getCachedForUse(sql);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
DatabaseAccess::findByPathLocked(Database& db, std::string_view path) {
    return db.impl_->findByPathLocked(path);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
DatabaseAccess::findByFingerprintLocked(Database& db, const std::array<uint8_t, 32>& fp) {
    return db.impl_->findByFingerprintLocked(fp);
}

std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
DatabaseAccess::getTrackLocked(Database& db, int64_t id) {
    return db.impl_->getTrackLocked(id);
}

std::expected<int64_t, kanariv::utils::Error>
DatabaseAccess::insertTrackLocked(Database& db, const kanariv::types::TrackRow& t) {
    return db.impl_->insertTrackLocked(t);
}

std::expected<void, kanariv::utils::Error>
DatabaseAccess::updateTrackLocked(Database& db, const kanariv::types::TrackRow& t) {
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
DatabaseAccess::queueEnqueueLocked(Database& db, int64_t qid, int64_t tid, int64_t index) {
    return db.impl_->queueEnqueueLocked(qid, tid, index);
}

std::expected<kanariv::types::QueueItem, kanariv::utils::Error>
DatabaseAccess::queueDequeueLocked(Database& db, int64_t qid) {
    return db.impl_->queueDequeueLocked(qid);
}

std::optional<kanariv::types::EngineState> DatabaseAccess::legacyEngineState(Database& db) {
    return db.legacyEngineState();
}

contracts::Storage makeStorage(std::shared_ptr<Database> db) {
    if (!db)
        return {};
    // One control block shared by every port: a consumer that keeps any port
    // alive keeps the database (and its connection) alive with it. State is the
    // exception -- it is a JSON sidecar beside the database file, not a table.
    auto state = makePlaybackStateStore(*db);
    return contracts::Storage{
        .tracks = std::static_pointer_cast<contracts::ITrackStore>(db),
        .playlists = std::static_pointer_cast<contracts::IPlaylistStore>(db),
        .queues = std::static_pointer_cast<contracts::IQueueStore>(db),
        .library = std::static_pointer_cast<contracts::ILibraryStore>(db),
        .history = std::static_pointer_cast<contracts::IHistoryStore>(db),
        .state = std::move(state),
        // Scanning combines discovery with persistence; only the composition
        // root that links the media stack can supply one (kanariv::library).
        .scanner = nullptr,
    };
}

} // namespace kanariv::db
