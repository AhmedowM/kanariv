#include <sqlite3.h>

#include <db/database_access.hpp>
#include <db/fts.hpp>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/db/search.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::db {

constexpr int bindSearchPat1 = 1;
constexpr int bindSearchPat2 = 2;
constexpr int bindSearchPat3 = 3;
constexpr int bindSearchPat4 = 4;
constexpr int bindSearchPat5 = 5;
constexpr int bindSearchPat6 = 6;
constexpr int bindSearchLimit = 7;
constexpr int defaultSearchLimit = 50;

// TU-local row mapper (alias for internal::fillTrackFromStmt).
static void fillTrackSearch(sqlite3_stmt* s, kanariv::types::Track& out) {
    internal::fillTrackFromStmt(s, out);
}

// TU-local single FTS5 MATCH query.
static auto tryFtsQuery(sqlite3* h, std::string_view query, int limit)
    -> std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error> {
    const char* sql =
        "SELECT t.id, t.fingerprint, t.path, t.deleted_at, t.size, t.mtime, t.duration, "
        "t.sample_rate, t.channels, t.bitrate, t.title, t.artist, t.album, t.album_artist, "
        "t.genre, t.year, t.track_num, t.disc_num, t.cover_art_path, t.rating, t.play_count, "
        "t.last_played, t.date_added, t.last_scanned, t.dirty, t.library_id, t.source_kind, "
        "t.source_key, t.source_available, t.source_display_name "
        "FROM tracks t JOIN tracks_fts f ON t.id = f.rowid WHERE tracks_fts MATCH ? ORDER BY rank "
        "LIMIT ?";
    SqliteStatement st;
    if (auto e = st.prepare(h, sql); !e) {
        return std::unexpected{e.error()};
    }
    st.bindText(1, query);
    st.bindInt(2, limit > 0 ? limit : defaultSearchLimit);
    std::vector<kanariv::types::Track> out;
    while (st.step()) {
        kanariv::types::Track t;
        fillTrackSearch(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

auto searchFts(contracts::IDatabase& db_, std::string_view query, int limit)
    -> std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error> {
    auto& db = dynamic_cast<Database&>(db_);
    if (query.empty()) {
        return std::vector<kanariv::types::Track>{};
    }
    std::string sanitized = internal::sanitizeFtsTerm(query);
    if (sanitized.empty()) {
        return std::vector<kanariv::types::Track>{};
    }
    std::string ftsQ = sanitized;
    std::shared_lock lock(DatabaseAccess::mutex(db));
    sqlite3* h = DatabaseAccess::handle(db);
    if (h == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    auto r = tryFtsQuery(h, ftsQ, limit);
    if (r && !r->empty()) {
        return r;
    }
    std::string prefix = sanitized + "*";
    auto r2 = tryFtsQuery(h, prefix, limit);
    if (r2 && !r2->empty()) {
        return r2;
    }
    std::string esc = internal::escapeLike(sanitized);
    std::string pat = "%" + esc + "%";
    const char* likeSql =
        "SELECT id, fingerprint, path, deleted_at, size, mtime, duration, sample_rate, channels, "
        "bitrate, title, artist, album, album_artist, genre, year, track_num, disc_num, "
        "cover_art_path, rating, play_count, last_played, date_added, last_scanned, dirty, "
        "library_id, source_kind, source_key, source_available, source_display_name "
        "FROM tracks WHERE title LIKE ? ESCAPE '\\' COLLATE NOCASE OR artist LIKE ? ESCAPE '\\' "
        "COLLATE NOCASE OR album LIKE ? ESCAPE '\\' COLLATE NOCASE OR album_artist LIKE ? ESCAPE "
        "'\\' COLLATE NOCASE OR genre LIKE ? ESCAPE '\\' COLLATE NOCASE OR path LIKE ? ESCAPE '\\' "
        "COLLATE NOCASE LIMIT ?";
    SqliteStatement st;
    if (auto e = st.prepare(h, likeSql); !e) {
        return std::unexpected{e.error()};
    }
    st.bindText(bindSearchPat1, pat);
    st.bindText(bindSearchPat2, pat);
    st.bindText(bindSearchPat3, pat);
    st.bindText(bindSearchPat4, pat);
    st.bindText(bindSearchPat5, pat);
    st.bindText(bindSearchPat6, pat);
    st.bindInt(bindSearchLimit, limit > 0 ? limit : defaultSearchLimit);
    std::vector<kanariv::types::Track> out;
    while (st.step()) {
        kanariv::types::Track t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

auto searchLike(contracts::IDatabase& db_, std::string_view query, int limit)
    -> std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error> {
    auto& db = dynamic_cast<Database&>(db_);
    if (query.empty()) {
        return std::vector<kanariv::types::Track>{};
    }
    std::string esc = internal::escapeLike(query);
    std::string pat = "%" + esc + "%";
    std::shared_lock lock(DatabaseAccess::mutex(db));
    sqlite3* h = DatabaseAccess::handle(db);
    if (h == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    }
    const char* likeSql =
        "SELECT id, fingerprint, path, deleted_at, size, mtime, duration, sample_rate, channels, "
        "bitrate, title, artist, album, album_artist, genre, year, track_num, disc_num, "
        "cover_art_path, rating, play_count, last_played, date_added, last_scanned, dirty, "
        "library_id, source_kind, source_key, source_available, source_display_name "
        "FROM tracks WHERE title LIKE ? ESCAPE '\\' COLLATE NOCASE OR artist LIKE ? ESCAPE '\\' "
        "COLLATE NOCASE OR album LIKE ? ESCAPE '\\' COLLATE NOCASE OR album_artist LIKE ? ESCAPE "
        "'\\' COLLATE NOCASE OR genre LIKE ? ESCAPE '\\' COLLATE NOCASE OR path LIKE ? ESCAPE '\\' "
        "COLLATE NOCASE LIMIT ?";
    SqliteStatement st;
    if (auto e = st.prepare(h, likeSql); !e) {
        return std::unexpected{e.error()};
    }
    st.bindText(bindSearchPat1, pat);
    st.bindText(bindSearchPat2, pat);
    st.bindText(bindSearchPat3, pat);
    st.bindText(bindSearchPat4, pat);
    st.bindText(bindSearchPat5, pat);
    st.bindText(bindSearchPat6, pat);
    st.bindInt(bindSearchLimit, limit > 0 ? limit : defaultSearchLimit);
    std::vector<kanariv::types::Track> out;
    while (st.step()) {
        kanariv::types::Track t;
        internal::fillTrackFromStmt(st.get(), t);
        out.push_back(std::move(t));
    }
    return out;
}

auto sanitizeFtsTerm(std::string_view term) -> std::string {
    return internal::sanitizeFtsTerm(term);
}

auto search(contracts::IDatabase& db_, std::string_view query, int limit)
    -> std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error> {
    auto& db = dynamic_cast<Database&>(db_);
    return searchFts(db, query, limit);
}

} // namespace kanariv::db
