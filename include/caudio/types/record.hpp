#pragma once

/**
 * @file record.hpp
 * @brief Plain-data record types for the dependency-free `caudio.types` layer.
 * @ingroup caudio_types
 * @details Value structs shared across the db, engine, ipc and app layers:
 * Track, Playlist, QueueItem, HistoryEntry, Bookmark, DeviceInfo, DeviceList,
 * TrackMetadata and LibraryStats. All are default-initialised aggregates; no
 * invariants beyond those documented per-field.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace caudio::types {

/**
 * @enum SourceKind
 * @brief Durable identity category for a track's bytes.
 * @ingroup caudio_types
 *
 * This is deliberately separate from `caudio::media::SourceKind`, which
 * describes a read transport (path or descriptor). A persisted source may be
 * an ordinary path, a host-issued stable identifier (for example an Android
 * document URI or MediaStore id), an app-private imported copy, or a source
 * whose permission was revoked. A host reopens external identities and hands
 * the media layer a descriptor; descriptors themselves are never persisted.
 */
enum class SourceKind : std::uint8_t {
    path = 0,       ///< Ordinary filesystem path (legacy default).
    external = 1,   ///< Stable host-issued identifier, not a filesystem path.
    imported = 2,   ///< App-private copy created by the host.
    unavailable = 3 ///< Source is known but cannot currently be reopened.
};

/**
 * @struct SourceRef
 * @brief Persisted source identity and availability for a track.
 * @ingroup caudio_types
 *
 * `key` is the stable identity used for deduplication:
 * - `path`: the filesystem path (normalized from Track::path when empty).
 * - `external`: the host-issued document/media identifier.
 * - `imported`: the app-private path or host key for the imported copy.
 * - `unavailable`: the last known key, retained so the host can retry.
 *
 * Deduplication rule: `(kind, key)` identifies a source. `Track::path` remains
 * the display/legacy path and must not be used to fake a content URI. Hosts
 * should call `normalizedSource()` before persistence or lookup so path rows
 * always carry an explicit key.
 */
struct SourceRef {
    SourceKind kind{SourceKind::path}; ///< Durable identity category.
    std::string key;                   ///< Stable identity; empty only before normalization.
    std::string display_name;          ///< Optional user-facing name.
    bool available{true};              ///< False when the host cannot reopen the source.
};

/**
 * @brief Returns the stable serialized name of a source kind.
 * @ingroup caudio_types
 * @param kind Source kind.
 * @return `"path"`, `"external"`, `"imported"`, or `"unavailable"`.
 */
[[nodiscard]] inline constexpr std::string_view sourceKindName(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::path:
        return "path";
    case SourceKind::external:
        return "external";
    case SourceKind::imported:
        return "imported";
    case SourceKind::unavailable:
        return "unavailable";
    }
    return "path";
}

/**
 * @brief Parses a source kind from its serialized name.
 * @ingroup caudio_types
 * @param name Case-sensitive name produced by sourceKindName().
 * @return Parsed kind, or nullopt for an unknown name.
 */
[[nodiscard]] inline constexpr std::optional<SourceKind>
sourceKindFromName(std::string_view name) noexcept {
    if (name == "path")
        return SourceKind::path;
    if (name == "external")
        return SourceKind::external;
    if (name == "imported")
        return SourceKind::imported;
    if (name == "unavailable")
        return SourceKind::unavailable;
    return std::nullopt;
}

/**
 * @brief Represents a single audio track row.
 * @ingroup caudio_types
 * @details Maps 1:1 to the `tracks` table. `fingerprint` is a 32-byte
 * BLAKE3 digest; `deleted_at == 0` means not soft-deleted. `library_id`
 * defaults to 1 (the built-in default library).
 */
