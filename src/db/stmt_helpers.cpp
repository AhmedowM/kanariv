#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <db/stmt_helpers.hpp>
#include <kanariv/types/db.hpp>
#include <string>
#include <string_view>

namespace kanariv::db::internal {

constexpr int colId = 0;
constexpr int colFingerprint = 1;
constexpr int colPath = 2;
constexpr int colDeletedAt = 3;
constexpr int colSize = 4;
constexpr int colMtime = 5;
constexpr int colDuration = 6;
constexpr int colSampleRate = 7;
constexpr int colChannels = 8;
constexpr int colBitrate = 9;
constexpr int colTitle = 10;
constexpr int colArtist = 11;
constexpr int colAlbum = 12;
constexpr int colAlbumArtist = 13;
constexpr int colGenre = 14;
constexpr int colYear = 15;
constexpr int colTrackNum = 16;
constexpr int colDiscNum = 17;
constexpr int colCoverArtPath = 18;
constexpr int colRating = 19;
constexpr int colPlayCount = 20;
constexpr int colLastPlayed = 21;
constexpr int colDateAdded = 22;
constexpr int colLastScanned = 23;
constexpr int colDirty = 24;
constexpr int colLibraryId = 25;
constexpr int colSourceKind = 26;
constexpr int colSourceKey = 27;
constexpr int colSourceAvailable = 28;
constexpr int colSourceDisplayName = 29;
constexpr int fingerprintSize = 32;

auto columnText(sqlite3_stmt* stmt, int col) noexcept -> std::string_view {
    const auto* p = sqlite3_column_text(stmt, col);
    return p != nullptr ? reinterpret_cast<const char*>(p) : "";
}

void fillTrackFromStmt(sqlite3_stmt* stmt, kanariv::types::Track& t) {
    t.id = sqlite3_column_int64(stmt, colId);
    t.fingerprint.fill(0);
    if (sqlite3_column_bytes(stmt, colFingerprint) == fingerprintSize) {
        std::memcpy(t.fingerprint.data(), sqlite3_column_blob(stmt, colFingerprint),
                    fingerprintSize);
    }
    t.path = columnText(stmt, colPath);
    t.deleted_at = sqlite3_column_int64(stmt, colDeletedAt);
    t.size = sqlite3_column_int64(stmt, colSize);
    t.mtime = sqlite3_column_int64(stmt, colMtime);
    t.duration = sqlite3_column_double(stmt, colDuration);
    t.sample_rate = static_cast<uint32_t>(sqlite3_column_int(stmt, colSampleRate));
    t.channels = static_cast<uint32_t>(sqlite3_column_int(stmt, colChannels));
    t.bitrate = static_cast<int>(sqlite3_column_int(stmt, colBitrate));
    t.title = columnText(stmt, colTitle);
    t.artist = columnText(stmt, colArtist);
    t.album = columnText(stmt, colAlbum);
    t.album_artist = columnText(stmt, colAlbumArtist);
    t.genre = columnText(stmt, colGenre);
    t.year = static_cast<int>(sqlite3_column_int(stmt, colYear));
    t.track_num = static_cast<int>(sqlite3_column_int(stmt, colTrackNum));
    t.disc_num = static_cast<int>(sqlite3_column_int(stmt, colDiscNum));
    t.cover_art_path = columnText(stmt, colCoverArtPath);
    t.rating = static_cast<int>(sqlite3_column_int(stmt, colRating));
    t.play_count = sqlite3_column_int64(stmt, colPlayCount);
    t.last_played = sqlite3_column_int64(stmt, colLastPlayed);
    t.date_added = sqlite3_column_int64(stmt, colDateAdded);
    t.last_scanned = sqlite3_column_int64(stmt, colLastScanned);
    t.dirty = sqlite3_column_int(stmt, colDirty) != 0;
    t.library_id = static_cast<int>(sqlite3_column_int(stmt, colLibraryId));
    t.source.kind =
        static_cast<kanariv::types::SourceKind>(sqlite3_column_int(stmt, colSourceKind));
    t.source.key = columnText(stmt, colSourceKey);
    t.source.available = sqlite3_column_int(stmt, colSourceAvailable) != 0;
    t.source.display_name = columnText(stmt, colSourceDisplayName);
}

SqliteErrGuard::~SqliteErrGuard() {
    if (ref != nullptr) {
        sqlite3_free(ref);
        ref = nullptr;
    }
}

StmtGuard::~StmtGuard() {
    if (s != nullptr) {
        sqlite3_finalize(s);
    }
}

} // namespace kanariv::db::internal
