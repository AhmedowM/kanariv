#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <db/stmt_helpers.hpp>
#include <kanariv/types/db.hpp>
#include <string>
#include <string_view>

namespace kanariv::db::internal {

std::string_view columnText(sqlite3_stmt* stmt, int col) noexcept {
    auto* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

kanariv::utils::Error sqliteError(sqlite3* db, int rc) {
    const int extended = sqlite3_extended_errcode(db);
    auto code = kanariv::utils::StatusCode::Internal;
    if (extended == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT)
        code = kanariv::utils::StatusCode::AlreadyExists;
    else if (extended == SQLITE_BUSY || extended == SQLITE_LOCKED)
        code = kanariv::utils::StatusCode::Busy;
    return kanariv::utils::makeError(code, sqlite3_errmsg(db));
}

void fillTrackFromStmt(sqlite3_stmt* stmt, kanariv::types::TrackRow& t) {
    t.id = sqlite3_column_int64(stmt, 0);
    t.fingerprint.fill(0);
    if (sqlite3_column_bytes(stmt, 1) == 32) {
        std::memcpy(t.fingerprint.data(), sqlite3_column_blob(stmt, 1), 32);
    }
    t.path = columnText(stmt, 2);
    t.size = sqlite3_column_int64(stmt, 3);
    t.mtime = sqlite3_column_int64(stmt, 4);
    t.duration = sqlite3_column_double(stmt, 5);
    t.sample_rate = static_cast<uint32_t>(sqlite3_column_int(stmt, 6));
    t.channels = static_cast<uint32_t>(sqlite3_column_int(stmt, 7));
    t.bitrate = static_cast<int>(sqlite3_column_int(stmt, 8));
    t.title = columnText(stmt, 9);
    t.artist = columnText(stmt, 10);
    t.album = columnText(stmt, 11);
    t.album_artist = columnText(stmt, 12);
    t.genre = columnText(stmt, 13);
    t.year = static_cast<int>(sqlite3_column_int(stmt, 14));
    t.track_num = static_cast<int>(sqlite3_column_int(stmt, 15));
    t.disc_num = static_cast<int>(sqlite3_column_int(stmt, 16));
    t.cover_art_path = columnText(stmt, 17);
    t.rating = static_cast<int>(sqlite3_column_int(stmt, 18));
    t.play_count = sqlite3_column_int64(stmt, 19);
    t.last_played = sqlite3_column_int64(stmt, 20);
    t.date_added = sqlite3_column_int64(stmt, 21);
    t.last_scanned = sqlite3_column_int64(stmt, 22);
    t.dirty = sqlite3_column_int(stmt, 23) != 0;
    t.library_id = static_cast<int>(sqlite3_column_int(stmt, 24));
    t.source.kind = static_cast<kanariv::types::SourceKind>(sqlite3_column_int(stmt, 25));
    t.source.key = columnText(stmt, 26);
    t.source.available = sqlite3_column_int(stmt, 27) != 0;
    t.source.display_name = columnText(stmt, 28);
    t.replayGainTrack = sqlite3_column_double(stmt, 29);
    t.replayGainAlbum = sqlite3_column_double(stmt, 30);
}

SqliteErrGuard::~SqliteErrGuard() {
    if (ref) {
        sqlite3_free(ref);
        ref = nullptr;
    }
}

StmtGuard::~StmtGuard() {
    if (s)
        sqlite3_finalize(s);
}

} // namespace kanariv::db::internal