struct Track {
    int64_t id{};                          ///< Row id (PK, 0 = not yet persisted).
    int64_t size{};                        ///< File size in bytes.
    int64_t mtime{};                       ///< Last write time (filesystem epoch).
    double duration{};                     ///< Duration in seconds.
    int64_t play_count{};                  ///< Number of plays.
    int64_t last_played{};                 ///< Timestamp of last play (unix seconds).
    int64_t date_added{};                  ///< Timestamp when inserted.
    int64_t last_scanned{};                ///< Timestamp of last scan.
    int64_t deleted_at{};                  ///< Soft-delete timestamp (0 = live).
    int64_t library_id{1};                 ///< Owning library id (FK -> libraries.id).
    std::string path;                      ///< Absolute filesystem path (NOT NULL).
    std::string title;                     ///< Title tag.
    std::string artist;                    ///< Artist tag.
    std::string album;                     ///< Album tag.
    std::string album_artist;              ///< Album-artist tag.
    std::string genre;                     ///< Genre tag.
    std::string cover_art_path;            ///< Path to cover art file.
    uint32_t sample_rate{};                ///< Sample rate in Hz.
    uint32_t channels{};                   ///< Channel count.
    int32_t bitrate{};                     ///< Bitrate in bps.
    int32_t year{};                        ///< Year tag.
    int32_t track_num{};                   ///< Track number within disc.
    int32_t disc_num{};                    ///< Disc number.
    int32_t rating{};                      ///< User rating (0..5).
    std::array<uint8_t, 32> fingerprint{}; ///< BLAKE3-256 content fingerprint (UNIQUE).
    bool dirty{false};                     ///< True if metadata needs re-scan.
    SourceRef source{};                    ///< Durable source identity (path by default).
};

/**
 * @brief Returns a persistence-ready source reference for a track.
 * @ingroup caudio_types
 * @param t Track whose `source` may leave the key empty.
 * @return Copy of `t.source`; for `SourceKind::path` an empty key is filled
 *         from `t.path`. Other kinds are returned unchanged so callers can
 *         detect a missing host identifier.
 */
[[nodiscard]] inline SourceRef normalizedSource(const Track& t) {
    SourceRef source = t.source;
    if (source.kind == SourceKind::path && source.key.empty())
        source.key = t.path;
    if (source.display_name.empty() && source.kind == SourceKind::path)
        source.display_name = t.path;
    return source;
}

/**
 * @brief Playlist row.
 * @ingroup caudio_types
 * @details Maps to `playlists`. `type == 0` is a manual playlist,
 * non-zero values denote smart playlists whose `smart_query` holds
 * the filter expression.
 */
struct Playlist {
    int64_t id{};            ///< Row id (PK).
    std::string name;        ///< Display name (NOT NULL).
    int32_t type{};          ///< Playlist type (0 = manual).
    std::string smart_query; ///< Smart-playlist filter (empty for manual).
    int64_t created{};       ///< Creation timestamp.
    int64_t modified{};      ///< Last modification timestamp.
    int64_t library_id{1};   ///< Owning library id.
};

/**
 * @brief Single entry in a playback queue.
 * @ingroup caudio_types
 * @details Maps to `queue`. The table enforces `UNIQUE(queue_id, position)`
 * as a safety net; the single-writer invariant is maintained by
 * `Database::dbMutex_`.
 */
struct QueueItem {
    int64_t id{};        ///< Row id (PK).
    int64_t queue_id{1}; ///< Owning queue id (FK -> queues.id).
    int64_t track_id{};  ///< Referenced track id (FK -> tracks.id).
    int64_t position{};  ///< Zero-based position within the queue (UNIQUE per queue_id).
    int64_t added{};     ///< Timestamp when enqueued.
};

/**
 * @brief Playback history entry.
 * @ingroup caudio_types
 * @details First 7 fields map 1:1 to the `history` table row. The trailing
 * `title`/`artist`/`path`/`duration` are track snapshots populated only by
 * JOIN queries (`history` + `tracks`); they are empty on freshly marked
 * rows and ignored on insert. Single definition shared by db, engine
 * and service layers (the former `engine::HistoryEntry` duplicate is deleted).
 */
struct HistoryEntry {
    int64_t id{};            ///< Row id (PK).
    int64_t track_id{};      ///< Played track id.
    int64_t started_at{};    ///< Start timestamp.
    int64_t completed_at{};  ///< Completion timestamp (0 if not completed).
    int64_t position_ms{};   ///< Position reached in milliseconds.
    double completion_pct{}; ///< Completion percentage [0,100].
    int64_t queue_id{1};     ///< Queue context.
    std::string title{};     ///< Track title snapshot (JOIN queries only).
    std::string artist{};    ///< Track artist snapshot (JOIN queries only).
    std::string path{};      ///< Track path snapshot (JOIN queries only).
    double duration{};       ///< Track duration snapshot (JOIN queries only).
};

