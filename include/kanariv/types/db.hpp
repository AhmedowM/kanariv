#pragma once

/**
 * @file db.hpp
 * @brief Database value types shared across kanariv components.
 * @ingroup kanariv_types
 * @details Defines database rows, filters and aggregate statistics without
 * coupling consumers to the SQLite database implementation.
 * All are default-initialised aggregates; no invariants beyond those
 * documented per-field.
 */

#include <kanariv/types/record.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace kanariv::types {

/**
 * @brief Queue container row.
 * @ingroup kanariv_types
 * @details Maps to `queues`. Separate from `queue` (items) table.
 * The default id is zero until persisted; `library_id` defaults to the
 * built-in library (1). This is a value type only and performs no database
 * I/O.
 */
struct Queue {
    int64_t id{};          ///< Row id (PK).
    std::string name;      ///< Display name.
    int32_t repeat_mode{}; ///< Repeat mode (0 = off).
    bool temp{};           ///< Ephemeral direct-play queue (purged on shutdown).
    int64_t library_id{1}; ///< Owning library id.
};

/**
 * @brief Library (scan root) row.
 * @ingroup kanariv_types
 * @details Maps to `libraries`. Id 1 is the default library with empty path.
 * Timestamps are Unix milliseconds. `extensions` is the comma-separated
 * allow-list consumed by the scanner; callers should preserve the spelling
 * expected by their scan policy.
 */
struct Library {
    int64_t id{};                                   ///< Row id (PK).
    std::string path;                               ///< Root directory path (UNIQUE).
    std::string name;                               ///< Display name.
    int64_t date_added{};                           ///< Creation timestamp.
    int64_t last_scanned{};                         ///< Last scan timestamp.
    bool auto_scan{true};                           ///< Whether to auto-scan on startup.
    bool recursive{true};                           ///< Whether to scan recursively.
    std::string extensions{"mp3,flac,ogg,wav,m4a"}; ///< Comma-separated allowed extensions.
};

/**
 * @brief Aggregate database statistics.
 * @ingroup kanariv_types
 * @details Returned by `Database::getStats()`. Counts exclude soft-deleted tracks.
 * All counters describe the snapshot at one database read; they are not
 * updated live if the database changes after construction.
 */
struct PlaybackStats {
    int64_t num_tracks{};        ///< Number of live tracks.
    int64_t num_playlists{};     ///< Number of playlists.
    int64_t num_queue_items{};   ///< Number of queue items.
    int64_t num_history{};       ///< Number of history entries.
    int64_t num_bookmarks{};     ///< Number of bookmarks.
    int64_t num_libraries{};     ///< Number of libraries.
    int64_t total_duration_ms{}; ///< Sum of track durations in milliseconds.
};

/**
 * @brief Filter for listing tracks.
 * @ingroup kanariv_types
 * @details All fields are optional filters combined with AND. `search`
 * maps to a LIKE on title/artist/album/genre. `limit == 0` means no limit;
 * `offset` requires `LIMIT -1` when no limit is set. Range fields are
 * inclusive; omit either bound for a one-sided range.
 * @par Ownership
 * String filters are owned by the query object. Passing an empty string or
 * disengaged optional means that criterion is not applied.
 */
struct TrackQuery {
    std::string artist;                  ///< Exact artist filter (empty = no filter).
    std::string album;                   ///< Exact album filter.
    std::string genre;                   ///< Exact genre filter.
    std::optional<int32_t> year{};       ///< Exact year filter.
    std::optional<int32_t> year_min{};   ///< Minimum year (inclusive).
    std::optional<int32_t> year_max{};   ///< Maximum year (inclusive).
    std::optional<int64_t> library_id{}; ///< Exact library filter.
    std::optional<bool> dirty{};         ///< Dirty flag filter.
    std::optional<int32_t> rating{};     ///< Exact user rating filter (0..5).
    std::optional<int64_t> play_count_min{}; ///< Minimum play count (inclusive).
    std::optional<int64_t> play_count_max{}; ///< Maximum play count (inclusive).
    std::optional<double> duration_min{};    ///< Minimum duration in seconds (inclusive).
    std::optional<double> duration_max{};    ///< Maximum duration in seconds (inclusive).
    int limit{};                         ///< Max rows (0 = unlimited).
    int offset{};                        ///< Row offset.
    std::string search;                  ///< Substring search across title/artist/album/genre.
    std::string sort;                    ///< Sort field: id, title, artist, album, year,
                                          ///< rating, play_count, duration, date_added.
                                          ///< Empty = default (id).
    std::string order;                   ///< Sort direction: "asc" or "desc". Default: asc.
};

/**
 * @brief Filter for listing history entries.
 * @ingroup kanariv_types
 * @details Filters are combined with AND. A zero `limit` means no limit;
 * `offset` is applied by the database listing operation.
 * @par Ownership
 * The query owns all filter values and can be reused for another listing call.
 */
struct HistoryQuery {
    std::optional<int64_t> track_id{}; ///< Filter by track.
    std::optional<int64_t> queue_id{}; ///< Filter by queue.
    int limit{};                       ///< Max rows (0 = unlimited).
    int offset{};                      ///< Row offset.
};

} // namespace kanariv::types
