#include <sqlite3.h>

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
#include <format>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json_state_store.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
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

#include "db_impl.hpp"

namespace kanariv::db {

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
        return std::unexpected{kanariv::db::internal::sqliteError(impl_->db_.get(), rc)};
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
    if (auto sRes = impl_->getCachedForUse("SELECT COALESCE(SUM(duration),0) FROM tracks"); sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            s.total_duration_ms = static_cast<int64_t>((st.columnDouble(0) * 1000));
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
    if (auto sRes = impl_->getCachedForUse("SELECT COALESCE(SUM(duration),0) FROM tracks"); sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_duration_ms = static_cast<int64_t>((st.columnDouble(0) * 1000));
        st.reset();
    }

    // Total play time (sum of play_count * duration)
    if (auto sRes =
            impl_->getCachedForUse("SELECT COALESCE(SUM(play_count * duration),0) FROM tracks");
        sRes) {
        SqliteStatement& st = *(*sRes);
        if (st.step())
            d.total_play_time_ms = static_cast<int64_t>((st.columnDouble(0) * 1000));
        st.reset();
    }

    // ------------------------------------------------------------------
    // Top-N listening reports
    // ------------------------------------------------------------------

    // Top tracks by play count (play_count DESC).
    if (topTracks) {
        d.top_tracks.emplace();
        if (*topTracks > 0) {
            std::string sql = std::string(internal::selectTracksCols) +
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