/**
 * @brief Bookmark / saved position.
 * @ingroup caudio_types
 * @details Maps to `bookmarks`.
 */
struct Bookmark {
    int64_t id{};          ///< Row id (PK).
    int64_t track_id{};    ///< Bookmarked track id.
    int64_t position_ms{}; ///< Saved position in milliseconds.
    std::string note;      ///< Optional user note.
    int64_t created{};     ///< Creation timestamp.
};

/**
 * @brief Audio playback device information.
 * @ingroup caudio_types
 * @details Canonical definition shared by the player enumeration and the IPC
 * `Devices` result.
 */
struct DeviceInfo {
    std::string id;        ///< Unique device identifier.
    std::string name;      ///< Human-readable device name.
    bool isDefault{false}; ///< True if this is the system default device.
};

/**
 * @brief Container for enumerated audio devices.
 * @ingroup caudio_types
 */
struct DeviceList {
    std::vector<DeviceInfo> devices; ///< Available playback devices.
};

/**
 * @brief Audio file metadata tags.
 * @ingroup caudio_types
 * @details Canonical definition shared by metadata extraction, scanning and
 * the db layer. Values mirror the decoded stream/tag data.
 */
struct TrackMetadata {
    std::string title;        ///< Title tag.
    std::string artist;       ///< Artist tag.
    std::string album;        ///< Album tag.
    std::string album_artist; ///< Album-artist tag.
    std::string genre;        ///< Genre tag.
    int year = 0;             ///< Year tag.
    int track_num = 0;        ///< Track number within disc.
    int disc_num = 0;         ///< Disc number.
    double duration = 0;      ///< Duration in seconds.
    int sample_rate = 0;      ///< Sample rate in Hz.
    int channels = 0;         ///< Channel count.
    int bitrate = 0;          ///< Bitrate in bps.
};

/**
 * @brief Top entity entry for listening reports (artist/album/genre).
 * @ingroup caudio_types
 * @details `name` is the entity name; `track_count` is how many distinct
 * tracks reference it; `total_plays` is the sum of play_count across those
 * tracks. For artists, multi-artist tracks are split so each individual
 * artist gets credited with the full play_count.
 */
struct TopEntity {
    std::string name{};            ///< Entity name (artist / album / genre).
    std::size_t track_count{0};    ///< Number of distinct tracks referencing it.
    std::int64_t total_plays{0};   ///< Sum of play_count across those tracks.
};

/**
 * @brief Library statistics with always-computed totals and optional top-N history.
 * @ingroup caudio_types
 * @details Canonical stats shape for `Database::libraryStats()` and the IPC
 * `LibraryStats` result. Totals are always populated; `top_tracks` is engaged
 * only when the caller asked for the top-N list, and an engaged empty vector
 * means "totals only" (top_tracks == 0). `queues` counts queue items.
 */
struct LibraryStats {
    std::size_t tracks{0};              ///< Total live tracks.
    std::size_t queues{0};              ///< Total queue items.
    std::size_t playlists{0};           ///< Total playlists.
    std::int64_t total_duration_ms{0};  ///< Sum of durations (ms).
    std::int64_t total_play_time_ms{0}; ///< Sum of play_count * duration (ms).
    /// Top tracks by total plays (play_count DESC); nullopt when not requested.
    std::optional<std::vector<Track>> top_tracks{};
    /// Top albums by total plays; nullopt when not requested.
    std::optional<std::vector<TopEntity>> top_albums{};
    /// Top genres by total plays; nullopt when not requested.
    std::optional<std::vector<TopEntity>> top_genres{};
    /// Top artists by total plays; nullopt when not requested.
    /// Multi-artist tracks are split so each individual artist is counted.
    std::optional<std::vector<TopEntity>> top_artists{};
};

} // namespace caudio::types
