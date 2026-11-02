#include <sqlite3.h>

#include <kanariv/types/db.hpp>
#include <cstdint>
#include <cstring>
#include <db/stmt_helpers.hpp>
#include <string>
#include <string_view>

namespace kanariv::db::internal {

std::string_view columnText(sqlite3_stmt* stmt, int col) noexcept {
    auto* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

void fillTrackFromStmt(sqlite3_stmt* stmt, kanariv::types::Track& t) {
    t.id = sqlite3_column_int64(stmt, 0);
    t.fingerprint.fill(0);
    if (sqlite3_column_bytes(stmt, 1) == 32) {
        std::memcpy(t.fingerprint.data(), sqlite3_column_blob(stmt, 1), 32);
    }
    t.path = columnText(stmt, 2);
    t.deleted_at = sqlite3_column_int64(stmt, 3);
    t.size = sqlite3_column_int64(stmt, 4);
    t.mtime = sqlite3_column_int64(stmt, 5);
    t.duration = sqlite3_column_double(stmt, 6);
    t.sample_rate = static_cast<uint32_t>(sqlite3_column_int(stmt, 7));
    t.channels = static_cast<uint32_t>(sqlite3_column_int(stmt, 8));
    t.bitrate = static_cast<int>(sqlite3_column_int(stmt, 9));
    t.title = columnText(stmt, 10);
    t.artist = columnText(stmt, 11);
    t.album = columnText(stmt, 12);
    t.album_artist = columnText(stmt, 13);
    t.genre = columnText(stmt, 14);
    t.year = static_cast<int>(sqlite3_column_int(stmt, 15));
    t.track_num = static_cast<int>(sqlite3_column_int(stmt, 16));
    t.disc_num = static_cast<int>(sqlite3_column_int(stmt, 17));
    t.cover_art_path = columnText(stmt, 18);
    t.rating = static_cast<int>(sqlite3_column_int(stmt, 19));
    t.play_count = sqlite3_column_int64(stmt, 20);
    t.last_played = sqlite3_column_int64(stmt, 21);
    t.date_added = sqlite3_column_int64(stmt, 22);
    t.last_scanned = sqlite3_column_int64(stmt, 23);
    t.dirty = sqlite3_column_int(stmt, 24) != 0;
    t.library_id = static_cast<int>(sqlite3_column_int(stmt, 25));
    t.source.kind = static_cast<kanariv::types::SourceKind>(sqlite3_column_int(stmt, 26));
    t.source.key = columnText(stmt, 27);
    t.source.available = sqlite3_column_int(stmt, 28) != 0;
    t.source.display_name = columnText(stmt, 29);
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
