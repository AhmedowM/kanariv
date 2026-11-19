#pragma once

/**
 * @file db_impl.hpp
 * @brief Shared private state of `Database::Impl` for the db TUs.
 * @ingroup kanariv_db
 * @details Included by `core.cpp` (open/facade/stats), `track_repo.cpp`,
 * `playlist_repo.cpp`, `queue_repo.cpp` and `history_repo.cpp`. Splitting the
 * translation units moves code, not synchronization: every repo entry point
 * still takes `dbMutex_`, and the statement cache is only touched through
 * `getCachedForUse`.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <db/statement.hpp>
#include <expected>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
inline constexpr std::string_view updateReplayGainSql =
    "UPDATE tracks SET replay_gain_track=?, replay_gain_album=? WHERE id=?";

namespace internal {

/// `?` placeholders in `sql`; used to pin the two track statements to the
/// single binder's range so a column added to one side cannot drift.
constexpr std::size_t placeholderCount(std::string_view sql) noexcept {
    std::size_t count = 0;
    for (char c : sql)
        count += c == '?';
    return count;
}

} // namespace internal

static_assert(internal::placeholderCount(insertTrackSql) == 28);
static_assert(internal::placeholderCount(updateTrackSql) == 29);

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
    kanariv::utils::Expected<SqliteStatement*> getCachedForUse(std::string_view sql) const;
    sqlite3* handle() const noexcept;
    sqlite3* handleLocked() const noexcept;
    std::mutex& mutex() const noexcept;

    // TrackRow / library locked helpers
    kanariv::utils::Expected<kanariv::types::TrackRow> findByPathLocked(std::string_view path);
    kanariv::utils::Expected<kanariv::types::TrackRow>
    findBySourceLocked(const kanariv::types::SourceRef& source);
    kanariv::utils::Expected<kanariv::types::TrackRow>
    findByFingerprintLocked(const std::array<uint8_t, 32>& fp);
    kanariv::utils::Expected<int64_t> insertTrackLocked(const kanariv::types::TrackRow& t);
    kanariv::utils::Expected<void> updateTrackLocked(const kanariv::types::TrackRow& t);
    /// Writes only the two ReplayGain columns (0 = unanalyzed, see analyze).
    kanariv::utils::Expected<void> setReplayGainLocked(int64_t id, double trackGainDb,
                                                       double albumGainDb);
    kanariv::utils::Expected<void> deleteTrackLocked(int64_t id);
    kanariv::utils::Expected<kanariv::types::TrackRow> getTrackLocked(int64_t id);
    kanariv::utils::Expected<std::vector<kanariv::types::Library>> libraryListLocked();
    kanariv::utils::Expected<void> libraryUpdateLocked(const kanariv::types::Library& l);

    // Playlist / queue locked helpers
    kanariv::utils::Expected<int64_t> createPlaylistLocked(std::string_view name, int type = 0,
                                                           std::string_view smart_query = {});
    kanariv::utils::Expected<void> playlistAddTrackLocked(int64_t pid, int64_t tid,
                                                          int64_t pos = -1);
    kanariv::utils::Expected<void> queueEnqueueLocked(int64_t qid, int64_t tid,
                                                      int64_t index = appendIndex);
    kanariv::utils::Expected<kanariv::types::QueueItem> queueDequeueLocked(int64_t qid);
    std::size_t queueCountLocked(int64_t qid);
};

/**
 * @brief Binds one `TrackRow`'s 28 column values starting at `firstIndex`.
 * @details Feeds both `insertTrackSql` and `updateTrackSql` (whose first 28
 * parameters are the same column order) plus the JSON import. `date_added`
 * binds NULL when zero, which reads back as 0 through `columnInt` either way.
 */
void bindTrackColumns(SqliteStatement& st, const kanariv::types::TrackRow& t, int firstIndex);

} // namespace kanariv::db
